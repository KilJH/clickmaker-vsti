#include "RenderHarness.h"

#include <thread>

namespace clickmaker::test
{

namespace
{
constexpr int WORKER_TIMEOUT_MS = 10000;

struct ExpectedWord
{
    double ppq;
    double length;
    int wordId;
};

WordTake takeOfLength (double seconds, double onsetSeconds)
{
    WordTake take;
    take.sampleRate = 22050.0;
    take.samples.assign ((size_t) (seconds * take.sampleRate), 0.1f);
    take.onsetFrame = (int) (onsetSeconds * take.sampleRate);
    return take;
}

// Counts renderer calls so tests can prove words are rendered once and only when needed.
struct CountingSpeech
{
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>> (0);

    SpeechRenderer renderer() const
    {
        return [counter = calls] (const SpeechRequest& request, const AbortCheck& abort)
        {
            ++*counter;
            return fakeSpeech (request, abort);
        };
    }
};
} // namespace

class CueSequenceTests final : public juce::UnitTest
{
public:
    CueSequenceTests() : juce::UnitTest ("Cue sequences", "Unit") {}

    void runTest() override
    {
        const auto fourFour = makeBarLayout ({ 4, 4 }, {});
        const int slot = 5;
        const int name = nameWordId (slot);

        beginTest ("4/4 count down, one bar");
        check (buildCueSequence (fourFour, slot, { false, 1, CountDirection::down }),
               { { 0, 1, numberWordId (4) }, { 1, 1, numberWordId (3) }, { 2, 1, numberWordId (2) }, { 3, 1, numberWordId (1) } });

        beginTest ("4/4 count down, two bars: 4 . 3 . | 4 3 2 1");
        check (buildCueSequence (fourFour, slot, { false, 2, CountDirection::down }),
               { { 0, 2, numberWordId (4) }, { 2, 2, numberWordId (3) }, { 4, 1, numberWordId (4) },
                 { 5, 1, numberWordId (3) }, { 6, 1, numberWordId (2) }, { 7, 1, numberWordId (1) } });

        beginTest ("Name bar, then an upward two-bar count");
        check (buildCueSequence (fourFour, slot, { true, 2, CountDirection::up }),
               { { 0, 4, name }, { 4, 2, numberWordId (1) }, { 6, 2, numberWordId (2) }, { 8, 1, numberWordId (1) },
                 { 9, 1, numberWordId (2) }, { 10, 1, numberWordId (3) }, { 11, 1, numberWordId (4) } });

        beginTest ("Name only");
        check (buildCueSequence (fourFour, slot, { true, 0, CountDirection::down }), { { 0, 4, name } });

        beginTest ("3/4 two bars speaks only the downbeat in the lead-in bar");
        check (buildCueSequence (makeBarLayout ({ 3, 4 }, {}), slot, { false, 2, CountDirection::down }),
               { { 0, 3, numberWordId (3) }, { 3, 1, numberWordId (3) }, { 4, 1, numberWordId (2) }, { 5, 1, numberWordId (1) } });

        beginTest ("Compound 6/8 counts dotted quarters");
        check (buildCueSequence (makeBarLayout ({ 6, 8 }, {}), slot, { false, 1, CountDirection::down }),
               { { 0, 1.5, numberWordId (2) }, { 1.5, 1.5, numberWordId (1) } });

        beginTest ("Grouped 7/8 counts group starts with uneven lengths");
        {
            const auto groups = parseGroupPatterns ("2+2+3");
            check (buildCueSequence (makeBarLayout ({ 7, 8 }, { true, groups }), slot, { false, 1, CountDirection::down }),
                   { { 0, 1, numberWordId (3) }, { 1, 1, numberWordId (2) }, { 2, 1.5, numberWordId (1) } });
        }

        beginTest ("Numbers above twelve stay silent but keep their time");
        {
            const auto sequence = buildCueSequence (makeBarLayout ({ 13, 8 }, { false, {} }), slot,
                                                    { false, 1, CountDirection::down });
            expectEquals (sequence.size, 12);
            expectEquals (sequence.events[0].ppq, 0.5);
            expectEquals (sequence.events[0].wordId, numberWordId (12));
        }
    }

private:
    void check (const CueSequence& sequence, const std::vector<ExpectedWord>& expected)
    {
        expectEquals (sequence.size, (int) expected.size());

        for (size_t i = 0; i < expected.size() && (int) i < sequence.size; ++i)
        {
            expectWithinAbsoluteError (sequence.events[i].ppq, expected[i].ppq, 1.0e-9);
            expectWithinAbsoluteError (sequence.events[i].lengthPpq, expected[i].length, 1.0e-9);
            expectEquals (sequence.events[i].wordId, expected[i].wordId);
        }
    }
};

class TakeTests final : public juce::UnitTest
{
public:
    TakeTests() : juce::UnitTest ("Takes", "Unit") {}

    void runTest() override
    {
        beginTest ("chooseTake picks the slowest take that fits and starts early by the onset");
        {
            const WordTakes takes { takeOfLength (0.40, 0.05), takeOfLength (0.30, 0.05), takeOfLength (0.22, 0.05) };

            const auto relaxed = chooseTake (&takes, 8.0, 1.0, 7.0, 120.0);
            expect (relaxed.take == &takes[0]);
            expectWithinAbsoluteError (relaxed.firePpq, 8.0 - takes[0].onsetSeconds() * 2.0, 1.0e-9); // 2 ppq per second at 120 bpm

            expect (chooseTake (&takes, 8.0, 1.0, 7.0, 200.0).take == &takes[1]);
            expect (chooseTake (&takes, 8.0, 1.0, 7.0, 400.0).take == &takes[2]);

            const auto late = chooseTake (&takes, 8.0, 1.0, 8.05, 120.0);
            expectWithinAbsoluteError (late.firePpq, 8.05, 1.0e-9);

            const auto silent = chooseTake (nullptr, 8.0, 1.0, 7.0, 120.0);
            expect (silent.take == nullptr);
            expectEquals (silent.firePpq, 8.0);
        }

        beginTest ("prepareTake rejects silence, trims, normalizes and finds the vowel");
        {
            const std::vector<float> silence (2205, 0.0f);
            expect (! prepareTake (silence, 22050.0).has_value());

            const auto take = fakeTake ("Four", 0.5f);
            float peak = 0.0f;
            for (const auto sample : take.samples)
                peak = std::max (peak, std::abs (sample));

            expectWithinAbsoluteError (peak, 0.891f, 0.01f);
            expectEquals (take.samples.front(), 0.0f);

            // 5 ms kept before the 30 ms consonant; the vowel follows.
            expectWithinAbsoluteError (take.onsetSeconds(), 0.035, 0.010);
            expect (take.durationSeconds() < 0.40);
        }

        beginTest ("addTake keeps takes ordered and drops ones that are barely shorter");
        {
            WordTakes takes;
            addTake (takes, takeOfLength (0.30, 0.0));
            addTake (takes, takeOfLength (0.40, 0.0));
            addTake (takes, takeOfLength (0.39, 0.0));
            expectEquals ((int) takes.size(), 2);
            expectWithinAbsoluteError (takes[0].durationSeconds(), 0.40, 0.001);
            expectWithinAbsoluteError (takes[1].durationSeconds(), 0.30, 0.001);
        }

        beginTest ("Word cache survives serialization and rejects broken takes");
        {
            WordCache cache;
            auto takes = std::make_shared<WordTakes>();
            takes->push_back (fakeTake ("Chorus", 0.5f));
            takes->push_back (fakeTake ("Chorus", 0.6f));
            cache[{ "Chorus", "voice.id", 55 }] = { takes, TAKE_COUNT, 0, false };

            auto tree = wordCacheToTree (cache);
            juce::ValueTree broken ("WORD");
            broken.setProperty ("text", "Broken", nullptr);
            juce::ValueTree badTake ("TAKE");
            badTake.setProperty ("sampleRate", 0.0, nullptr);
            badTake.setProperty ("pcm", juce::MemoryBlock (3), nullptr);
            broken.appendChild (badTake, nullptr);
            tree.appendChild (broken, nullptr);

            juce::MemoryOutputStream stream;
            tree.writeToStream (stream);
            const auto restored = wordCacheFromTree (juce::ValueTree::readFromData (stream.getData(), stream.getDataSize()));

            expectEquals ((int) restored.size(), 1);
            const auto& word = restored.at ({ "Chorus", "voice.id", 55 });
            expectEquals ((int) word.takes->size(), 2);
            expectEquals (word.stepsDone, TAKE_COUNT);
            expectEquals ((*word.takes)[0].onsetFrame, (*takes)[0].onsetFrame);
            expectEquals ((*word.takes)[0].samples.size(), (*takes)[0].samples.size());
            expectWithinAbsoluteError ((*word.takes)[0].samples[600], (*takes)[0].samples[600], 1.0f / 16000.0f);
        }
    }
};

class HandoffTests final : public juce::UnitTest
{
public:
    HandoffTests() : juce::UnitTest ("Config handoff", "Unit") {}

    void runTest() override
    {
        beginTest ("Configs are adopted in order and the last one always arrives");

        constexpr std::uint32_t PUBLISH_COUNT = 20000;
        ConfigHandoff handoff;
        std::atomic<bool> publisherDone { false };
        bool ordered = true;

        std::thread publisher ([&]
        {
            for (std::uint32_t sequence = 1; sequence <= PUBLISH_COUNT; ++sequence)
            {
                auto config = std::make_unique<EngineConfig>();
                config->namedSlots = sequence;
                handoff.publish (std::move (config));
            }

            publisherDone = true;
        });

        // The audio side: randomly busy (no adoption) or idle, reading whatever is current.
        juce::Random random (3);
        std::uint32_t lastSeen = 0;

        for (int spins = 0; spins < 10000000; ++spins)
        {
            if (random.nextBool())
                handoff.adoptPending();

            if (const auto* config = handoff.current())
            {
                ordered = ordered && config->namedSlots >= lastSeen;
                lastSeen = config->namedSlots;
            }

            if (publisherDone && lastSeen == PUBLISH_COUNT)
                break;
        }

        publisher.join();
        expect (ordered);
        expectEquals ((int) lastSeen, (int) PUBLISH_COUNT);
    }
};

class SpeechWorkerTests final : public juce::UnitTest
{
public:
    SpeechWorkerTests() : juce::UnitTest ("Speech worker", "Unit") {}

    void runTest() override
    {
        beginTest ("Every word is rendered once per take, and an edit re-renders only that word");
        {
            ConfigHandoff handoff;
            CountingSpeech speech;
            SpeechWorker worker (handoff, speech.renderer());

            expect (waitUntil ([&] { return worker.status().complete; }, WORKER_TIMEOUT_MS));
            const auto status = worker.status();
            expectEquals (status.wordsTotal, 12 + 13);
            expectEquals (status.wordsReady, status.wordsTotal);
            expectEquals (speech.calls->load(), status.wordsTotal * TAKE_COUNT);

            auto settings = worker.settings();
            settings.slots[2].name = "Intro 2";
            worker.setSettings (settings);
            expect (waitUntil ([&] { return worker.status().complete; }, WORKER_TIMEOUT_MS));
            expectEquals (speech.calls->load(), (status.wordsTotal + 1) * TAKE_COUNT);
        }

        beginTest ("Hangul a voice cannot read falls back to a Korean voice");
        {
            ConfigHandoff handoff;
            CountingSpeech speech;
            SpeechWorker worker (handoff, speech.renderer());

            auto settings = defaultSettings();
            settings.countWords = koreanCountWords();
            settings.slots = {};
            settings.slots[0].name = "프리코러스";
            worker.setSettings (settings);

            expect (waitUntil ([&] { return worker.status().complete; }, WORKER_TIMEOUT_MS));
            const auto status = worker.status();
            expectEquals (status.wordsReady, 13);
            expectEquals (status.wordsFailed, 0);
            expectEquals (speech.calls->load(), 13 * TAKE_COUNT * 2);
        }

        beginTest ("A render that gets no answer in time is retried instead of failing");
        {
            ConfigHandoff handoff;
            auto timeoutsLeft = std::make_shared<std::atomic<int>> (1);
            SpeechWorker worker (handoff, [timeoutsLeft] (const SpeechRequest& request, const AbortCheck& abort)
                                              -> std::optional<RenderedSpeech>
            {
                if (timeoutsLeft->fetch_sub (1) > 0)
                    return std::nullopt; // what renderSpeech reports when the main thread was too busy

                return fakeSpeech (request, abort);
            });

            Settings settings;
            settings.slots[0].name = "Chorus";
            worker.setSettings (settings);

            expect (waitUntil ([&] { return worker.status().complete; }, WORKER_TIMEOUT_MS));
            expectEquals (worker.status().wordsReady, 1);
            expectEquals (worker.status().wordsFailed, 0);
        }

        beginTest ("Restored words are used as they are, without rendering");
        {
            ConfigHandoff sourceHandoff;
            SpeechWorker source (sourceHandoff, fakeSpeech);
            expect (waitUntil ([&] { return source.status().complete; }, WORKER_TIMEOUT_MS));

            ConfigHandoff handoff;
            CountingSpeech speech;
            SpeechWorker worker (handoff, speech.renderer());
            worker.restore (source.settings(), source.wordsInUse());

            expect (waitUntil ([&] { return worker.status().complete; }, WORKER_TIMEOUT_MS));
            expectEquals (speech.calls->load(), 0);
            expectEquals (worker.status().wordsReady, source.status().wordsReady);
        }
    }
};

static CueSequenceTests cueSequenceTests;
static TakeTests takeTests;
static HandoffTests handoffTests;
static SpeechWorkerTests speechWorkerTests;

} // namespace clickmaker::test
