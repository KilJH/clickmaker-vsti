#include "RenderHarness.h"

namespace clickmaker::test
{

namespace
{
constexpr double SAMPLES_PER_BEAT_120 = 24000.0; // 120 bpm at 48 kHz

struct FiredTick
{
    int sample = 0;
    TickKind kind = TickKind::beat;
};

struct TickRun
{
    ClickPattern pattern;
    double bpm = 120.0;
    double startPpq = 0.0;
    double barStartPpq = 0.0;
    std::vector<int> blockSizes { 512 };
    int totalSamples = 0;
    double jitterPpq = 0.0;
};

// Drives collectTicks over consecutive constant-tempo blocks, like a host would.
std::vector<FiredTick> fireTicks (const TickRun& run)
{
    TickCursor cursor;
    std::array<Tick, MAX_TICKS_PER_BLOCK> ticks {};
    std::vector<FiredTick> fired;
    juce::Random jitter (7);
    const double ppqPerSample = run.bpm / (60.0 * TEST_SAMPLE_RATE);

    int sample = 0;
    for (size_t index = 0; sample < run.totalSamples; ++index)
    {
        const int numSamples = run.blockSizes[index % run.blockSizes.size()];

        BlockTime time;
        time.isPlaying = true;
        time.bpm = run.bpm;
        time.ppqPerSample = ppqPerSample;
        time.ppqStart = run.startPpq + sample * ppqPerSample + (jitter.nextDouble() * 2.0 - 1.0) * run.jitterPpq;
        time.barStartPpq = run.barStartPpq
                         + std::floor ((time.ppqStart - run.barStartPpq) / run.pattern.barPpq) * run.pattern.barPpq;
        time.discontinuity = index == 0;
        time.justStarted = index == 0;

        const int count = collectTicks (time, numSamples, run.pattern, cursor, ticks);
        for (int i = 0; i < count; ++i)
            if (sample + ticks[(size_t) i].sampleOffset < run.totalSamples) // the last block may run past the end
                fired.push_back ({ sample + ticks[(size_t) i].sampleOffset, ticks[(size_t) i].kind });

        sample += numSamples;
    }

    return fired;
}

ClickPattern patternFor (Meter meter, GridOptions grid, bool compound = true, const std::vector<GroupPattern>& groups = {})
{
    return makeClickPattern (makeBarLayout (meter, { compound, groups }), grid);
}
} // namespace

class TimelineTests final : public juce::UnitTest
{
public:
    TimelineTests() : juce::UnitTest ("Timeline", "Unit") {}

    void runTest() override
    {
        beginTest ("Bar layouts follow the meter, compound beats and groups");
        {
            const auto fourFour = makeBarLayout ({ 4, 4 }, {});
            expectEquals (fourFour.numBeats, 4);
            expectEquals (fourFour.beatPpq[3], 3.0);
            expectEquals (fourFour.barPpq, 4.0);

            const auto sixEight = makeBarLayout ({ 6, 8 }, { true, {} });
            expectEquals (sixEight.numBeats, 2);
            expectEquals (sixEight.beatPpq[1], 1.5);

            const auto sixEightSimple = makeBarLayout ({ 6, 8 }, { false, {} });
            expectEquals (sixEightSimple.numBeats, 6);
            expectEquals (sixEightSimple.beatPpq[5], 2.5);

            expectEquals (makeBarLayout ({ 12, 8 }, {}).numBeats, 4);
            expectEquals (makeBarLayout ({ 9, 16 }, {}).beatPpq[2], 1.5);
            expectEquals (makeBarLayout ({ 3, 8 }, {}).numBeats, 3);
            expectEquals (makeBarLayout ({ 2, 2 }, {}).beatPpq[1], 2.0);

            const auto groups = parseGroupPatterns ("2+2+3");
            const auto sevenEight = makeBarLayout ({ 7, 8 }, { true, groups });
            expectEquals (sevenEight.numBeats, 3);
            expectEquals (sevenEight.beatPpq[2], 2.0);
            expectEquals (sevenEight.barPpq, 3.5);
            expectEquals (sevenEight.beatEndPpq (2), 3.5);
        }

        beginTest ("Group patterns ignore invalid entries");
        {
            const auto patterns = parseGroupPatterns (" 2+2+3 , 3 + 2 ; abc, 3+, 4, 0+3");
            expectEquals ((int) patterns.size(), 2);
            expectEquals (patterns[0].total(), 7);
            expectEquals (patterns[1].total(), 5);
        }

        beginTest ("Snapping picks the nearest beat, preferring the later one on a tie");
        {
            const auto bar = makeBarLayout ({ 4, 4 }, {});
            expectEquals (snapToNearestBeat (0.1, 0.0, bar), 0.0);
            expectEquals (snapToNearestBeat (0.6, 0.0, bar), 1.0);
            expectEquals (snapToNearestBeat (3.6, 0.0, bar), 4.0);
            expectEquals (snapToNearestBeat (1.5, 0.0, bar), 2.0);
            expectEquals (snapToNearestBeat (-0.4, 0.0, bar), 0.0);
            expectEquals (snapToNearestBeat (5.2, 4.0, bar), 5.0);
        }

        beginTest ("The clock falls back to the bar grid when the host gives no bar start");
        {
            TransportClock clock;
            clock.prepare (TEST_SAMPLE_RATE);

            juce::AudioPlayHead::PositionInfo info;
            info.setIsPlaying (true);
            info.setBpm (120.0);
            info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
            info.setPpqPosition (6.5);

            const auto time = clock.next (info, 512);
            expect (time.isPlaying && time.justStarted && time.discontinuity && time.cueBreak);
            expectEquals (time.barStartPpq, 4.0);
        }

        beginTest ("The clock rejects invalid host values");
        {
            TransportClock clock;
            clock.prepare (TEST_SAMPLE_RATE);

            juce::AudioPlayHead::PositionInfo info;
            info.setIsPlaying (true);
            info.setBpm (0.0);
            info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 3 });
            info.setPpqPosition (0.0);

            const auto time = clock.next (info, 512);
            expectEquals (time.bpm, DEFAULT_BPM);
            expect (time.meter == Meter {});
        }

        beginTest ("Small position corrections keep cues; real jumps cancel them");
        {
            TransportClock clock;
            clock.prepare (TEST_SAMPLE_RATE);

            juce::AudioPlayHead::PositionInfo info;
            info.setIsPlaying (true);
            info.setBpm (120.0);
            info.setPpqPosition (0.0);
            clock.next (info, 480); // 0.02 ppq

            info.setPpqPosition (0.02 + 0.03);
            const auto corrected = clock.next (info, 480);
            expect (corrected.discontinuity);
            expect (! corrected.cueBreak);

            info.setPpqPosition (8.0);
            expect (clock.next (info, 480).cueBreak);
        }
    }
};

class ClickTickTests final : public juce::UnitTest
{
public:
    ClickTickTests() : juce::UnitTest ("Click ticks", "Unit") {}

    void runTest() override
    {
        beginTest ("Ticks land on exact samples across mixed block sizes");
        {
            TickRun run;
            run.pattern = patternFor ({ 4, 4 }, {});
            run.blockSizes = { 512, 37, 1024, 7, 333 };
            run.totalSamples = (int) (8 * SAMPLES_PER_BEAT_120);

            const auto fired = fireTicks (run);
            expectEquals ((int) fired.size(), 8);

            for (size_t k = 0; k < fired.size(); ++k)
            {
                expectEquals (fired[k].sample, (int) ((double) k * SAMPLES_PER_BEAT_120));
                expect (fired[k].kind == (k % 4 == 0 ? TickKind::accent : TickKind::beat));
            }
        }

        beginTest ("A tick on a block boundary fires exactly once");
        {
            for (const int blockSize : { 24000, 12000, 8000 })
            {
                TickRun run;
                run.pattern = patternFor ({ 4, 4 }, {});
                run.blockSizes = { blockSize };
                run.totalSamples = (int) (8 * SAMPLES_PER_BEAT_120);
                expectEquals ((int) fireTicks (run).size(), 8);
            }
        }

        beginTest ("Host position jitter neither doubles nor drops ticks");
        {
            TickRun run;
            run.pattern = patternFor ({ 4, 4 }, { GridChoice::sixteenth });
            run.blockSizes = { 64 };
            run.jitterPpq = 1.0e-7;
            run.totalSamples = (int) (8 * SAMPLES_PER_BEAT_120);

            const auto fired = fireTicks (run);
            expectEquals ((int) fired.size(), 32);

            for (size_t k = 0; k < fired.size(); ++k)
                expectWithinAbsoluteError (fired[k].sample, (int) ((double) k * SAMPLES_PER_BEAT_120 / 4), 1);
        }

        beginTest ("Swing delays only the off-beats");
        {
            TickRun eighths;
            eighths.pattern = patternFor ({ 4, 4 }, { GridChoice::eighth, true, 2.0 / 3.0 });
            eighths.totalSamples = (int) (2 * SAMPLES_PER_BEAT_120);
            const auto fired = fireTicks (eighths);
            expectEquals ((int) fired.size(), 4);
            expectEquals (fired[1].sample, 16000);
            expect (fired[1].kind == TickKind::sub);
            expectEquals (fired[2].sample, 24000);

            TickRun sixteenths;
            sixteenths.pattern = patternFor ({ 4, 4 }, { GridChoice::sixteenth, true, 2.0 / 3.0 });
            sixteenths.totalSamples = (int) SAMPLES_PER_BEAT_120;
            const auto fine = fireTicks (sixteenths);
            expectEquals ((int) fine.size(), 4);
            expectEquals (fine[1].sample, 8000);
            expectEquals (fine[2].sample, 12000);
            expectEquals (fine[3].sample, 20000);
        }

        beginTest ("Compound 6/8 clicks dotted quarters with eighth subdivisions and ignores swing");
        {
            const auto pattern = patternFor ({ 6, 8 }, { GridChoice::eighth, true, 2.0 / 3.0 });
            expectEquals (pattern.size, 6);

            const std::array<TickKind, 6> expected { TickKind::accent, TickKind::sub, TickKind::sub,
                                                     TickKind::beat, TickKind::sub, TickKind::sub };
            for (int i = 0; i < pattern.size; ++i)
            {
                expect (pattern.entries[(size_t) i].kind == expected[(size_t) i]);
                expectWithinAbsoluteError (pattern.entries[(size_t) i].ppq, i * 0.5, 1.0e-9);
            }
        }

        beginTest ("Grouped 7/8 accents group starts");
        {
            const auto groups = parseGroupPatterns ("2+2+3");
            const auto beats = patternFor ({ 7, 8 }, {}, true, groups);
            expectEquals (beats.size, 3);
            expectEquals (beats.entries[2].ppq, 2.0);

            const auto eighths = patternFor ({ 7, 8 }, { GridChoice::eighth }, true, groups);
            expectEquals (eighths.size, 7);
            const std::array<TickKind, 7> expected { TickKind::accent, TickKind::sub, TickKind::beat, TickKind::sub,
                                                     TickKind::beat, TickKind::sub, TickKind::sub };
            for (int i = 0; i < eighths.size; ++i)
                expect (eighths.entries[(size_t) i].kind == expected[(size_t) i]);
        }

        beginTest ("Accent off turns the downbeat into a normal beat");
        {
            const auto pattern = patternFor ({ 4, 4 }, { GridChoice::beat, false, 0.5, false });
            expect (pattern.entries[0].kind == TickKind::beat);
        }

        beginTest ("Negative positions (pre-roll) still land on the grid");
        {
            TickRun run;
            run.pattern = patternFor ({ 4, 4 }, {});
            run.startPpq = -4.0;
            run.totalSamples = (int) (5 * SAMPLES_PER_BEAT_120);

            const auto fired = fireTicks (run);
            expectEquals ((int) fired.size(), 5);
            expect (fired[0].kind == TickKind::accent);
            expect (fired[4].kind == TickKind::accent);
            expectEquals (fired[4].sample, (int) (4 * SAMPLES_PER_BEAT_120));
        }
    }
};

static TimelineTests timelineTests;
static ClickTickTests clickTickTests;

} // namespace clickmaker::test
