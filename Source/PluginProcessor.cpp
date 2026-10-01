#include "PluginProcessor.h"

#include "ParamIds.h"
#include "PluginEditor.h"

namespace clickmaker
{

namespace
{
const juce::Identifier STATE_TYPE ("ClickMakerState");
const juce::Identifier VERSION_PROPERTY ("version");
constexpr int STATE_VERSION = 1;
constexpr int PARAMETER_VERSION = 1;
constexpr int MIN_SCRATCH_SAMPLES = 4096;
constexpr int CLICK_CHANNEL = 0;
constexpr int CUE_CHANNEL = 1;
constexpr int MAIN_BUS = 0;
constexpr int CUE_BUS = 1;
constexpr double BEAT_DISPLAY_EPSILON = 1.0e-6;

juce::String panToText (float value, int)
{
    const int percent = juce::roundToInt (std::abs (value) * 100.0f);
    return percent == 0 ? juce::String ("C") : juce::String (value < 0.0f ? "L" : "R") + juce::String (percent);
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto toggle = [&layout] (const char* id, const char* name, bool initial)
    {
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, PARAMETER_VERSION }, name, initial));
    };
    const auto choice = [&layout] (const char* id, const char* name, const StringArray& options, int initial)
    {
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, PARAMETER_VERSION }, name, options, initial));
    };
    const auto ranged = [&layout] (const char* id, const char* name, NormalisableRange<float> range, float initial, const char* unit)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, PARAMETER_VERSION }, name, range, initial,
                                                           AudioParameterFloatAttributes().withLabel (unit)));
    };
    const auto pan = [&layout] (const char* id, const char* name)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, PARAMETER_VERSION }, name,
                                                           NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f,
                                                           AudioParameterFloatAttributes().withStringFromValueFunction (panToText)));
    };
    const auto pitchRange = []
    {
        NormalisableRange<float> range (200.0f, 4000.0f, 1.0f);
        range.setSkewForCentre (1000.0f);
        return range;
    };

    toggle (param::CLICK_ON, "Click On", true);
    ranged (param::CLICK_LEVEL, "Click Level", { -48.0f, 6.0f, 0.1f }, -6.0f, "dB");
    pan (param::CLICK_PAN, "Click Pan");
    choice (param::CLICK_SOUND, "Click Sound", { "Beep", "Wood", "Cowbell" }, 0);
    choice (param::CLICK_GRID, "Click Grid", { "Beat", "1/8", "1/8T", "1/16", "1/16T" }, 0);
    toggle (param::SWING_ON, "Swing On", false);
    ranged (param::SWING_AMOUNT, "Swing Amount", { 50.0f, 75.0f, 0.1f }, 66.7f, "%");
    toggle (param::ACCENT_ON, "Accent On", true);
    toggle (param::COMPOUND, "Compound Beats", true);
    ranged (param::ACCENT_PITCH, "Accent Pitch", pitchRange(), 1600.0f, "Hz");
    ranged (param::BEAT_PITCH, "Beat Pitch", pitchRange(), 1000.0f, "Hz");
    ranged (param::SUB_PITCH, "Sub Pitch", pitchRange(), 800.0f, "Hz");
    ranged (param::ACCENT_GAIN, "Accent Gain", { -24.0f, 0.0f, 0.1f }, 0.0f, "dB");
    ranged (param::BEAT_GAIN, "Beat Gain", { -24.0f, 0.0f, 0.1f }, -3.0f, "dB");
    ranged (param::SUB_GAIN, "Sub Gain", { -24.0f, 0.0f, 0.1f }, -12.0f, "dB");
    ranged (param::CLICK_DECAY, "Click Decay", { 5.0f, 200.0f, 1.0f, 0.5f }, 40.0f, "ms");
    toggle (param::CUE_ON, "Cue On", true);
    ranged (param::CUE_LEVEL, "Cue Level", { -48.0f, 6.0f, 0.1f }, -3.0f, "dB");
    pan (param::CUE_PAN, "Cue Pan");
    choice (param::COUNT_LENGTH, "Count Length", { "4 (1 bar)", "8 (2 bars)" }, 0);
    choice (param::COUNT_DIRECTION, "Count Direction", { "Down 4 3 2 1", "Up 1 2 3 4" }, 0);
    toggle (param::CUE_TO_AUX, "Cue To Aux Out", false);
    return layout;
}
} // namespace

ClickMakerProcessor::ClickMakerProcessor (SpeechRenderer renderer)
    : AudioProcessor (BusesProperties()
                          .withOutput ("Main", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Cue", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "PARAMETERS", createParameterLayout()),
      speechWorker (configHandoff, std::move (renderer))
{
    const auto raw = [this] (const char* id) { return parameters.getRawParameterValue (id); };

    values.clickOn = raw (param::CLICK_ON);
    values.clickLevel = raw (param::CLICK_LEVEL);
    values.clickPan = raw (param::CLICK_PAN);
    values.clickSound = raw (param::CLICK_SOUND);
    values.clickGrid = raw (param::CLICK_GRID);
    values.swingOn = raw (param::SWING_ON);
    values.swingAmount = raw (param::SWING_AMOUNT);
    values.accentOn = raw (param::ACCENT_ON);
    values.compound = raw (param::COMPOUND);
    values.accentPitch = raw (param::ACCENT_PITCH);
    values.beatPitch = raw (param::BEAT_PITCH);
    values.subPitch = raw (param::SUB_PITCH);
    values.accentGain = raw (param::ACCENT_GAIN);
    values.beatGain = raw (param::BEAT_GAIN);
    values.subGain = raw (param::SUB_GAIN);
    values.clickDecay = raw (param::CLICK_DECAY);
    values.cueOn = raw (param::CUE_ON);
    values.cueLevel = raw (param::CUE_LEVEL);
    values.cuePan = raw (param::CUE_PAN);
    values.countLength = raw (param::COUNT_LENGTH);
    values.countDirection = raw (param::COUNT_DIRECTION);
    values.cueToAux = raw (param::CUE_TO_AUX);
}

juce::AudioProcessorEditor* ClickMakerProcessor::createEditor()
{
    return new ClickMakerEditor (*this);
}

bool ClickMakerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (! layouts.inputBuses.isEmpty() || layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (int bus = 1; bus < layouts.outputBuses.size(); ++bus)
    {
        const auto& set = layouts.outputBuses.getReference (bus);
        if (! set.isDisabled() && set != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

void ClickMakerProcessor::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    clock.prepare (sampleRate);
    click.prepare (sampleRate);
    cue.prepare (sampleRate);
    tickCursor = {};
    scratch.setSize (2, std::max (maximumBlockSize, MIN_SCRATCH_SAMPLES));

    // Start the gain ramps at their targets so the first downbeat is not faded in.
    const auto settings = readBlockSettings();
    lastClickGain = panGains (settings.clickGain, settings.clickPan);
    lastCueGain = panGains (settings.cueGain, settings.cuePan);
}

void ClickMakerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    if (numSamples == 0)
        return;

    // Only reached if a host breaks its announced maximum block size.
    if (numSamples > scratch.getNumSamples())
        scratch.setSize (2, numSamples, false, false, true);

    // Swapping is only safe while no word voice points into the current config.
    if (! cue.isSounding())
        configHandoff.adoptPending();

    const auto* config = configHandoff.current();
    const auto settings = readBlockSettings();
    const int previewSlot = previewRequest.exchange (NO_SLOT);

    if (previewSlot != NO_SLOT)
        clock.startPreview();

    const auto* playHead = getPlayHead();
    const auto time = clock.next (playHead != nullptr ? playHead->getPosition() : juce::nullopt, numSamples);

    if (time.cueBreak)
        cue.cancel();

    const auto groups = config != nullptr ? std::span<const GroupPattern> (config->groups) : std::span<const GroupPattern>();
    const auto bar = makeBarLayout (time.meter, { settings.compound, groups });

    if (previewSlot != NO_SLOT && time.isPreview)
        cue.trigger (previewSlot, time.ppqStart, time, bar, shapeFor (previewSlot, config, settings));

    float* clickBus = scratch.getWritePointer (CLICK_CHANNEL);
    float* cueBus = scratch.getWritePointer (CUE_CHANNEL);
    juce::FloatVectorOperations::clear (clickBus, numSamples);
    juce::FloatVectorOperations::clear (cueBus, numSamples);

    // Render up to each note so words of a replaced cue that precede the note are not lost.
    int rendered = 0;

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        const bool stopsCue = message.isAllNotesOff() || message.isAllSoundOff();
        const int slot = message.isNoteOn() ? message.getNoteNumber() - FIRST_SLOT_NOTE : NO_SLOT;
        const bool isCueNote = slot >= 0 && slot < SLOT_COUNT;

        if (! stopsCue && ! isCueNote)
            continue;

        const int position = juce::jlimit (rendered, numSamples, metadata.samplePosition);
        cue.render (cueBus, rendered, position, time, config);
        rendered = position;

        if (stopsCue)
            cue.cancel();
        else if (time.isPlaying && ! time.isPreview)
            cue.trigger (slot, time.ppqAt (position), time, bar, shapeFor (slot, config, settings));
        else
            previewRequest.store (slot); // auditioned from the next block on the preview clock
    }

    cue.render (cueBus, rendered, numSamples, time, config);

    const auto pattern = makeClickPattern (bar, settings.grid);
    const int tickCount = collectTicks (time, numSamples, pattern, tickCursor, ticks);
    click.render (clickBus, numSamples, std::span<const Tick> (ticks.data(), (size_t) tickCount), settings.sound);

    if (time.isPreview && ! cue.isActive())
        clock.stopPreview();

    auto mainOut = getBusBuffer (buffer, false, MAIN_BUS);
    auto cueOut = getBusCount (false) > CUE_BUS ? getBusBuffer (buffer, false, CUE_BUS) : juce::AudioBuffer<float>();
    const bool splitCue = settings.cueToAux && cueOut.getNumChannels() >= 2;

    mix (mainOut, clickBus, numSamples, lastClickGain, panGains (settings.clickGain, settings.clickPan));
    mix (splitCue ? cueOut : mainOut, cueBus, numSamples, lastCueGain, panGains (settings.cueGain, settings.cuePan));

    updateBeatDisplay (time, bar);
    midi.clear();
}

ClickMakerProcessor::BlockSettings ClickMakerProcessor::readBlockSettings() const
{
    const auto value = [] (const std::atomic<float>* parameter) { return parameter->load (std::memory_order_relaxed); };
    const auto isOn = [&] (const std::atomic<float>* parameter) { return value (parameter) >= 0.5f; };
    const auto index = [&] (const std::atomic<float>* parameter) { return juce::roundToInt (value (parameter)); };
    const auto gain = [&] (const std::atomic<float>* parameter) { return juce::Decibels::decibelsToGain (value (parameter)); };

    BlockSettings settings;
    settings.grid = { (GridChoice) index (values.clickGrid), isOn (values.swingOn),
                      value (values.swingAmount) / 100.0, isOn (values.accentOn) };
    settings.compound = isOn (values.compound);
    settings.sound.type = (ClickSoundType) index (values.clickSound);
    settings.sound.decaySeconds = value (values.clickDecay) / 1000.0f;
    settings.sound.tones[(size_t) TickKind::accent] = { value (values.accentPitch), gain (values.accentGain) };
    settings.sound.tones[(size_t) TickKind::beat] = { value (values.beatPitch), gain (values.beatGain) };
    settings.sound.tones[(size_t) TickKind::sub] = { value (values.subPitch), gain (values.subGain) };
    settings.clickGain = isOn (values.clickOn) ? gain (values.clickLevel) : 0.0f;
    settings.clickPan = value (values.clickPan);
    settings.cueGain = isOn (values.cueOn) ? gain (values.cueLevel) : 0.0f;
    settings.cuePan = value (values.cuePan);
    settings.defaultCountBars = index (values.countLength) + 1;
    settings.direction = (CountDirection) index (values.countDirection);
    settings.cueToAux = isOn (values.cueToAux);
    return settings;
}

CueShape ClickMakerProcessor::shapeFor (int slot, const EngineConfig* config, const BlockSettings& settings)
{
    const auto mode = config != nullptr ? config->countModes[(size_t) slot] : CountMode::useDefault;
    const int bars = mode == CountMode::nameOnly ? 0
                   : mode == CountMode::oneBar   ? 1
                   : mode == CountMode::twoBars  ? 2
                                                 : settings.defaultCountBars;

    // Whether a slot has a name comes from its text, so a failed render never shifts the timing.
    return { config != nullptr && config->hasName (slot), bars, settings.direction };
}

ClickMakerProcessor::StereoGain ClickMakerProcessor::panGains (float gain, float pan)
{
    const float angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    return { gain * std::cos (angle), gain * std::sin (angle) };
}

void ClickMakerProcessor::mix (juce::AudioBuffer<float>& destination, const float* source, int numSamples,
                               StereoGain& last, StereoGain next)
{
    destination.addFromWithRamp (0, 0, source, numSamples, last.left, next.left);
    destination.addFromWithRamp (1, 0, source, numSamples, last.right, next.right);
    last = next;
}

void ClickMakerProcessor::updateBeatDisplay (const BlockTime& time, const BarLayout& bar)
{
    int beat = -1;

    if (time.isPlaying)
    {
        const double positionInBar = time.ppqStart - time.barStartPpq;
        beat = 0;

        for (int i = 1; i < bar.numBeats; ++i)
            if (positionInBar >= bar.beatPpq[(size_t) i] - BEAT_DISPLAY_EPSILON)
                beat = i;
    }

    displayBeat.store (beat, std::memory_order_relaxed);
    displayBeatsPerBar.store (bar.numBeats, std::memory_order_relaxed);
    displayCueSlot.store (cue.currentSlot(), std::memory_order_relaxed);
}

BeatDisplay ClickMakerProcessor::beatDisplay() const
{
    return { displayBeat.load (std::memory_order_relaxed),
             displayBeatsPerBar.load (std::memory_order_relaxed),
             displayCueSlot.load (std::memory_order_relaxed) };
}

void ClickMakerProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state (STATE_TYPE);
    state.setProperty (VERSION_PROPERTY, STATE_VERSION, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    state.appendChild (settingsToTree (speechWorker.settings()), nullptr);
    state.appendChild (wordCacheToTree (speechWorker.wordsInUse()), nullptr);

    juce::MemoryOutputStream stream (destination, false);
    state.writeToStream (stream);
}

void ClickMakerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);

    if (! state.hasType (STATE_TYPE) || (int) state[VERSION_PROPERTY] != STATE_VERSION)
        return;

    if (const auto saved = state.getChildWithName (parameters.state.getType()); saved.isValid())
        parameters.replaceState (saved);

    speechWorker.restore (settingsFromTree (state.getChildWithName (SETTINGS_TREE_TYPE)),
                          wordCacheFromTree (state.getChildWithName (AUDIO_TREE_TYPE)));
}

} // namespace clickmaker

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new clickmaker::ClickMakerProcessor();
}
