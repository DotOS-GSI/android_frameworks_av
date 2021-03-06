/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "APM::Serializer"
//#define LOG_NDEBUG 0

#include <memory>
#include <string>
#include <utility>

#include <hidl/Status.h>
#include <libxml/parser.h>
#include <libxml/xinclude.h>
#include <media/convert.h>
#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <cutils/properties.h>
#include "Serializer.h"
#include "TypeConverter.h"

namespace android {

namespace {

static bool forceDisableA2dpOffload = false;

// TODO(mnaganov): Consider finding an alternative for using HIDL code.
using hardware::Return;
using hardware::Status;
using utilities::convertTo;

template<typename E, typename C>
struct AndroidCollectionTraits {
    typedef sp<E> Element;
    typedef C Collection;
    typedef void* PtrSerializingCtx;

    static status_t addElementToCollection(const Element &element, Collection *collection) {
        return collection->add(element) >= 0 ? NO_ERROR : BAD_VALUE;
    }
};

template<typename C>
struct StdCollectionTraits {
    typedef C Collection;
    typedef typename C::value_type Element;
    typedef void* PtrSerializingCtx;

    static status_t addElementToCollection(const Element &element, Collection *collection) {
        auto pair = collection->insert(element);
        return pair.second ? NO_ERROR : BAD_VALUE;
    }
};

struct AudioGainTraits : public AndroidCollectionTraits<AudioGain, AudioGains>
{
    static constexpr const char *tag = "gain";
    static constexpr const char *collectionTag = "gains";

    struct Attributes
    {
        /** gain modes supported, e.g. AUDIO_GAIN_MODE_CHANNELS. */
        static constexpr const char *mode = "mode";
        /** controlled channels, needed if mode AUDIO_GAIN_MODE_CHANNELS. */
        static constexpr const char *channelMask = "channel_mask";
        static constexpr const char *minValueMB = "minValueMB"; /**< min value in millibel. */
        static constexpr const char *maxValueMB = "maxValueMB"; /**< max value in millibel. */
        /** default value in millibel. */
        static constexpr const char *defaultValueMB = "defaultValueMB";
        static constexpr const char *stepValueMB = "stepValueMB"; /**< step value in millibel. */
        /** needed if mode AUDIO_GAIN_MODE_RAMP. */
        static constexpr const char *minRampMs = "minRampMs";
        /** needed if mode AUDIO_GAIN_MODE_RAMP. */
        static constexpr const char *maxRampMs = "maxRampMs";
        /** needed to allow use setPortGain instead of setStreamVolume. */
        static constexpr const char *useForVolume = "useForVolume";

    };

    static Return<Element> deserialize(const xmlNode *cur, PtrSerializingCtx serializingContext);
    // No children
};

// A profile section contains a name,  one audio format and the list of supported sampling rates
// and channel masks for this format
struct AudioProfileTraits : public AndroidCollectionTraits<AudioProfile, AudioProfileVector>
{
    static constexpr const char *tag = "profile";
    static constexpr const char *collectionTag = "profiles";

    struct Attributes
    {
        static constexpr const char *samplingRates = "samplingRates";
        static constexpr const char *format = "format";
        static constexpr const char *channelMasks = "channelMasks";
    };

    static Return<Element> deserialize(const xmlNode *cur, PtrSerializingCtx serializingContext);
};

struct MixPortTraits : public AndroidCollectionTraits<IOProfile, IOProfileCollection>
{
    static constexpr const char *tag = "mixPort";
    static constexpr const char *collectionTag = "mixPorts";

    struct Attributes
    {
        static constexpr const char *name = "name";
        static constexpr const char *role = "role";
        static constexpr const char *roleSource = "source"; /**< <attribute role source value>. */
        static constexpr const char *flags = "flags";
        static constexpr const char *maxOpenCount = "maxOpenCount";
        static constexpr const char *maxActiveCount = "maxActiveCount";
    };

    static Return<Element> deserialize(const xmlNode *cur, PtrSerializingCtx serializingContext);
    // Children: GainTraits
};

struct DevicePortTraits : public AndroidCollectionTraits<DeviceDescriptor, DeviceVector>
{
    static constexpr const char *tag = "devicePort";
    static constexpr const char *collectionTag = "devicePorts";

    struct Attributes
    {
        /**  <device tag name>: any string without space. */
        static constexpr const char *tagName = "tagName";
        static constexpr const char *type = "type"; /**< <device type>. */
        static constexpr const char *role = "role"; /**< <device role: sink or source>. */
        static constexpr const char *roleSource = "source"; /**< <attribute role source value>. */
        /** optional: device address, char string less than 64. */
        static constexpr const char *address = "address";
        /** optional: the list of encoded audio formats that are known to be supported. */
        static constexpr const char *encodedFormats = "encodedFormats";
    };

    static Return<Element> deserialize(const xmlNode *cur, PtrSerializingCtx serializingContext);
    // Children: GainTraits (optional)
};

struct RouteTraits : public AndroidCollectionTraits<AudioRoute, AudioRouteVector>
{
    static constexpr const char *tag = "route";
    static constexpr const char *collectionTag = "routes";

    struct Attributes
    {
        static constexpr const char *type = "type"; /**< <route type>: mix or mux. */
        static constexpr const char *typeMix = "mix"; /**< type attribute mix value. */
        static constexpr const char *sink = "sink"; /**< <sink: involved in this route>. */
        /** sources: all source that can be involved in this route. */
        static constexpr const char *sources = "sources";
    };

    typedef HwModule *PtrSerializingCtx;

    static Return<Element> deserialize(const xmlNode *cur, PtrSerializingCtx serializingContext);
};

struct ModuleTraits : public AndroidCollectionTraits<HwModule, HwModuleCollection>
{
    static constexpr const char *tag = "module";
    static constexpr const char *collectionTag = "modules";

    static constexpr const char *childAttachedDevicesTag = "attachedDevices";
    static constexpr const char *childAttachedDeviceTag = "item";
    static constexpr const char *childDefaultOutputDeviceTag = "defaultOutputDevice";

    struct Attributes
    {
        static constexpr const char *name = "name";
        static constexpr const char *version = "halVersion";
    };

    typedef AudioPolicyConfig *PtrSerializingCtx;

    static Return<Element> deserialize(const xmlNode *cur, PtrSerializingCtx serializingContext);
    // Children: mixPortTraits, devicePortTraits, and routeTraits
    // Need to call deserialize on each child
};

struct GlobalConfigTraits
{
    static constexpr const char *tag = "globalConfiguration";

    struct Attributes
    {
        static constexpr const char *speakerDrcEnabled = "speaker_drc_enabled";
        static constexpr const char *callScreenModeSupported= "call_screen_mode_supported";
        static constexpr const char *engineLibrarySuffix = "engine_library";
    };

    static status_t deserialize(const xmlNode *root, AudioPolicyConfig *config);
};

struct SurroundSoundTraits
{
    static constexpr const char *tag = "surroundSound";

    static status_t deserialize(const xmlNode *root, AudioPolicyConfig *config);
    // Children: SurroundSoundFormatTraits
};

struct SurroundSoundFormatTraits : public StdCollectionTraits<AudioPolicyConfig::SurroundFormats>
{
    static constexpr const char *tag = "format";
    static constexpr const char *collectionTag = "formats";

    struct Attributes
    {
        static constexpr const char *name = "name";
        static constexpr const char *subformats = "subformats";
    };

    static Return<Element> deserialize(const xmlNode *cur, PtrSerializingCtx serializingContext);
};

class PolicySerializer
{
public:
    PolicySerializer() : mVersion{std::to_string(gMajor) + "." + std::to_string(gMinor)}
    {
        ALOGV("%s: Version=%s Root=%s", __func__, mVersion.c_str(), rootName);
    }
    status_t deserialize(const char *configFile, AudioPolicyConfig *config);

private:
    static constexpr const char *rootName = "audioPolicyConfiguration";
    static constexpr const char *versionAttribute = "version";
    static constexpr uint32_t gMajor = 1; /**< the major number of the policy xml format version. */
    static constexpr uint32_t gMinor = 0; /**< the minor number of the policy xml format version. */

    typedef AudioPolicyConfig Element;

    const std::string mVersion;

    // Children: ModulesTraits, VolumeTraits, SurroundSoundTraits (optional)
};

template <class T>
constexpr void (*xmlDeleter)(T* t);
template <>
constexpr auto xmlDeleter<xmlDoc> = xmlFreeDoc;
// http://b/111067277 - Add back constexpr when we switch to C++17.
template <>
auto xmlDeleter<xmlChar> = [](xmlChar *s) { xmlFree(s); };

/** @return a unique_ptr with the correct deleter for the libxml2 object. */
template <class T>
constexpr auto make_xmlUnique(T *t) {
    // Wrap deleter in lambda to enable empty base optimization
    auto deleter = [](T *t) { xmlDeleter<T>(t); };
    return std::unique_ptr<T, decltype(deleter)>{t, deleter};
}

std::string getXmlAttribute(const xmlNode *cur, const char *attribute)
{
    auto xmlValue = make_xmlUnique(xmlGetProp(cur, reinterpret_cast<const xmlChar*>(attribute)));
    if (xmlValue == nullptr) {
        return "";
    }
    std::string value(reinterpret_cast<const char*>(xmlValue.get()));
    return value;
}

template <class Trait>
const xmlNode* getReference(const xmlNode *cur, const std::string &refName)
{
    for (; cur != NULL; cur = cur->next) {
        if (!xmlStrcmp(cur->name, reinterpret_cast<const xmlChar*>(Trait::collectionTag))) {
            for (const xmlNode *child = cur->children; child != NULL; child = child->next) {
                if ((!xmlStrcmp(child->name,
                                        reinterpret_cast<const xmlChar*>(Trait::referenceTag)))) {
                    std::string name = getXmlAttribute(child, Trait::Attributes::referenceName);
                    if (refName == name) {
                        return child;
                    }
                }
            }
        }
    }
    return NULL;
}

template <class Trait>
status_t deserializeCollection(const xmlNode *cur,
        typename Trait::Collection *collection,
        typename Trait::PtrSerializingCtx serializingContext)
{
    for (cur = cur->xmlChildrenNode; cur != NULL; cur = cur->next) {
        const xmlNode *child = NULL;
        if (!xmlStrcmp(cur->name, reinterpret_cast<const xmlChar*>(Trait::collectionTag))) {
            child = cur->xmlChildrenNode;
        } else if (!xmlStrcmp(cur->name, reinterpret_cast<const xmlChar*>(Trait::tag))) {
            child = cur;
        }
        for (; child != NULL; child = child->next) {
            if (!xmlStrcmp(child->name, reinterpret_cast<const xmlChar*>(Trait::tag))) {
                auto element = Trait::deserialize(child, serializingContext);
                if (element.isOk()) {
                    status_t status = Trait::addElementToCollection(element, collection);
                    if (status != NO_ERROR) {
                        ALOGE("%s: could not add element to %s collection", __func__,
                            Trait::collectionTag);
                        return status;
                    }
                } else {
                    ALOGE("Ignoring...");
                }
            }
        }
        if (!xmlStrcmp(cur->name, reinterpret_cast<const xmlChar*>(Trait::tag))) {
            return NO_ERROR;
        }
    }
    return NO_ERROR;
}

Return<AudioGainTraits::Element> AudioGainTraits::deserialize(const xmlNode *cur,
        PtrSerializingCtx /*serializingContext*/)
{
    static uint32_t index = 0;
    Element gain = new AudioGain(index++, true);

    std::string mode = getXmlAttribute(cur, Attributes::mode);
    if (!mode.empty()) {
        gain->setMode(GainModeConverter::maskFromString(mode));
    }

    std::string channelsLiteral = getXmlAttribute(cur, Attributes::channelMask);
    if (!channelsLiteral.empty()) {
        gain->setChannelMask(channelMaskFromString(channelsLiteral));
    }

    std::string minValueMBLiteral = getXmlAttribute(cur, Attributes::minValueMB);
    int32_t minValueMB;
    if (!minValueMBLiteral.empty() && convertTo(minValueMBLiteral, minValueMB)) {
        gain->setMinValueInMb(minValueMB);
    }

    std::string maxValueMBLiteral = getXmlAttribute(cur, Attributes::maxValueMB);
    int32_t maxValueMB;
    if (!maxValueMBLiteral.empty() && convertTo(maxValueMBLiteral, maxValueMB)) {
        gain->setMaxValueInMb(maxValueMB);
    }

    std::string defaultValueMBLiteral = getXmlAttribute(cur, Attributes::defaultValueMB);
    int32_t defaultValueMB;
    if (!defaultValueMBLiteral.empty() && convertTo(defaultValueMBLiteral, defaultValueMB)) {
        gain->setDefaultValueInMb(defaultValueMB);
    }

    std::string stepValueMBLiteral = getXmlAttribute(cur, Attributes::stepValueMB);
    uint32_t stepValueMB;
    if (!stepValueMBLiteral.empty() && convertTo(stepValueMBLiteral, stepValueMB)) {
        gain->setStepValueInMb(stepValueMB);
    }

    std::string minRampMsLiteral = getXmlAttribute(cur, Attributes::minRampMs);
    uint32_t minRampMs;
    if (!minRampMsLiteral.empty() && convertTo(minRampMsLiteral, minRampMs)) {
        gain->setMinRampInMs(minRampMs);
    }

    std::string maxRampMsLiteral = getXmlAttribute(cur, Attributes::maxRampMs);
    uint32_t maxRampMs;
    if (!maxRampMsLiteral.empty() && convertTo(maxRampMsLiteral, maxRampMs)) {
        gain->setMaxRampInMs(maxRampMs);
    }
    std::string useForVolumeLiteral = getXmlAttribute(cur, Attributes::useForVolume);
    bool useForVolume = false;
    if (!useForVolumeLiteral.empty() && convertTo(useForVolumeLiteral, useForVolume)) {
        gain->setUseForVolume(useForVolume);
    }
    ALOGV("%s: adding new gain mode %08x channel mask %08x min mB %d max mB %d UseForVolume: %d",
          __func__, gain->getMode(), gain->getChannelMask(), gain->getMinValueInMb(),
          gain->getMaxValueInMb(), useForVolume);

    if (gain->getMode() != 0) {
        return gain;
    } else {
        return Status::fromStatusT(BAD_VALUE);
    }
}

static bool fixedEarpieceChannels = false;
Return<AudioProfileTraits::Element> AudioProfileTraits::deserialize(const xmlNode *cur,
        PtrSerializingCtx serializingContext)
{
    bool isOutput = serializingContext != nullptr;
    std::string samplingRates = getXmlAttribute(cur, Attributes::samplingRates);
    std::string format = getXmlAttribute(cur, Attributes::format);
    std::string channels = getXmlAttribute(cur, Attributes::channelMasks);
    ChannelTraits::Collection channelsMask = channelMasksFromString(channels, ",");

    //Some Foxconn devices have wrong earpiece channel mask, leading to no channel mask
    if(channelsMask.size() == 1 && *channelsMask.begin() == AUDIO_CHANNEL_IN_MONO && isOutput) {
        fixedEarpieceChannels = true;
        channelsMask = channelMasksFromString("AUDIO_CHANNEL_OUT_MONO", ",");
    }

    Element profile = new AudioProfile(formatFromString(format, gDynamicFormat),
            channelsMask,
            samplingRatesFromString(samplingRates, ","));

    profile->setDynamicFormat(profile->getFormat() == gDynamicFormat);
    profile->setDynamicChannels(profile->getChannels().empty());
    profile->setDynamicRate(profile->getSampleRates().empty());

    return profile;
}

Return<MixPortTraits::Element> MixPortTraits::deserialize(const xmlNode *child,
        PtrSerializingCtx /*serializingContext*/)
{
    std::string name = getXmlAttribute(child, Attributes::name);
    if (name.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::name);
        return Status::fromStatusT(BAD_VALUE);
    }
    ALOGV("%s: %s %s=%s", __func__, tag, Attributes::name, name.c_str());
    std::string role = getXmlAttribute(child, Attributes::role);
    if (role.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::role);
        return Status::fromStatusT(BAD_VALUE);
    }
    ALOGV("%s: Role=%s", __func__, role.c_str());
    audio_port_role_t portRole = (role == Attributes::roleSource) ?
            AUDIO_PORT_ROLE_SOURCE : AUDIO_PORT_ROLE_SINK;

    Element mixPort = new IOProfile(name, portRole);

    AudioProfileTraits::Collection profiles;
    status_t status = deserializeCollection<AudioProfileTraits>(child, &profiles, NULL);
    if (status != NO_ERROR) {
        return Status::fromStatusT(status);
    }
    if (profiles.empty()) {
        profiles.add(AudioProfile::createFullDynamic(gDynamicFormat));
    }
    // The audio profiles are in order of listed in audio policy configuration file.
    // Sort audio profiles accroding to the format.
    sortAudioProfiles(profiles);
    mixPort->setAudioProfiles(profiles);

    std::string flags = getXmlAttribute(child, Attributes::flags);
    if (!flags.empty()) {
        // Source role
        if (portRole == AUDIO_PORT_ROLE_SOURCE) {
            mixPort->setFlags(OutputFlagConverter::maskFromString(flags));
        } else {
            // Sink role
            mixPort->setFlags(InputFlagConverter::maskFromString(flags));
        }
    }
    std::string maxOpenCount = getXmlAttribute(child, Attributes::maxOpenCount);
    if (!maxOpenCount.empty()) {
        convertTo(maxOpenCount, mixPort->maxOpenCount);
    }
    std::string maxActiveCount = getXmlAttribute(child, Attributes::maxActiveCount);
    if (!maxActiveCount.empty()) {
        convertTo(maxActiveCount, mixPort->maxActiveCount);
    }
    // Deserialize children
    AudioGainTraits::Collection gains;
    status = deserializeCollection<AudioGainTraits>(child, &gains, NULL);
    if (status != NO_ERROR) {
        return Status::fromStatusT(status);
    }
    mixPort->setGains(gains);

    return mixPort;
}

Return<DevicePortTraits::Element> DevicePortTraits::deserialize(const xmlNode *cur,
        PtrSerializingCtx /*serializingContext*/)
{
    std::string name = getXmlAttribute(cur, Attributes::tagName);
    if (name.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::tagName);
        return Status::fromStatusT(BAD_VALUE);
    }
    ALOGV("%s: %s %s=%s", __func__, tag, Attributes::tagName, name.c_str());
    std::string typeName = getXmlAttribute(cur, Attributes::type);
    if (typeName.empty()) {
        ALOGE("%s: no type for %s", __func__, name.c_str());
        return Status::fromStatusT(BAD_VALUE);
    }
    ALOGV("%s: %s %s=%s", __func__, tag, Attributes::type, typeName.c_str());
    std::string role = getXmlAttribute(cur, Attributes::role);
    if (role.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::role);
        return Status::fromStatusT(BAD_VALUE);
    }
    ALOGV("%s: %s %s=%s", __func__, tag, Attributes::role, role.c_str());
    audio_port_role_t portRole = (role == Attributes::roleSource) ?
                AUDIO_PORT_ROLE_SOURCE : AUDIO_PORT_ROLE_SINK;

    audio_devices_t type = AUDIO_DEVICE_NONE;
    if (!deviceFromString(typeName, type) ||
            (!audio_is_input_device(type) && portRole == AUDIO_PORT_ROLE_SOURCE) ||
            (!audio_is_output_devices(type) && portRole == AUDIO_PORT_ROLE_SINK)) {
        ALOGW("%s: bad type %08x", __func__, type);
        return Status::fromStatusT(BAD_VALUE);
    }
    std::string encodedFormatsLiteral = getXmlAttribute(cur, Attributes::encodedFormats);
    ALOGV("%s: %s %s=%s", __func__, tag, Attributes::encodedFormats, encodedFormatsLiteral.c_str());
    FormatVector encodedFormats;
    if (!encodedFormatsLiteral.empty()) {
        encodedFormats = formatsFromString(encodedFormatsLiteral, " ");
    }
    std::string address = getXmlAttribute(cur, Attributes::address);
    Element deviceDesc = new DeviceDescriptor(type, name, address, encodedFormats);

    AudioProfileTraits::Collection profiles;
    status_t status;
    if(audio_is_output_devices(type))
        status = deserializeCollection<AudioProfileTraits>(cur, &profiles, (PtrSerializingCtx)1);
    else
        status = deserializeCollection<AudioProfileTraits>(cur, &profiles, NULL);
    if (status != NO_ERROR) {
        return Status::fromStatusT(status);
    }
    if (profiles.empty()) {
        profiles.add(AudioProfile::createFullDynamic(gDynamicFormat));
    }
    // The audio profiles are in order of listed in audio policy configuration file.
    // Sort audio profiles accroding to the format.
    sortAudioProfiles(profiles);
    deviceDesc->setAudioProfiles(profiles);

    // Deserialize AudioGain children
    status = deserializeCollection<AudioGainTraits>(cur, &deviceDesc->mGains, NULL);
    if (status != NO_ERROR) {
        return Status::fromStatusT(status);
    }
    ALOGV("%s: adding device tag %s type %08x address %s", __func__,
          deviceDesc->getName().c_str(), type, deviceDesc->address().c_str());
    return deviceDesc;
}

char* trim(char * s) {
    int l = strlen(s);

    if (l > 0) {
      while (isspace(s[l - 1])) --l;
      while (*s && isspace(*s)) ++s, --l;
    }

    return strndup(s, l);
}

Return<RouteTraits::Element> RouteTraits::deserialize(const xmlNode *cur, PtrSerializingCtx ctx)
{
    std::string type = getXmlAttribute(cur, Attributes::type);
    if (type.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::type);
        return Status::fromStatusT(BAD_VALUE);
    }
    audio_route_type_t routeType = (type == Attributes::typeMix) ?
                AUDIO_ROUTE_MIX : AUDIO_ROUTE_MUX;

    ALOGV("%s: %s %s=%s", __func__, tag, Attributes::type, type.c_str());
    Element route = new AudioRoute(routeType);

    std::string sinkAttr = getXmlAttribute(cur, Attributes::sink);
    if (sinkAttr.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::sink);
        return Status::fromStatusT(BAD_VALUE);
    }
    // Convert Sink name to port pointer
    sp<PolicyAudioPort> sink = ctx->findPortByTagName(sinkAttr);
    if (sink == NULL) {
        ALOGE("%s: no sink found with name=%s", __func__, sinkAttr.c_str());
        return Status::fromStatusT(BAD_VALUE);
    }
    route->setSink(sink);

    std::string sourcesAttr = getXmlAttribute(cur, Attributes::sources);
    if (sourcesAttr.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::sources);
        return Status::fromStatusT(BAD_VALUE);
    }
    // Tokenize and Convert Sources name to port pointer
    PolicyAudioPortVector sources;
    std::unique_ptr<char[]> sourcesLiteral{strndup(
                sourcesAttr.c_str(), strlen(sourcesAttr.c_str()))};
    char *devTag = strtok(sourcesLiteral.get(), ",");
    while (devTag != NULL) {
        if (strlen(devTag) != 0) {
            sp<PolicyAudioPort> source = ctx->findPortByTagName(devTag);
            if (source == NULL) {
                source = ctx->findPortByTagName(trim(devTag));
                if (source == NULL) {
                    ALOGE("%s: no source found with name=%s", __func__, devTag);
                }
            }
            if(source != nullptr) {
                sources.add(source);
            }
        }
        devTag = strtok(NULL, ",");
    }

    sink->addRoute(route);
    for (size_t i = 0; i < sources.size(); i++) {
        sp<PolicyAudioPort> source = sources.itemAt(i);
        source->addRoute(route);
    }
    route->setSources(sources);
    return route;
}

static void fixupQualcommBtScoRoute(RouteTraits::Collection& routes, DevicePortTraits::Collection& devicePorts, HwModule* ctx) {
    // On many Qualcomm devices, there is a BT SCO Headset Mic => primary input mix
    // But Telephony Rx => BT SCO Headset route is missing
    // When we detect such case, add the missing route

    // If we have:
    // <route type="mix" sink="Telephony Tx" sources="voice_tx"/>
    // <route type="mix" sink="primary input" sources="Built-In Mic,Built-In Back Mic,Wired Headset Mic,BT SCO Headset Mic"/>
    // <devicePort tagName="BT SCO Headset" type="AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET" role="sink" />
    // And no <route type="mix" sink="BT SCO Headset" />

    // Add:
    // <route type="mix" sink="BT SCO Headset" sources="primary output,deep_buffer,compressed_offload,Telephony Rx"/>
    bool foundBtScoHeadsetDevice = false;
    for(const auto& device: devicePorts) {
        if(device->getTagName() == "BT SCO Headset") {
            foundBtScoHeadsetDevice = true;
            break;
        }
    }
    if(!foundBtScoHeadsetDevice) {
        ALOGE("No BT SCO Headset device found, don't patch policy");
        return;
    }

    bool foundTelephony = false;
    bool foundBtScoInput = false;
    bool foundScoHeadsetRoute = false;
    for(const auto& route: routes) {
        ALOGE("Looking at route %d\n", route->getType());
        if(route->getType() != AUDIO_ROUTE_MIX)
            continue;
        auto sink = route->getSink();
        ALOGE("... With sink %s\n", sink->getTagName().c_str());
        if(sink->getTagName() == "Telephony Tx") {
            foundTelephony = true;
            continue;
        }
        if(sink->getTagName() == "BT SCO Headset") {
            foundScoHeadsetRoute = true;
            break;
        }
        for(const auto& source: route->getSources()) {
            ALOGE("... With source %s\n", source->getTagName().c_str());
            if(source->getTagName() == "BT SCO Headset Mic") {
                foundBtScoInput = true;
                break;
            }
        }
    }
    //The route we want to add is already there
    ALOGE("Done looking for existing routes");
    if(foundScoHeadsetRoute)
        return;

    ALOGE("No existing route found... %d %d", foundTelephony ? 1 : 0, foundBtScoInput ? 1 : 0);
    //We couldn't find the routes we assume are required for the function we want to add
    if(!foundTelephony || !foundBtScoInput)
        return;
    ALOGE("Adding our own.");

    // Add:
    // <route type="mix" sink="BT SCO Headset" sources="primary output,deep_buffer,compressed_offload,Telephony Rx"/>
    AudioRoute *newRoute = new AudioRoute(AUDIO_ROUTE_MIX);

    auto sink = ctx->findPortByTagName("BT SCO Headset");
    ALOGE("Got sink %p\n", sink.get());
    newRoute->setSink(sink);

    Vector<sp<PolicyAudioPort>> sources;
    for(const auto& sourceName: {
            "primary output",
            "deep_buffer",
            "compressed_offload",
            "Telephony Rx"
            }) {
        auto source = ctx->findPortByTagName(sourceName);
        ALOGE("Got source %p\n", source.get());
        if (source.get() != nullptr) {
            sources.add(source);
            source->addRoute(newRoute);
        }
    }

    newRoute->setSources(sources);

    sink->addRoute(newRoute);

    auto ret = routes.add(newRoute);
    ALOGE("route add returned %zd", ret);
}

Return<ModuleTraits::Element> ModuleTraits::deserialize(const xmlNode *cur, PtrSerializingCtx ctx)
{
    std::string name = getXmlAttribute(cur, Attributes::name);
    if (name.empty()) {
        ALOGE("%s: No %s found", __func__, Attributes::name);
        return Status::fromStatusT(BAD_VALUE);
    }
    uint32_t versionMajor = 0, versionMinor = 0;
    std::string versionLiteral = getXmlAttribute(cur, Attributes::version);
    if (!versionLiteral.empty()) {
        sscanf(versionLiteral.c_str(), "%u.%u", &versionMajor, &versionMinor);
        ALOGV("%s: mHalVersion = major %u minor %u",  __func__,
              versionMajor, versionMajor);
    }

    ALOGV("%s: %s %s=%s", __func__, tag, Attributes::name, name.c_str());

    Element module = new HwModule(name.c_str(), versionMajor, versionMinor);

    bool isA2dpModule = strcmp(name.c_str(), "a2dp") == 0;
    bool isPrimaryModule = strcmp(name.c_str(), "primary") == 0;

    // Deserialize childrens: Audio Mix Port, Audio Device Ports (Source/Sink), Audio Routes
    MixPortTraits::Collection mixPorts;
    status_t status = deserializeCollection<MixPortTraits>(cur, &mixPorts, NULL);
    if (status != NO_ERROR) {
        return Status::fromStatusT(status);
    }
    if(forceDisableA2dpOffload && isA2dpModule) {
        for(const auto& mixPort: mixPorts) {
            ALOGE("Disable a2dp offload...? %s", mixPort->getTagName().c_str());
            //"a2dp" sw module already has a2dp out
            if(mixPort->getTagName() == "a2dp output") {
                forceDisableA2dpOffload = false;
                break;
            }
        }
    }
    if(forceDisableA2dpOffload && isA2dpModule) {
        //Add
        //<mixPort name="a2dp output" role="source"/>
        auto mixPort = new IOProfile("a2dp output", AUDIO_PORT_ROLE_SOURCE);
        AudioProfileTraits::Collection profiles;
        profiles.add(AudioProfile::createFullDynamic());
        mixPort->setAudioProfiles(profiles);
        mixPorts.push_back(mixPort);
    }
    module->setProfiles(mixPorts);

    DevicePortTraits::Collection devicePorts;
    status = deserializeCollection<DevicePortTraits>(cur, &devicePorts, NULL);
    if (status != NO_ERROR) {
        return Status::fromStatusT(status);
    }
    Vector<std::string> a2dpOuts;
    a2dpOuts.push_back("BT A2DP Out");
    a2dpOuts.push_back("BT A2DP Headphones");
    a2dpOuts.push_back("BT A2DP Speaker");
    if(forceDisableA2dpOffload) {
        if(isA2dpModule) {
            //<devicePort tagName="BT A2DP Out" type="AUDIO_DEVICE_OUT_BLUETOOTH_A2DP" role="sink" address="lhdc_a2dp">
            //  <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
            //      samplingRates="44100,48000,96000"
            //      channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
            //</devicePort>
            if(true) {
                FormatVector formats;
                //auto devicePortOut = new DeviceDescriptor(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, formats, "BT A2DP Out");
                auto devicePortOut = new DeviceDescriptor(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, "BT A2DP Out");
                AudioProfileTraits::Collection profiles;
                ChannelTraits::Collection channels;
                channels.insert(AUDIO_CHANNEL_OUT_STEREO);
                SampleRateSet sampleRates;
                sampleRates.insert(44100);
                sampleRates.insert(48000);
                sampleRates.insert(96000);
                auto profile = new AudioProfile(AUDIO_FORMAT_PCM_16_BIT, channels, sampleRates);
                profiles.add(profile);
                devicePortOut->setAudioProfiles(profiles);
                devicePortOut->setAddress("lhdc_a2dp");
                devicePorts.add(devicePortOut);
            }
            //<devicePort tagName="BT A2DP Headphones" type="AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES" role="sink" address="lhdc_a2dp">
            //  <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
            //      samplingRates="44100,48000,96000"
            //      channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
            //</devicePort>
            if(true) {
                FormatVector formats;
                auto devicePortOut = new DeviceDescriptor(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES, "BT A2DP Headphones");
                AudioProfileTraits::Collection profiles;
                ChannelTraits::Collection channels;
                channels.insert(AUDIO_CHANNEL_OUT_STEREO);
                SampleRateSet sampleRates;
                sampleRates.insert(44100);
                sampleRates.insert(48000);
                sampleRates.insert(96000);
                auto profile = new AudioProfile(AUDIO_FORMAT_PCM_16_BIT, channels, sampleRates);
                profiles.add(profile);
                devicePortOut->setAudioProfiles(profiles);
                devicePortOut->setAddress("lhdc_a2dp");
                devicePorts.add(devicePortOut);
            }
            //<devicePort tagName="BT A2DP Speaker" type="AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER" role="sink" address="lhdc_a2dp">
            //  <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
            //      samplingRates="44100,48000,96000"
            //      channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
            //</devicePort>
            if(true) {
                FormatVector formats;
                auto devicePortOut = new DeviceDescriptor(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER, "BT A2DP Speaker");
                AudioProfileTraits::Collection profiles;
                ChannelTraits::Collection channels;
                channels.insert(AUDIO_CHANNEL_OUT_STEREO);
                SampleRateSet sampleRates;
                sampleRates.insert(44100);
                sampleRates.insert(48000);
                sampleRates.insert(96000);
                auto profile = new AudioProfile(AUDIO_FORMAT_PCM_16_BIT, channels, sampleRates);
                profiles.add(profile);
                devicePortOut->setAudioProfiles(profiles);
                devicePortOut->setAddress("lhdc_a2dp");
                devicePorts.add(devicePortOut);

            }
        } else if(isPrimaryModule) {
            for(const auto& out: a2dpOuts) {
                auto iterA = std::find_if(devicePorts.begin(), devicePorts.end(), [out](const auto port) {
                        if(port->getTagName() == out) return true;
                        return false;
                        });
                if(iterA != devicePorts.end()) {
                    ALOGE("Erasing device port %s", (*iterA)->getTagName().c_str());
                    devicePorts.erase(iterA);
                }
            }
        }
    }
    module->setDeclaredDevices(devicePorts);

    RouteTraits::Collection routes;
    status = deserializeCollection<RouteTraits>(cur, &routes, module.get());
    if (status != NO_ERROR) {
        return Status::fromStatusT(status);
    }
    if(forceDisableA2dpOffload) {
        if(strcmp(name.c_str(), "primary") == 0) {
            for(const auto& out: a2dpOuts) {
                auto iterA = std::find_if(routes.begin(), routes.end(), [out](const auto route) {
                        if(route->getType() != AUDIO_ROUTE_MIX)
                        return false;
                        auto sink = route->getSink();
                        if(sink->getTagName() == out) {
                            return true;
                        }
                        return false;
                });
                if(iterA != routes.end()) {
                    auto sink = (*iterA)->getSink()->getTagName();
                    ALOGE("Erasing route %s", sink.c_str());
                    routes.erase(iterA);
                }
            }
        } else if(isA2dpModule) {
            //<route type="mix" sink="BT A2DP Out"
            //  sources="a2dp output"/>
            if(true) {
                auto newRoute = new AudioRoute(AUDIO_ROUTE_MIX);
                auto sink = module->findPortByTagName("BT A2DP Out");
                auto source = module->findPortByTagName("a2dp output");
                newRoute->setSink(sink);
                Vector<sp<PolicyAudioPort>> sources;
                sources.add(source);

                sink->addRoute(newRoute);
                source->addRoute(newRoute);
                newRoute->setSources(sources);

                routes.add(newRoute);
            }
            //<route type="mix" sink="BT A2DP Headphones"
            //  sources="a2dp output"/>
            if(true) {
                auto newRoute = new AudioRoute(AUDIO_ROUTE_MIX);
                auto sink = module->findPortByTagName("BT A2DP Headphones");
                auto source = module->findPortByTagName("a2dp output");
                newRoute->setSink(sink);
                Vector<sp<PolicyAudioPort>> sources;
                sources.add(source);

                sink->addRoute(newRoute);
                source->addRoute(newRoute);
                newRoute->setSources(sources);
                routes.add(newRoute);
            }
            //<route type="mix" sink="BT A2DP Speaker"
            //  sources="a2dp output"/>
            if(true) {
                auto newRoute = new AudioRoute(AUDIO_ROUTE_MIX);
                auto sink = module->findPortByTagName("BT A2DP Speaker");
                auto source = module->findPortByTagName("a2dp output");
                newRoute->setSink(sink);
                Vector<sp<PolicyAudioPort>> sources;
                sources.add(source);

                sink->addRoute(newRoute);
                source->addRoute(newRoute);
                newRoute->setSources(sources);
                routes.add(newRoute);
            }
        }
    }
    ALOGE("Good morning");
    fixupQualcommBtScoRoute(routes, devicePorts, module.get());
    ALOGE("Good morning2");
    module->setRoutes(routes);

    for (const xmlNode *children = cur->xmlChildrenNode; children != NULL;
         children = children->next) {
        if (!xmlStrcmp(children->name, reinterpret_cast<const xmlChar*>(childAttachedDevicesTag))) {
            ALOGV("%s: %s %s found", __func__, tag, childAttachedDevicesTag);
            for (const xmlNode *child = children->xmlChildrenNode; child != NULL;
                 child = child->next) {
                if (!xmlStrcmp(child->name,
                                reinterpret_cast<const xmlChar*>(childAttachedDeviceTag))) {
                    auto attachedDevice = make_xmlUnique(xmlNodeListGetString(
                                    child->doc, child->xmlChildrenNode, 1));
                    if (attachedDevice != nullptr) {
                        ALOGV("%s: %s %s=%s", __func__, tag, childAttachedDeviceTag,
                                reinterpret_cast<const char*>(attachedDevice.get()));
                        sp<DeviceDescriptor> device = module->getDeclaredDevices().
                                getDeviceFromTagName(std::string(reinterpret_cast<const char*>(
                                                        attachedDevice.get())));
                        if(device != nullptr) {
                            ctx->addDevice(device);
                        } else {
                            ALOGE("NULL DEVICE %s: %s %s=%s", __func__, tag, childAttachedDeviceTag,
                                    reinterpret_cast<const char*>(attachedDevice.get()));
                        }
                    }
                }
            }
        }
        if (!xmlStrcmp(children->name,
                        reinterpret_cast<const xmlChar*>(childDefaultOutputDeviceTag))) {
            auto defaultOutputDevice = make_xmlUnique(xmlNodeListGetString(
                            children->doc, children->xmlChildrenNode, 1));
            if (defaultOutputDevice != nullptr) {
                ALOGV("%s: %s %s=%s", __func__, tag, childDefaultOutputDeviceTag,
                        reinterpret_cast<const char*>(defaultOutputDevice.get()));
                sp<DeviceDescriptor> device = module->getDeclaredDevices().getDeviceFromTagName(
                        std::string(reinterpret_cast<const char*>(defaultOutputDevice.get())));
                if (device != 0 && ctx->getDefaultOutputDevice() == 0) {
                    ctx->setDefaultOutputDevice(device);
                    ALOGV("%s: default is %08x",
                            __func__, ctx->getDefaultOutputDevice()->type());
                }
            }
        }
    }

    if(fixedEarpieceChannels) {
        sp<DeviceDescriptor> device =
            module->getDeclaredDevices().getDeviceFromTagName("Earpiece");
        if(device != 0)
            ctx->addDevice(device);
        fixedEarpieceChannels = false;
    }
    return module;
}

status_t GlobalConfigTraits::deserialize(const xmlNode *root, AudioPolicyConfig *config)
{
    for (const xmlNode *cur = root->xmlChildrenNode; cur != NULL; cur = cur->next) {
        if (!xmlStrcmp(cur->name, reinterpret_cast<const xmlChar*>(tag))) {
            bool value;
            std::string attr = getXmlAttribute(cur, Attributes::speakerDrcEnabled);
            if (!attr.empty() &&
                    convertTo<std::string, bool>(attr, value)) {
                config->setSpeakerDrcEnabled(value);
            }
            attr = getXmlAttribute(cur, Attributes::callScreenModeSupported);
            if (!attr.empty() &&
                    convertTo<std::string, bool>(attr, value)) {
                config->setCallScreenModeSupported(value);
            }
            std::string engineLibrarySuffix = getXmlAttribute(cur, Attributes::engineLibrarySuffix);
            if (!engineLibrarySuffix.empty()) {
                config->setEngineLibraryNameSuffix(engineLibrarySuffix);
            }
            return NO_ERROR;
        }
    }
    return NO_ERROR;
}

status_t SurroundSoundTraits::deserialize(const xmlNode *root, AudioPolicyConfig *config)
{
    config->setDefaultSurroundFormats();

    for (const xmlNode *cur = root->xmlChildrenNode; cur != NULL; cur = cur->next) {
        if (!xmlStrcmp(cur->name, reinterpret_cast<const xmlChar*>(tag))) {
            AudioPolicyConfig::SurroundFormats formats;
            status_t status = deserializeCollection<SurroundSoundFormatTraits>(
                    cur, &formats, nullptr);
            if (status == NO_ERROR) {
                config->setSurroundFormats(formats);
            }
            return NO_ERROR;
        }
    }
    return NO_ERROR;
}

Return<SurroundSoundFormatTraits::Element> SurroundSoundFormatTraits::deserialize(
        const xmlNode *cur, PtrSerializingCtx /*serializingContext*/)
{
    std::string formatLiteral = getXmlAttribute(cur, Attributes::name);
    if (formatLiteral.empty()) {
        ALOGE("%s: No %s found for a surround format", __func__, Attributes::name);
        return Status::fromStatusT(BAD_VALUE);
    }
    audio_format_t format = formatFromString(formatLiteral);
    if (format == AUDIO_FORMAT_DEFAULT) {
        ALOGE("%s: Unrecognized format %s", __func__, formatLiteral.c_str());
        return Status::fromStatusT(BAD_VALUE);
    }
    Element pair = std::make_pair(format, Collection::mapped_type{});

    std::string subformatsLiteral = getXmlAttribute(cur, Attributes::subformats);
    if (subformatsLiteral.empty()) return pair;
    FormatVector subformats = formatsFromString(subformatsLiteral, " ");
    for (const auto& subformat : subformats) {
        auto result = pair.second.insert(subformat);
        if (!result.second) {
            ALOGE("%s: could not add subformat %x to collection", __func__, subformat);
            return Status::fromStatusT(BAD_VALUE);
        }
    }
    return pair;
}

status_t PolicySerializer::deserialize(const char *configFile, AudioPolicyConfig *config)
{
    auto doc = make_xmlUnique(xmlParseFile(configFile));
    if (doc == nullptr) {
        ALOGE("%s: Could not parse %s document.", __func__, configFile);
        return BAD_VALUE;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc.get());
    if (root == NULL) {
        ALOGE("%s: Could not parse %s document: empty.", __func__, configFile);
        return BAD_VALUE;
    }
    if (xmlXIncludeProcess(doc.get()) < 0) {
        ALOGE("%s: libxml failed to resolve XIncludes on %s document.", __func__, configFile);
    }

    if (xmlStrcmp(root->name, reinterpret_cast<const xmlChar*>(rootName)))  {
        ALOGE("%s: No %s root element found in xml data %s.", __func__, rootName,
                reinterpret_cast<const char*>(root->name));
        return BAD_VALUE;
    }

    std::string version = getXmlAttribute(root, versionAttribute);
    if (version.empty()) {
        ALOGE("%s: No version found in root node %s", __func__, rootName);
        return BAD_VALUE;
    }
    if (version != mVersion) {
        ALOGE("%s: Version does not match; expect %s got %s", __func__, mVersion.c_str(),
              version.c_str());
        return BAD_VALUE;
    }
    // Lets deserialize children
    // Modules
    ModuleTraits::Collection modules;
    status_t status = deserializeCollection<ModuleTraits>(root, &modules, config);
    if (status != NO_ERROR) {
        return status;
    }
    config->setHwModules(modules);

    // Global Configuration
    GlobalConfigTraits::deserialize(root, config);

    // Surround configuration
    SurroundSoundTraits::deserialize(root, config);

    return android::OK;
}

}  // namespace

status_t deserializeAudioPolicyFile(const char *fileName, AudioPolicyConfig *config)
{
    PolicySerializer serializer;
    forceDisableA2dpOffload = property_get_bool("persist.sys.phh.disable_a2dp_offload", false);
    return serializer.deserialize(fileName, config);
}

} // namespace android
