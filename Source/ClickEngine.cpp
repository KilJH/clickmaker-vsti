#include "ClickEngine.h"

#include <algorithm>
#include <cmath>

namespace clickmaker
{

namespace
{
constexpr double PPQ_EPSILON = 1.0e-6;
constexpr double TICK_EPSILON = 1.0e-9;
constexpr double START_LOOKBACK_SECONDS = 0.005;
constexpr double GAP_TOLERANCE_SECONDS = 0.005;
constexpr float SILENCE_LEVEL = 1.0e-4f; // -80 dB
constexpr float DECAY_TARGET = 1.0e-3f;  // decay times are measured to -60 dB
constexpr juce::int64 NOISE_SEED = 0x436c6b4d;

struct SoundPreset
{
    double partialRatio;
    float partialGain;
    float noiseLevel;
    float noiseSeconds;
};

// Indexed by ClickSoundType.
constexpr std::array<SoundPreset, 3> PRESETS { {
    { 2.0, 0.0f, 0.0f, 0.0f },      // beep: a pure sine
    { 2.76, 0.45f, 0.5f, 0.003f },  // wood: inharmonic partial plus a short noise tick
    { 1.48, 0.8f, 0.15f, 0.002f },  // cowbell: the 808 partial ratio
} };

double gridLengthPpq (GridChoice grid)
{
    switch (grid)
    {
        case GridChoice::eighth:           return 0.5;
        case GridChoice::eighthTriplet:    return 1.0 / 3.0;
        case GridChoice::sixteenth:        return 0.25;
        case GridChoice::sixteenthTriplet: return 1.0 / 6.0;
        case GridChoice::beat:             break;
    }

    return 0.0;
}

bool isNearInteger (double value)
{
    return std::abs (value - std::round (value)) < PPQ_EPSILON;
}

int beatContaining (const BarLayout& bar, double ppq)
{
    int beat = 0;
    while (beat + 1 < bar.numBeats && ppq >= bar.beatPpq[(size_t) beat + 1] - PPQ_EPSILON)
        ++beat;
    return beat;
}
} // namespace

ClickPattern makeClickPattern (const BarLayout& bar, const GridOptions& options)
{
    ClickPattern pattern;
    pattern.barPpq = bar.barPpq;

    const auto add = [&pattern] (double ppq, TickKind kind)
    {
        if (pattern.size < MAX_TICKS_PER_BAR)
            pattern.entries[(size_t) pattern.size++] = { ppq, kind };
    };

    for (int beat = 0; beat < bar.numBeats; ++beat)
        add (bar.beatPpq[(size_t) beat], beat == 0 && options.accent ? TickKind::accent : TickKind::beat);

    const double grid = gridLengthPpq (options.grid);

    if (grid > 0.0)
    {
        const bool swingable = options.swing
                            && (options.grid == GridChoice::eighth || options.grid == GridChoice::sixteenth);
        const double swingDelay = (2.0 * options.swingRatio - 1.0) * grid;

        for (int k = 0; k * grid < bar.barPpq - PPQ_EPSILON; ++k)
        {
            double ppq = k * grid;
            const int beat = beatContaining (bar, ppq);
            const double beatStart = bar.beatPpq[(size_t) beat];
            const double stepInBeat = (ppq - beatStart) / grid;

            if (std::abs (ppq - beatStart) < PPQ_EPSILON)
                continue; // already added as a beat

            // Swing only pairs ticks that live inside one beat, so the beat itself never moves.
            const double stepsInBeat = (bar.beatEndPpq (beat) - beatStart) / grid;
            const bool evenBeat = isNearInteger (stepsInBeat) && (int) std::round (stepsInBeat) % 2 == 0;
            const bool offbeat = isNearInteger (stepInBeat) && (int) std::round (stepInBeat) % 2 == 1;

            if (swingable && evenBeat && offbeat)
                ppq += swingDelay;

            add (ppq, TickKind::sub);
        }
    }

    std::sort (pattern.entries.begin(), pattern.entries.begin() + pattern.size,
               [] (const auto& a, const auto& b) { return a.ppq < b.ppq; });
    return pattern;
}

int collectTicks (const BlockTime& time, int numSamples, const ClickPattern& pattern, TickCursor& cursor, std::span<Tick> out)
{
    if (! time.isPlaying || numSamples <= 0 || pattern.size == 0)
        return 0;

    // A tick belongs to the block whose sample it rounds to: [start - 1/2, start + n - 1/2) samples.
    const double halfSample = 0.5 * time.ppqPerSample;
    const double windowEnd = time.ppqStart + (numSamples - 0.5) * time.ppqPerSample;
    const double ppqPerSecond = time.bpm / 60.0;
    double windowStart = time.ppqStart - halfSample;

    if (time.discontinuity)
    {
        // Logic can report the first block after start slightly late; never lose the first downbeat.
        if (time.justStarted)
            windowStart -= START_LOOKBACK_SECONDS * ppqPerSecond;

        cursor.lastTickPpq = windowStart - TICK_EPSILON;
    }
    else
    {
        // Small host gaps fire late at offset 0 instead of being skipped; the cursor prevents repeats.
        windowStart -= GAP_TOLERANCE_SECONDS * ppqPerSecond;
    }

    const double scanFrom = std::max (windowStart, cursor.lastTickPpq + TICK_EPSILON);
    int count = 0;

    for (double bar = time.barStartPpq + std::floor ((scanFrom - time.barStartPpq) / pattern.barPpq) * pattern.barPpq;
         bar < windowEnd;
         bar += pattern.barPpq)
    {
        for (int i = 0; i < pattern.size; ++i)
        {
            const auto& entry = pattern.entries[(size_t) i];
            const double ppq = bar + entry.ppq;

            if (ppq < scanFrom)
                continue;

            if (ppq >= windowEnd || count == (int) out.size())
                return count;

            const auto offset = (int) std::lround ((ppq - time.ppqStart) / time.ppqPerSample);
            out[(size_t) count++] = { juce::jlimit (0, numSamples - 1, offset), entry.kind };
            cursor.lastTickPpq = ppq;
        }
    }

    return count;
}

void ClickEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    active = false;
    noise.setSeed (NOISE_SEED);
}

void ClickEngine::render (float* bus, int numSamples, std::span<const Tick> ticks, const ClickSound& sound)
{
    int position = 0;

    for (const auto& tick : ticks)
    {
        // A tick that is turned off must not cut the one before it.
        if (sound.tones[(size_t) tick.kind].gain <= 0.0f)
            continue;

        renderVoice (bus, position, tick.sampleOffset);
        position = tick.sampleOffset;
        start (tick.kind, sound);
    }

    renderVoice (bus, position, numSamples);
}

void ClickEngine::start (TickKind kind, const ClickSound& sound)
{
    const auto& preset = PRESETS[(size_t) sound.type];
    const auto& tone = sound.tones[(size_t) kind];

    active = true;
    phase1 = 0.0;
    phase2 = 0.0;
    increment1 = juce::MathConstants<double>::twoPi * tone.frequency / sampleRate;
    increment2 = increment1 * preset.partialRatio;
    partialGain = preset.partialGain;
    toneLevel = tone.gain * (1.0f - preset.noiseLevel) / (1.0f + preset.partialGain);
    noiseLevel = tone.gain * preset.noiseLevel;
    envelope = 1.0f;
    envelopeDecay = std::pow (DECAY_TARGET, 1.0f / (sound.decaySeconds * (float) sampleRate));
    noiseDecay = preset.noiseSeconds > 0.0f
                   ? std::pow (DECAY_TARGET, 1.0f / (preset.noiseSeconds * (float) sampleRate))
                   : 0.0f;
}

void ClickEngine::renderVoice (float* bus, int from, int to)
{
    if (! active)
        return;

    for (int i = from; i < to; ++i)
    {
        const auto tone = (float) std::sin (phase1) + partialGain * (float) std::sin (phase2);
        const auto click = noiseLevel * (noise.nextFloat() * 2.0f - 1.0f);
        bus[i] += envelope * toneLevel * tone + click;

        phase1 += increment1;
        phase2 += increment2;
        envelope *= envelopeDecay;
        noiseLevel *= noiseDecay;

        if (envelope < SILENCE_LEVEL)
        {
            active = false;
            return;
        }
    }
}

} // namespace clickmaker
