#pragma once

#include "Timeline.h"

#include <limits>

namespace clickmaker
{

constexpr int MAX_TICKS_PER_BAR = 256;
constexpr int MAX_TICKS_PER_BLOCK = 512;

enum class GridChoice { beat, eighth, eighthTriplet, sixteenth, sixteenthTriplet };
enum class TickKind { accent, beat, sub };
enum class ClickSoundType { beep, wood, cowbell };

struct GridOptions
{
    GridChoice grid = GridChoice::beat;
    bool swing = false;
    double swingRatio = 0.5; // 0.5 is straight, 2/3 is a triplet shuffle
    bool accent = true;
};

// Tick positions for one bar, relative to the bar start and sorted.
struct ClickPattern
{
    struct Entry
    {
        double ppq = 0.0;
        TickKind kind = TickKind::beat;
    };

    std::array<Entry, MAX_TICKS_PER_BAR> entries {};
    int size = 0;
    double barPpq = 4.0;
};

ClickPattern makeClickPattern (const BarLayout&, const GridOptions&);

struct Tick
{
    int sampleOffset = 0;
    TickKind kind = TickKind::beat;
};

// Remembers the last fired tick so block-boundary rounding never fires a tick twice.
struct TickCursor
{
    double lastTickPpq = -std::numeric_limits<double>::infinity();
};

int collectTicks (const BlockTime&, int numSamples, const ClickPattern&, TickCursor&, std::span<Tick> out);

struct ClickTone
{
    float frequency = 1000.0f;
    float gain = 1.0f;
};

struct ClickSound
{
    ClickSoundType type = ClickSoundType::beep;
    float decaySeconds = 0.04f;
    std::array<ClickTone, 3> tones; // indexed by TickKind
};

// A single voice restarted on every tick; a new attack masks the cut-off tail.
class ClickEngine
{
public:
    void prepare (double sampleRate);
    void render (float* bus, int numSamples, std::span<const Tick> ticks, const ClickSound&);

private:
    void start (TickKind, const ClickSound&);
    void renderVoice (float* bus, int from, int to);

    double sampleRate = 44100.0;
    bool active = false;
    double phase1 = 0.0;
    double phase2 = 0.0;
    double increment1 = 0.0;
    double increment2 = 0.0;
    float toneLevel = 0.0f;
    float partialGain = 0.0f;
    float envelope = 0.0f;
    float envelopeDecay = 0.0f;
    float noiseLevel = 0.0f;
    float noiseDecay = 0.0f;
    juce::Random noise;
};

} // namespace clickmaker
