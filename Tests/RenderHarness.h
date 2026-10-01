#pragma once

#include "PluginProcessor.h"

#include <limits>

namespace clickmaker::test
{

constexpr double TEST_SAMPLE_RATE = 48000.0;
constexpr int LEFT = 0;
constexpr int RIGHT = 1;

struct NoteEvent
{
    double ppq = 0.0;
    int note = FIRST_SLOT_NOTE;
};

struct JumpEvent
{
    double atPpq = 0.0;
    double toPpq = 0.0;
};

struct MeterChange
{
    double atPpq = 0.0; // must fall on a bar line of the previous meter
    Meter meter;
};

// A scripted host timeline: tempo and meter change only at block boundaries, like a real host.
struct Scenario
{
    juce::String name;
    double sampleRate = TEST_SAMPLE_RATE;
    std::vector<int> blockSizes { 512 };
    double startPpq = 0.0;
    double lengthPpq = 16.0; // timeline time to render, including stopped time
    std::function<double (double ppq)> bpmAt = [] (double) { return 120.0; };
    std::vector<MeterChange> meters { { 0.0, {} } };
    std::vector<NoteEvent> notes;
    std::vector<JumpEvent> jumps;
    bool playing = true;
    double stopAtPpq = std::numeric_limits<double>::infinity();
    bool provideBarStart = true;
    double jitterPpq = 0.0;
};

struct BlockLog
{
    int startSample = 0;
    int numSamples = 0;
    double ppqStart = 0.0;
    double ppqPerSample = 0.0;
    bool playing = false;
};

struct RenderResult
{
    juce::AudioBuffer<float> audio;
    std::vector<BlockLog> blocks;

    // Exact output sample of a timeline position (the first time it is played).
    double sampleAtPpq (double ppq) const;
};

RenderResult renderScenario (juce::AudioProcessor&, const Scenario&);

Meter meterAt (const Scenario&, double ppq);
double barStartAt (const Scenario&, double ppq);

std::vector<int> findOnsets (const float* samples, int numSamples, float threshold, int minimumGap);
double estimateFrequency (const float* samples, int numSamples, double sampleRate);
int countNonSilent (const float* samples, int numSamples, float threshold);
void writeWav (const juce::AudioBuffer<float>&, double sampleRate, const juce::String& name);

// Deterministic speech: a short high "consonant" and then a sine "vowel" whose pitch names the word.
std::optional<RenderedSpeech> fakeSpeech (const SpeechRequest&, const AbortCheck&);
double fakeFrequencyFor (const std::string& text);
WordTake fakeTake (const std::string& text, float rate);

bool waitUntil (const std::function<bool()>& condition, int timeoutMs);
void setParameter (ClickMakerProcessor&, const char* id, float plainValue);

} // namespace clickmaker::test
