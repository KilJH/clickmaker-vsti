#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <span>
#include <vector>

namespace clickmaker
{

constexpr int MAX_BEATS_PER_BAR = 32;
constexpr double DEFAULT_BPM = 120.0;

struct Meter
{
    int numerator = 4;
    int denominator = 4;

    double barPpq() const { return numerator * 4.0 / denominator; }
    bool operator== (const Meter&) const = default;
};

// A user grouping such as 2+2+3, counted in the meter's denominator unit.
struct GroupPattern
{
    std::vector<int> sizes;

    int total() const;
};

std::vector<GroupPattern> parseGroupPatterns (const juce::String& text);

struct BeatOptions
{
    bool compound = true;
    std::span<const GroupPattern> groups;
};

// Beat positions inside one bar. In grouped meters a beat is the start of a group.
struct BarLayout
{
    double barPpq = 4.0;
    int numBeats = 4;
    std::array<double, MAX_BEATS_PER_BAR> beatPpq {};

    double beatEndPpq (int beat) const { return beat + 1 < numBeats ? beatPpq[(size_t) beat + 1] : barPpq; }
};

BarLayout makeBarLayout (Meter, const BeatOptions&);
double snapToNearestBeat (double ppq, double barStartPpq, const BarLayout&);

struct BlockTime
{
    bool isPlaying = false;     // host transport or preview clock is running
    bool isPreview = false;
    bool discontinuity = false; // this block does not continue the previous one
    bool cueBreak = false;      // the jump is large enough to invalidate a scheduled cue
    bool justStarted = false;
    double bpm = DEFAULT_BPM;
    double ppqStart = 0.0;
    double ppqPerSample = 0.0;
    double barStartPpq = 0.0;
    Meter meter;

    double ppqAt (int sampleOffset) const { return ppqStart + sampleOffset * ppqPerSample; }
};

// Turns host positions (or a synthetic preview clock) into validated block timing.
class TransportClock
{
public:
    void prepare (double sampleRate);
    BlockTime next (const juce::Optional<juce::AudioPlayHead::PositionInfo>& host, int numSamples);

    void startPreview();
    void stopPreview() { previewing = false; }
    bool isPreviewing() const { return previewing; }

private:
    double sampleRate = 44100.0;
    double bpm = DEFAULT_BPM;
    Meter meter;
    bool previewing = false;
    bool restartRequested = false;
    bool wasPlaying = false;
    bool wasPreviewing = false;
    double expectedPpq = 0.0;
    double previewPpq = 0.0;
};

} // namespace clickmaker
