#pragma once

#include "ClickEngine.h"
#include "CueEngine.h"
#include "SpeechWorker.h"

namespace clickmaker
{

struct BeatDisplay
{
    int beat = -1; // -1 while stopped
    int beatsPerBar = 4;
    int cueSlot = NO_SLOT;

    bool operator== (const BeatDisplay&) const = default;
};

class ClickMakerProcessor final : public juce::AudioProcessor
{
public:
    // Tests pass a fake renderer so results do not depend on the installed voices.
    explicit ClickMakerProcessor (SpeechRenderer renderer = renderSpeech);

    const juce::String getName() const override { return "ClickMaker"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    Settings settings() const { return speechWorker.settings(); }
    std::uint64_t setSettings (Settings next) { return speechWorker.setSettings (std::move (next)); }
    std::uint64_t settingsRevision() const { return speechWorker.revision(); }
    std::pair<Settings, std::uint64_t> settingsSnapshot() const { return speechWorker.snapshot(); }
    SpeechStatus speechStatus() const { return speechWorker.status(); }
    void requestPreview (int slot) { previewRequest.store (slot); }
    BeatDisplay beatDisplay() const;

    juce::AudioProcessorValueTreeState parameters;

private:
    struct StereoGain
    {
        float left = 0.0f;
        float right = 0.0f;
    };

    struct BlockSettings
    {
        GridOptions grid;
        bool compound = true;
        ClickSound sound;
        float clickGain = 1.0f;
        float clickPan = 0.0f;
        float cueGain = 1.0f;
        float cuePan = 0.0f;
        int defaultCountBars = 1;
        CountDirection direction = CountDirection::down;
        bool cueToAux = false;
    };

    struct ParameterValues
    {
        std::atomic<float>* clickOn = nullptr;
        std::atomic<float>* clickLevel = nullptr;
        std::atomic<float>* clickPan = nullptr;
        std::atomic<float>* clickSound = nullptr;
        std::atomic<float>* clickGrid = nullptr;
        std::atomic<float>* swingOn = nullptr;
        std::atomic<float>* swingAmount = nullptr;
        std::atomic<float>* accentOn = nullptr;
        std::atomic<float>* compound = nullptr;
        std::atomic<float>* accentPitch = nullptr;
        std::atomic<float>* beatPitch = nullptr;
        std::atomic<float>* subPitch = nullptr;
        std::atomic<float>* accentGain = nullptr;
        std::atomic<float>* beatGain = nullptr;
        std::atomic<float>* subGain = nullptr;
        std::atomic<float>* clickDecay = nullptr;
        std::atomic<float>* cueOn = nullptr;
        std::atomic<float>* cueLevel = nullptr;
        std::atomic<float>* cuePan = nullptr;
        std::atomic<float>* countLength = nullptr;
        std::atomic<float>* countDirection = nullptr;
        std::atomic<float>* cueToAux = nullptr;
    };

    BlockSettings readBlockSettings() const;
    static CueShape shapeFor (int slot, const EngineConfig*, const BlockSettings&);
    static StereoGain panGains (float gain, float pan);
    static void mix (juce::AudioBuffer<float>& destination, const float* source, int numSamples,
                     StereoGain& last, StereoGain next);
    void updateBeatDisplay (const BlockTime&, const BarLayout&);

    // Declaration order matters: the worker (publisher) must stop before the handoff frees configs.
    ConfigHandoff configHandoff;
    SpeechWorker speechWorker;

    ParameterValues values;
    TransportClock clock;
    ClickEngine click;
    CueEngine cue;
    TickCursor tickCursor;
    juce::AudioBuffer<float> scratch; // channel 0 click, channel 1 cue
    StereoGain lastClickGain;
    StereoGain lastCueGain;
    std::array<Tick, MAX_TICKS_PER_BLOCK> ticks {};

    std::atomic<int> previewRequest { NO_SLOT };
    std::atomic<int> displayBeat { -1 };
    std::atomic<int> displayBeatsPerBar { 4 };
    std::atomic<int> displayCueSlot { NO_SLOT };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClickMakerProcessor)
};

} // namespace clickmaker
