#include "RenderHarness.h"

#include "ParamIds.h"

#include <CoreFoundation/CoreFoundation.h>

namespace clickmaker::test
{

namespace
{
constexpr int WORKER_TIMEOUT_MS = 10000;
constexpr int PUBLISH_SETTLE_MS = 500;
constexpr int UNLOAD_SETTLE_MS = 200;
constexpr float SAME_OUTPUT_TOLERANCE = 1.0e-5f;
const auto PLUGIN_STATE_KEY = CFSTR ("jucePluginState"); // where JUCE's AU wrapper keeps the plugin's own state

struct CFRelease
{
    void operator() (CFTypeRef object) const { if (object != nullptr) ::CFRelease (object); }
};

template <typename CFType>
using CFOwned = std::unique_ptr<std::remove_pointer_t<CFType>, CFRelease>;

// A hosted AU takes its state as an AU ClassInfo property list, which is also what Logic saves in projects.
juce::MemoryBlock wrapInClassInfo (juce::AudioPluginInstance& instance, const juce::MemoryBlock& pluginState)
{
    juce::MemoryBlock classInfo;
    instance.getStateInformation (classInfo);

    const CFOwned<CFDataRef> source (CFDataCreate (kCFAllocatorDefault, (const UInt8*) classInfo.getData(), (CFIndex) classInfo.getSize()));
    const CFOwned<CFPropertyListRef> list (CFPropertyListCreateWithData (kCFAllocatorDefault, source.get(),
                                                                                    kCFPropertyListMutableContainersAndLeaves, nullptr, nullptr));
    const CFOwned<CFDataRef> blob (CFDataCreate (kCFAllocatorDefault, (const UInt8*) pluginState.getData(), (CFIndex) pluginState.getSize()));
    CFDictionarySetValue ((CFMutableDictionaryRef) list.get(), PLUGIN_STATE_KEY, blob.get());

    const CFOwned<CFDataRef> wrapped (CFPropertyListCreateData (kCFAllocatorDefault, list.get(), kCFPropertyListBinaryFormat_v1_0, 0, nullptr));
    return { CFDataGetBytePtr (wrapped.get()), (size_t) CFDataGetLength (wrapped.get()) };
}

juce::MemoryBlock unwrapClassInfo (juce::AudioPluginInstance& instance)
{
    juce::MemoryBlock classInfo;
    instance.getStateInformation (classInfo);

    const CFOwned<CFDataRef> source (CFDataCreate (kCFAllocatorDefault, (const UInt8*) classInfo.getData(), (CFIndex) classInfo.getSize()));
    const CFOwned<CFPropertyListRef> list (CFPropertyListCreateWithData (kCFAllocatorDefault, source.get(),
                                                                                    kCFPropertyListImmutable, nullptr, nullptr));
    const auto blob = (CFDataRef) CFDictionaryGetValue ((CFDictionaryRef) list.get(), PLUGIN_STATE_KEY);
    return blob != nullptr ? juce::MemoryBlock (CFDataGetBytePtr (blob), (size_t) CFDataGetLength (blob)) : juce::MemoryBlock();
}
} // namespace

// Loads the installed .component the way a host does (host callbacks for tempo, meter and
// transport, MusicDeviceMIDIEvent, ClassInfo state) and checks it renders what the processor renders.
class AUHostedTests final : public juce::UnitTest
{
public:
    AUHostedTests() : juce::UnitTest ("AU round trip", "AUHosted") {}

    void runTest() override
    {
        beginTest ("The installed component renders exactly like the processor");

        juce::AudioUnitPluginFormat format;
        juce::OwnedArray<juce::PluginDescription> found;
        format.findAllTypesForFile (found, CLICKMAKER_AU_IDENTIFIER);
        expectEquals (found.size(), 1, "build ClickMaker_AU and refresh the AU registry first");

        if (found.isEmpty())
            return;

        juce::String error;
        auto instance = format.createInstanceFromDescription (*found[0], TEST_SAMPLE_RATE, 512, error);
        expect (instance != nullptr, error);

        if (instance == nullptr)
            return;

        // Synthetic words travel inside the state, so neither side needs speech synthesis.
        auto reference = std::make_unique<ClickMakerProcessor> (fakeSpeech);
        expect (waitUntil ([&] { return reference->speechStatus().complete; }, WORKER_TIMEOUT_MS));
        setParameter (*reference, param::CLICK_PAN, -1.0f);
        setParameter (*reference, param::CUE_PAN, 1.0f);
        setParameter (*reference, param::CLICK_GRID, 1.0f);
        setParameter (*reference, param::CLICK_SOUND, 1.0f);

        juce::MemoryBlock state;
        reference->getStateInformation (state);
        const auto classInfo = wrapInClassInfo (*instance, state);
        instance->setStateInformation (classInfo.getData(), (int) classInfo.getSize());

        // What Logic would save back into a project restores the same settings.
        const auto saved = unwrapClassInfo (*instance);
        ClickMakerProcessor reloaded (fakeSpeech);
        reloaded.setStateInformation (saved.getData(), (int) saved.getSize());
        expect (reloaded.settings() == reference->settings(), "settings changed through the AU ClassInfo round trip");

        ClickMakerProcessor direct (fakeSpeech);
        direct.setStateInformation (state.getData(), (int) state.getSize());
        expect (waitUntil ([&] { return direct.speechStatus().complete; }, WORKER_TIMEOUT_MS));
        waitUntil ([] { return false; }, PUBLISH_SETTLE_MS); // let the hosted copy publish its restored words

        expectEquals (instance->getBusCount (false), 2);

        Scenario scenario;
        scenario.name = "au_round_trip";
        scenario.bpmAt = [] (double) { return 137.0; };
        scenario.meters = { { 0.0, { 7, 8 } } };
        scenario.lengthPpq = 14.0;
        scenario.notes = { { 3.5, FIRST_SLOT_NOTE + 5 } };

        const auto viaAU = renderScenario (*instance, scenario);
        scenario.name = "au_round_trip_direct";
        const auto viaDirect = renderScenario (direct, scenario);

        const int channels = std::min (viaAU.audio.getNumChannels(), viaDirect.audio.getNumChannels());
        expect (channels >= 2);
        expectEquals (viaAU.audio.getNumSamples(), viaDirect.audio.getNumSamples());

        float difference = 0.0f;
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < viaDirect.audio.getNumSamples(); ++i)
                difference = std::max (difference, std::abs (viaAU.audio.getSample (c, i) - viaDirect.audio.getSample (c, i)));

        expect (difference < SAME_OUTPUT_TOLERANCE, "max difference " + juce::String (difference));
        expect (viaAU.audio.getMagnitude (LEFT, 0, viaAU.audio.getNumSamples()) > 0.1f, "no click through the AU");
        expect (viaAU.audio.getMagnitude (RIGHT, 0, viaAU.audio.getNumSamples()) > 0.1f, "no cue through the AU");

        instance.reset();
        waitUntil ([] { return false; }, UNLOAD_SETTLE_MS); // JUCE timers may still post while unloading
    }
};

static AUHostedTests auHostedTests;

} // namespace clickmaker::test
