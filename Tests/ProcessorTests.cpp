#include "RenderHarness.h"

#include "ParamIds.h"

namespace clickmaker::test
{

namespace
{
constexpr int WORKER_TIMEOUT_MS = 10000;
constexpr float ONSET_THRESHOLD = 1.0e-3f;
constexpr int ONSET_GAP = 480;          // 10 ms of silence separates two sounds
constexpr int TIMING_TOLERANCE = 5;     // samples, about 0.1 ms
constexpr int PITCH_WINDOW = 384;       // 8 ms
constexpr int VOWEL_WINDOW = 960;       // 20 ms, enough cycles to tell the fake numbers apart
constexpr double MAX_ONSET_LEAD_SECONDS = 0.15;
constexpr int SLOT_C1 = 0;
constexpr int SLOT_F1 = 5;             // "Pre-Chorus" by default

std::unique_ptr<ClickMakerProcessor> makeReadyProcessor (SpeechRenderer renderer = fakeSpeech)
{
    auto processor = std::make_unique<ClickMakerProcessor> (std::move (renderer));
    waitUntil ([&] { return processor->speechStatus().complete; }, WORKER_TIMEOUT_MS);
    return processor;
}

// Click on the left, cue on the right, so each can be measured alone.
void splitChannels (ClickMakerProcessor& processor)
{
    setParameter (processor, param::CLICK_PAN, -1.0f);
    setParameter (processor, param::CUE_PAN, 1.0f);
}

std::vector<int> onsetsOf (const RenderResult& result, int channel)
{
    return findOnsets (result.audio.getReadPointer (channel), result.audio.getNumSamples(), ONSET_THRESHOLD, ONSET_GAP);
}

double pitchAt (const RenderResult& result, int channel, int sample)
{
    return estimateFrequency (result.audio.getReadPointer (channel, sample), PITCH_WINDOW, TEST_SAMPLE_RATE);
}

// The cue's pitch shortly after a beat, which names the fake word sounding there.
double cuePitchAfter (const RenderResult& result, double ppq, double seconds)
{
    const auto sample = (int) std::round (result.sampleAtPpq (ppq) + seconds * TEST_SAMPLE_RATE);
    return estimateFrequency (result.audio.getReadPointer (RIGHT, sample), VOWEL_WINDOW, TEST_SAMPLE_RATE);
}

double firstAudibleSeconds (const WordTake& take)
{
    const auto first = std::find_if (take.samples.begin(), take.samples.end(),
                                     [] (float s) { return std::abs (s) > ONSET_THRESHOLD; });
    return (double) (first - take.samples.begin()) / take.sampleRate;
}

// Where a word's first audible sample should appear in the output.
double expectedWordOnset (const RenderResult& result, double beatPpq, double earliestPpq, const WordTake& take, double bpm)
{
    const double leadPpq = std::min (take.onsetSeconds(), MAX_ONSET_LEAD_SECONDS) * bpm / 60.0;
    const double firePpq = std::max (beatPpq - leadPpq, earliestPpq);
    return result.sampleAtPpq (firePpq) + firstAudibleSeconds (take) * TEST_SAMPLE_RATE;
}

bool hasSoundAfter (const RenderResult& result, int channel, int from)
{
    return result.audio.getMagnitude (channel, from, result.audio.getNumSamples() - from) > 1.0e-6f;
}

// Words with a long consonant that, like real speech, shortens with the rate. A number's slowest take fits its
// beat, but the next number starts early by its consonant and would cut it.
std::optional<RenderedSpeech> longConsonantSpeech (const SpeechRequest& request, const AbortCheck&)
{
    constexpr double SAMPLE_RATE = 22050.0;
    constexpr double PADDING_SECONDS = 0.02;
    constexpr double CONSONANT_SECONDS = 0.12;
    constexpr double VOWEL_SECONDS = 0.4;
    constexpr double CONSONANT_HZ = 6000.0;
    constexpr double VOWEL_HZ = 400.0;

    const double stretch = DEFAULT_RATE_PERCENT / 100.0 / request.rate;
    RenderedSpeech speech { {}, SAMPLE_RATE, "fake" };

    const auto append = [&speech] (double seconds, double frequency, float level)
    {
        for (int i = 0; i < (int) (seconds * SAMPLE_RATE); ++i)
            speech.samples.push_back (level * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / SAMPLE_RATE));
    };

    append (PADDING_SECONDS, 0.0, 0.0f);
    append (CONSONANT_SECONDS * stretch, CONSONANT_HZ, 0.4f);
    append (VOWEL_SECONDS * stretch, VOWEL_HZ, 0.8f);
    append (PADDING_SECONDS, 0.0, 0.0f);
    return speech;
}

struct SteadyPlayHead final : juce::AudioPlayHead
{
    PositionInfo info;

    juce::Optional<PositionInfo> getPosition() const override { return info; }
};
} // namespace

class ClickRenderTests final : public juce::UnitTest
{
public:
    ClickRenderTests() : juce::UnitTest ("Click rendering", "Processor") {}

    void runTest() override
    {
        auto processor = makeReadyProcessor();
        splitChannels (*processor);

        beginTest ("Clicks follow the host grid across mixed block sizes");
        {
            Scenario scenario;
            scenario.name = "click_44_120";
            scenario.blockSizes = { 512, 37, 1024, 7 };
            scenario.lengthPpq = 15.75; // off the grid, so a final block cannot catch the next beat
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, LEFT);

            expectEquals ((int) onsets.size(), 16);
            for (size_t k = 0; k < onsets.size(); ++k)
            {
                expectWithinAbsoluteError (onsets[k], (int) std::round (result.sampleAtPpq ((double) k)), 2);
                expectWithinAbsoluteError (pitchAt (result, LEFT, onsets[k]), k % 4 == 0 ? 1600.0 : 1000.0, 150.0);
            }

            expectEquals (countNonSilent (result.audio.getReadPointer (RIGHT), result.audio.getNumSamples(), 1.0e-6f), 0);
        }

        beginTest ("A stopped transport is silent");
        {
            Scenario scenario;
            scenario.playing = false;
            scenario.lengthPpq = 2.0;
            const auto result = renderScenario (*processor, scenario);
            expectEquals (result.audio.getMagnitude (0, result.audio.getNumSamples()), 0.0f);
        }

        beginTest ("Tempo and meter changes keep clicks and accents on the host grid");
        {
            Scenario scenario;
            scenario.name = "click_meter_tempo";
            scenario.blockSizes = { 4096 };
            scenario.lengthPpq = 27.75;
            scenario.bpmAt = [] (double ppq) { return ppq < 8.0 ? 120.0 : 90.0; };
            scenario.meters = { { 0.0, { 4, 4 } }, { 16.0, { 3, 4 } } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, LEFT);

            expectEquals ((int) onsets.size(), 28);
            for (size_t k = 0; k < onsets.size(); ++k)
            {
                const bool accent = k < 16 ? k % 4 == 0 : (k - 16) % 3 == 0;
                expectWithinAbsoluteError (onsets[k], (int) std::round (result.sampleAtPpq ((double) k)), 2);
                expectWithinAbsoluteError (pitchAt (result, LEFT, onsets[k]), accent ? 1600.0 : 1000.0, 150.0);
            }
        }

        beginTest ("A cycle wrap that the host does not split plays the downbeat once");
        {
            Scenario scenario;
            scenario.blockSizes = { 1000 };
            scenario.lengthPpq = 32.0;
            scenario.jumps = { { 16.0, 0.0 } };
            const auto onsets = onsetsOf (renderScenario (*processor, scenario), LEFT);

            expectEquals ((int) onsets.size(), 32);
            for (size_t k = 1; k < onsets.size(); ++k)
                expect (onsets[k] - onsets[k - 1] > 23000);
        }

        beginTest ("Swing and a 1/16 grid render in time");
        {
            setParameter (*processor, param::CLICK_GRID, 3.0f);
            setParameter (*processor, param::SWING_ON, 1.0f);
            setParameter (*processor, param::SWING_AMOUNT, 66.7f);
            setParameter (*processor, param::CLICK_DECAY, 20.0f);

            Scenario scenario;
            scenario.name = "click_16th_swing";
            scenario.lengthPpq = 3.9;
            const auto onsets = onsetsOf (renderScenario (*processor, scenario), LEFT);
            const double swungSixteenth = 0.25 + (2.0 * 0.667 - 1.0) * 0.25;

            expectEquals ((int) onsets.size(), 16);
            expectWithinAbsoluteError (onsets[1], (int) std::round (swungSixteenth * 24000.0), 2);

            setParameter (*processor, param::CLICK_GRID, 0.0f);
            setParameter (*processor, param::SWING_ON, 0.0f);
            setParameter (*processor, param::CLICK_DECAY, 80.0f);
        }
    }
};

class CueRenderTests final : public juce::UnitTest
{
public:
    CueRenderTests() : juce::UnitTest ("Cue rendering", "Processor") {}

    void runTest() override
    {
        auto processor = makeReadyProcessor();
        splitChannels (*processor);

        beginTest ("A count-only cue speaks 4 3 2 1 with each vowel on its beat");
        {
            Scenario scenario;
            scenario.name = "cue_count_120";
            scenario.lengthPpq = 9.0;
            scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_C1 } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, RIGHT);
            const std::array<const char*, 4> words { "Four", "Three", "Two", "One" };

            expectEquals ((int) onsets.size(), 4);
            for (size_t k = 0; k < words.size() && k < onsets.size(); ++k)
            {
                const auto take = fakeTake (words[k], 0.5f);
                expectWithinAbsoluteError ((double) onsets[k], expectedWordOnset (result, 4.0 + (double) k, 4.0, take, 120.0),
                                           (double) TIMING_TOLERANCE);
                const int vowel = onsets[k] + (int) (0.04 * TEST_SAMPLE_RATE);
                expectWithinAbsoluteError (pitchAt (result, RIGHT, vowel), fakeFrequencyFor (words[k]), fakeFrequencyFor (words[k]) * 0.1);
            }
        }

        beginTest ("A named cue with an 8 count: name bar, 4 . 3 . | 4 3 2 1");
        {
            setParameter (*processor, param::COUNT_LENGTH, 1.0f);

            Scenario scenario;
            scenario.name = "cue_named_8";
            scenario.lengthPpq = 17.0;
            scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_F1 } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, RIGHT);

            const std::array<double, 7> beats { 4.0, 8.0, 10.0, 12.0, 13.0, 14.0, 15.0 };
            const std::array<const char*, 7> words { "Pre-Chorus", "Four", "Three", "Four", "Three", "Two", "One" };

            expectEquals ((int) onsets.size(), 7);
            for (size_t k = 0; k < beats.size() && k < onsets.size(); ++k)
                expectWithinAbsoluteError ((double) onsets[k],
                                           expectedWordOnset (result, beats[k], 4.0, fakeTake (words[k], 0.5f), 120.0),
                                           (double) TIMING_TOLERANCE);

            setParameter (*processor, param::COUNT_LENGTH, 0.0f);
        }

        beginTest ("A late note starts its first word at the note and keeps the rest on the beat");
        {
            Scenario scenario;
            scenario.lengthPpq = 9.0;
            scenario.notes = { { 4.02, FIRST_SLOT_NOTE + SLOT_C1 } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, RIGHT);

            expectEquals ((int) onsets.size(), 4);
            expectWithinAbsoluteError ((double) onsets[0], expectedWordOnset (result, 4.0, 4.02, fakeTake ("Four", 0.5f), 120.0),
                                       (double) TIMING_TOLERANCE);
            expectWithinAbsoluteError ((double) onsets[1], expectedWordOnset (result, 5.0, 4.02, fakeTake ("Three", 0.5f), 120.0),
                                       (double) TIMING_TOLERANCE);
        }

        beginTest ("A note placed a 16th early lets the first word lead into its beat");
        {
            Scenario scenario;
            scenario.lengthPpq = 9.0;
            scenario.notes = { { 3.75, FIRST_SLOT_NOTE + SLOT_C1 } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, RIGHT);

            expectEquals ((int) onsets.size(), 4);
            if (! onsets.empty())
                expectWithinAbsoluteError ((double) onsets[0], expectedWordOnset (result, 4.0, 3.75, fakeTake ("Four", 0.5f), 120.0),
                                           (double) TIMING_TOLERANCE);
        }

        beginTest ("A note pressed mid-bar, as on a live pad, counts from the next bar");
        {
            Scenario scenario;
            scenario.lengthPpq = 13.0;
            scenario.notes = { { 5.5, FIRST_SLOT_NOTE + SLOT_C1 } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, RIGHT);
            const std::array<const char*, 4> words { "Four", "Three", "Two", "One" };

            expectEquals ((int) onsets.size(), 4);
            for (size_t k = 0; k < words.size() && k < onsets.size(); ++k)
                expectWithinAbsoluteError ((double) onsets[k],
                                           expectedWordOnset (result, 8.0 + (double) k, 5.5, fakeTake (words[k], 0.5f), 120.0),
                                           (double) TIMING_TOLERANCE);
        }

        beginTest ("A meter change during a cue counts the bar as the host plays it");
        {
            // Name bar in 4/4, then a 2/4 bar before the section at ppq 10.
            Scenario scenario;
            scenario.lengthPpq = 14.0;
            scenario.meters = { { 0.0, { 4, 4 } }, { 8.0, { 2, 4 } }, { 10.0, { 4, 4 } } };
            scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_F1 } };

            const auto down = renderScenario (*processor, scenario);
            expectWithinAbsoluteError (cuePitchAfter (down, 8.0, 0.06), fakeFrequencyFor ("Two"), 40.0);
            expectWithinAbsoluteError (cuePitchAfter (down, 9.0, 0.02), fakeFrequencyFor ("One"), 40.0);
            expect (! hasSoundAfter (down, RIGHT, (int) down.sampleAtPpq (10.0)), "the count ran into the section");

            // Counting up, the word already started for the bar is still right and must not restart.
            setParameter (*processor, param::COUNT_DIRECTION, 1.0f);
            const auto up = renderScenario (*processor, scenario);
            expectWithinAbsoluteError (cuePitchAfter (up, 8.0, 0.01), fakeFrequencyFor ("One"), 40.0);
            expectWithinAbsoluteError (cuePitchAfter (up, 9.0, 0.02), fakeFrequencyFor ("Two"), 40.0);
            expect (! hasSoundAfter (up, RIGHT, (int) up.sampleAtPpq (10.0)), "the count ran into the section");
            setParameter (*processor, param::COUNT_DIRECTION, 0.0f);
        }

        beginTest ("A name can share its bar with the count: Pre-Chorus 3 2 1");
        {
            setParameter (*processor, param::NAME_LENGTH, 1.0f);

            Scenario scenario;
            scenario.lengthPpq = 12.5;
            scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_F1 } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, RIGHT);
            const std::array<const char*, 3> words { "Three", "Two", "One" };

            expectEquals ((int) onsets.size(), 4);
            for (size_t k = 0; k < words.size() && k + 1 < onsets.size(); ++k)
                expectWithinAbsoluteError ((double) onsets[k + 1],
                                           expectedWordOnset (result, 5.0 + (double) k, 4.0, fakeTake (words[k], 0.5f), 120.0),
                                           (double) TIMING_TOLERANCE);
            expect (! hasSoundAfter (result, RIGHT, (int) result.sampleAtPpq (8.0)), "the cue ran past its bar");

            // A slot can still give its name a bar of its own: Pre-Chorus | 4 3 2 1.
            auto settings = processor->settings();
            settings.slots[SLOT_F1].nameLength = NameLength::ownBar;
            processor->setSettings (settings);
            expect (waitUntil ([&] { return processor->speechStatus().complete; }, WORKER_TIMEOUT_MS));
            expectEquals ((int) onsetsOf (renderScenario (*processor, scenario), RIGHT).size(), 5);

            settings.slots[SLOT_F1].nameLength = NameLength::useDefault;
            processor->setSettings (settings);
            expect (waitUntil ([&] { return processor->speechStatus().complete; }, WORKER_TIMEOUT_MS));
            setParameter (*processor, param::NAME_LENGTH, 0.0f);
        }

        beginTest ("A fast tempo plays a faster take");
        {
            const auto wordLength = [&] (double bpm)
            {
                Scenario scenario;
                scenario.lengthPpq = 8.0;
                scenario.bpmAt = [bpm] (double) { return bpm; };
                scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_C1 } };
                const auto result = renderScenario (*processor, scenario);
                const auto onsets = onsetsOf (result, RIGHT);
                return onsets.size() < 3 ? 0
                                         : countNonSilent (result.audio.getReadPointer (RIGHT, onsets[1]), onsets[2] - onsets[1],
                                                           ONSET_THRESHOLD);
            };

            const int relaxed = wordLength (120.0);
            const int fast = wordLength (220.0);
            expect (relaxed > 0 && fast > 0);
            expect (fast < relaxed * 0.92, "expected a faster take at 220 bpm");
        }

        beginTest ("Stopping mid-cue silences it within the choke time");
        {
            Scenario scenario;
            scenario.lengthPpq = 8.0;
            scenario.stopAtPpq = 5.5;
            scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_C1 } };
            const auto result = renderScenario (*processor, scenario);

            const auto stopBlock = std::find_if (result.blocks.begin(), result.blocks.end(),
                                                 [] (const BlockLog& b) { return ! b.playing; });
            expect (stopBlock != result.blocks.end());
            const int quietFrom = stopBlock->startSample + (int) (0.005 * TEST_SAMPLE_RATE) + 1;
            expect (! hasSoundAfter (result, RIGHT, quietFrom));
        }

        beginTest ("A tempo change during a cue keeps later words on the new beats");
        {
            Scenario scenario;
            scenario.lengthPpq = 9.0;
            scenario.bpmAt = [] (double ppq) { return ppq < 5.5 ? 120.0 : 90.0; };
            scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_C1 } };
            const auto result = renderScenario (*processor, scenario);
            const auto onsets = onsetsOf (result, RIGHT);

            expectEquals ((int) onsets.size(), 4);
            if (onsets.size() == 4)
                expectWithinAbsoluteError ((double) onsets[3], expectedWordOnset (result, 7.0, 4.0, fakeTake ("One", 0.5f), 90.0),
                                           (double) TIMING_TOLERANCE);
        }

        beginTest ("Preview plays the cue and clicks while the transport is stopped");
        {
            Scenario scenario;
            scenario.playing = false;
            scenario.lengthPpq = 6.0;
            processor->requestPreview (SLOT_C1);
            const auto result = renderScenario (*processor, scenario);

            expectEquals ((int) onsetsOf (result, RIGHT).size(), 4);
            expect (onsetsOf (result, LEFT).size() >= 4);
        }

        beginTest ("A word is fitted to end before the next one starts early on its consonant");
        {
            auto slowStart = makeReadyProcessor (longConsonantSpeech);
            splitChannels (*slowStart);

            Scenario scenario;
            scenario.lengthPpq = 9.0;
            scenario.notes = { { 3.75, FIRST_SLOT_NOTE + SLOT_C1 } };
            expectEquals ((int) onsetsOf (renderScenario (*slowStart, scenario), RIGHT).size(), 4, "a number ran into the next one");
        }

        beginTest ("The cue moves to the aux bus when split outputs are on");
        {
            setParameter (*processor, param::CUE_TO_AUX, 1.0f);
            setParameter (*processor, param::CLICK_ON, 0.0f);

            Scenario scenario;
            scenario.lengthPpq = 9.0;
            scenario.notes = { { 4.0, FIRST_SLOT_NOTE + SLOT_C1 } };
            const auto result = renderScenario (*processor, scenario);

            expectEquals (result.audio.getNumChannels(), 4);
            expectEquals (result.audio.getMagnitude (LEFT, 0, result.audio.getNumSamples()), 0.0f);
            expectEquals (result.audio.getMagnitude (RIGHT, 0, result.audio.getNumSamples()), 0.0f);
            expectEquals ((int) findOnsets (result.audio.getReadPointer (3), result.audio.getNumSamples(),
                                            ONSET_THRESHOLD, ONSET_GAP).size(), 4);

            setParameter (*processor, param::CUE_TO_AUX, 0.0f);
            setParameter (*processor, param::CLICK_ON, 1.0f);
        }
    }
};

class StateTests final : public juce::UnitTest
{
public:
    StateTests() : juce::UnitTest ("State", "Processor") {}

    void runTest() override
    {
        beginTest ("A saved state restores settings and audio without any speech rendering");

        auto original = makeReadyProcessor();
        splitChannels (*original);
        setParameter (*original, param::COUNT_LENGTH, 1.0f);

        auto settings = original->settings();
        settings.slots[SLOT_C1].name = "Count In";
        settings.slots[SLOT_C1].nameLength = NameLength::oneBeat;
        settings.slots[SLOT_F1].countMode = CountMode::nameOnly;
        settings.accentGroups = "2+2+3";
        original->setSettings (settings);
        expect (waitUntil ([&] { return original->speechStatus().complete; }, WORKER_TIMEOUT_MS));

        juce::MemoryBlock state;
        original->getStateInformation (state);
        expect (state.getSize() < 2 * 1024 * 1024, "state is " + juce::String ((int) state.getSize()) + " bytes");

        auto calls = std::make_shared<std::atomic<int>> (0);
        ClickMakerProcessor restored ([calls] (const SpeechRequest& request, const AbortCheck& abort)
        {
            ++*calls;
            return fakeSpeech (request, abort);
        });
        restored.setStateInformation (state.getData(), (int) state.getSize());

        expect (waitUntil ([&] { return restored.speechStatus().complete; }, WORKER_TIMEOUT_MS));
        expectEquals (calls->load(), 0);
        expect (restored.settings() == settings);

        Scenario scenario;
        scenario.lengthPpq = 14.0;
        scenario.notes = { { 0.0, FIRST_SLOT_NOTE + SLOT_C1 }, { 12.0, FIRST_SLOT_NOTE + SLOT_F1 } };
        const auto expected = renderScenario (*original, scenario);
        const auto actual = renderScenario (restored, scenario);

        float difference = 0.0f;
        for (int c = 0; c < expected.audio.getNumChannels(); ++c)
            for (int i = 0; i < expected.audio.getNumSamples(); ++i)
                difference = std::max (difference, std::abs (expected.audio.getSample (c, i) - actual.audio.getSample (c, i)));

        // Restored takes went through 16-bit storage.
        expect (difference < 1.0e-4f, "difference " + juce::String (difference));
    }
};

class PerformanceTests final : public juce::UnitTest
{
public:
    PerformanceTests() : juce::UnitTest ("Performance", "Processor") {}

    void runTest() override
    {
        beginTest ("Ten minutes of 1/16 clicks and cues render in under 1% of real time");

        auto processor = makeReadyProcessor();
        setParameter (*processor, param::CLICK_GRID, 3.0f);

        constexpr double MINUTES = 10.0;
        constexpr double BPM = 120.0;
        constexpr int BLOCK = 64;
        constexpr double CUE_EVERY_PPQ = 16.0;

        SteadyPlayHead playHead;
        playHead.info.setIsPlaying (true);
        playHead.info.setBpm (BPM);
        playHead.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        processor->setPlayHead (&playHead);
        processor->prepareToPlay (TEST_SAMPLE_RATE, BLOCK);

        juce::AudioBuffer<float> buffer (processor->getTotalNumOutputChannels(), BLOCK);
        juce::MidiBuffer midi;
        const double ppqPerBlock = BLOCK * BPM / (60.0 * TEST_SAMPLE_RATE);
        const auto blocks = (int) (MINUTES * 60.0 * TEST_SAMPLE_RATE / BLOCK);
        double nextCue = 0.0;

        const auto started = juce::Time::getMillisecondCounterHiRes();

        for (int b = 0; b < blocks; ++b)
        {
            const double ppq = b * ppqPerBlock;
            playHead.info.setPpqPosition (ppq);
            playHead.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            midi.clear();

            if (ppq >= nextCue)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, FIRST_SLOT_NOTE + SLOT_F1, (juce::uint8) 100), 0);
                nextCue += CUE_EVERY_PPQ;
            }

            processor->processBlock (buffer, midi);
        }

        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - started;
        const double realTimeShare = elapsedMs / (MINUTES * 60.0 * 1000.0);
        processor->releaseResources();
        processor->setPlayHead (nullptr);

        logMessage ("render share of real time: " + juce::String (realTimeShare * 100.0, 4) + " %");

       #if ! JUCE_DEBUG // debug and sanitizer builds are several times slower by design
        expect (realTimeShare < 0.01);
       #endif
    }
};

static ClickRenderTests clickRenderTests;
static CueRenderTests cueRenderTests;
static StateTests stateTests;
static PerformanceTests performanceTests;

} // namespace clickmaker::test
