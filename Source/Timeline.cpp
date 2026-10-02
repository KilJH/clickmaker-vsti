#include "Timeline.h"

#include <cmath>

namespace clickmaker
{

namespace
{
constexpr double MIN_BPM = 10.0;
constexpr double MAX_BPM = 999.0;
constexpr int MAX_NUMERATOR = 32;
constexpr int MAX_DENOMINATOR = 32;
constexpr double PPQ_EPSILON = 1.0e-6;
constexpr double DISCONTINUITY_SECONDS = 0.005;
constexpr double CUE_BREAK_PPQ = 0.25;
constexpr double LATE_NOTE_PPQ = 0.25; // a 16th
constexpr int COMPOUND_UNITS_PER_BEAT = 3;

bool isValidMeter (int numerator, int denominator)
{
    const bool powerOfTwo = denominator > 0 && (denominator & (denominator - 1)) == 0;
    return numerator >= 1 && numerator <= MAX_NUMERATOR && powerOfTwo && denominator <= MAX_DENOMINATOR;
}

bool isCompound (Meter meter)
{
    return meter.denominator >= 8 && meter.numerator > 3 && meter.numerator % COMPOUND_UNITS_PER_BEAT == 0;
}
} // namespace

int GroupPattern::total() const
{
    int sum = 0;
    for (const auto size : sizes)
        sum += size;
    return sum;
}

std::vector<GroupPattern> parseGroupPatterns (const juce::String& text)
{
    std::vector<GroupPattern> patterns;

    for (const auto& token : juce::StringArray::fromTokens (text, ",;", ""))
    {
        GroupPattern pattern;

        for (const auto& part : juce::StringArray::fromTokens (token, "+", ""))
        {
            const auto digits = part.trim();
            const auto size = digits.getIntValue();

            if (digits.isEmpty() || ! digits.containsOnly ("0123456789") || size < 1)
            {
                pattern.sizes.clear();
                break;
            }

            pattern.sizes.push_back (size);
        }

        // A single group would mean one beat per bar, which the plain meter already covers.
        if (pattern.sizes.size() >= 2 && pattern.total() <= MAX_NUMERATOR)
            patterns.push_back (std::move (pattern));
    }

    return patterns;
}

BarLayout makeBarLayout (Meter meter, const BeatOptions& options)
{
    BarLayout layout;
    const double unit = 4.0 / meter.denominator;
    layout.barPpq = meter.numerator * unit;

    for (const auto& pattern : options.groups)
    {
        if (pattern.total() != meter.numerator)
            continue;

        layout.numBeats = (int) pattern.sizes.size();
        int position = 0;

        for (size_t i = 0; i < pattern.sizes.size(); ++i)
        {
            layout.beatPpq[i] = position * unit;
            position += pattern.sizes[i];
        }

        return layout;
    }

    const int unitsPerBeat = options.compound && isCompound (meter) ? COMPOUND_UNITS_PER_BEAT : 1;
    layout.numBeats = meter.numerator / unitsPerBeat;

    for (int i = 0; i < layout.numBeats; ++i)
        layout.beatPpq[(size_t) i] = i * unitsPerBeat * unit;

    return layout;
}

double cueStartPpq (double notePpq, double barStartPpq, double barPpq)
{
    const double sinceBarStart = notePpq - LATE_NOTE_PPQ - barStartPpq;
    return barStartPpq + std::ceil ((sinceBarStart - PPQ_EPSILON) / barPpq) * barPpq;
}

void TransportClock::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    previewing = false;
    wasPlaying = false;
    wasPreviewing = false;
}

void TransportClock::startPreview()
{
    // Auditioning during host playback would cancel the cue that is actually playing.
    if (wasPlaying && ! wasPreviewing)
        return;

    previewing = true;
    restartRequested = true;
    previewPpq = 0.0;
}

BlockTime TransportClock::next (const juce::Optional<juce::AudioPlayHead::PositionInfo>& host, int numSamples)
{
    const bool hostPlaying = host.hasValue() && host->getIsPlaying();

    if (hostPlaying)
        previewing = false;

    if (host.hasValue())
    {
        if (const auto hostBpm = host->getBpm(); hostBpm.hasValue() && *hostBpm >= MIN_BPM && *hostBpm <= MAX_BPM)
            bpm = *hostBpm;

        if (const auto signature = host->getTimeSignature();
            signature.hasValue() && isValidMeter (signature->numerator, signature->denominator))
            meter = { signature->numerator, signature->denominator };
    }

    BlockTime time;
    time.bpm = bpm;
    time.meter = meter;
    time.ppqPerSample = bpm / (60.0 * sampleRate);
    const double barPpq = meter.barPpq();

    if (hostPlaying)
    {
        time.isPlaying = true;
        const auto ppq = host->getPpqPosition();
        time.ppqStart = ppq.hasValue() ? *ppq : (wasPlaying ? expectedPpq : 0.0);

        // The host's bar start carries the history of meter changes, so prefer it when it is consistent.
        if (const auto hostBar = host->getPpqPositionOfLastBarStart();
            hostBar.hasValue() && *hostBar <= time.ppqStart + PPQ_EPSILON)
            time.barStartPpq = *hostBar + std::floor ((time.ppqStart - *hostBar + PPQ_EPSILON) / barPpq) * barPpq;
        else
            time.barStartPpq = std::floor (time.ppqStart / barPpq) * barPpq;
    }
    else if (previewing)
    {
        time.isPlaying = true;
        time.isPreview = true;
        time.ppqStart = previewPpq;
        time.barStartPpq = std::floor (previewPpq / barPpq) * barPpq;
    }

    const bool stateChanged = time.isPlaying != wasPlaying || previewing != wasPreviewing || restartRequested;
    const double drift = std::abs (time.ppqStart - expectedPpq);

    time.justStarted = time.isPlaying && stateChanged;
    time.discontinuity = stateChanged || (time.isPlaying && drift > DISCONTINUITY_SECONDS * bpm / 60.0);
    time.cueBreak = stateChanged || (time.isPlaying && drift >= CUE_BREAK_PPQ);

    expectedPpq = time.ppqStart + numSamples * time.ppqPerSample;

    if (previewing)
        previewPpq = expectedPpq;

    wasPlaying = time.isPlaying;
    wasPreviewing = previewing;
    restartRequested = false;
    return time;
}

} // namespace clickmaker
