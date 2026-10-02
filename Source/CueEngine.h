#pragma once

#include "Timeline.h"
#include "VoiceBank.h"

namespace clickmaker
{

constexpr int MAX_CUE_EVENTS = 40;

enum class CountDirection { down, up };

struct CueShape
{
    bool hasName = false;
    int countBars = 1; // 0 = name only, 1 = "4", 2 = "8"
    CountDirection direction = CountDirection::down;
    int nameBeats = 0; // 0: "Chorus | 4 3 2 1"; 1: "Chorus 3 2 1"; 2: "Chorus . 2 1"
};

struct CueEvent
{
    double ppq = 0.0; // relative to the cue start until the cue is scheduled
    double lengthPpq = 0.0;
    int wordId = 0;
};

struct CueSequence
{
    std::array<CueEvent, MAX_CUE_EVENTS> events {};
    int size = 0;

    void push (const CueEvent& event)
    {
        if (size < MAX_CUE_EVENTS)
            events[(size_t) size++] = event;
    }
};

CueSequence buildCueSequence (const BarLayout&, int slot, const CueShape&);

struct TakeChoice
{
    const WordTake* take = nullptr;
    double firePpq = 0.0;
};

TakeChoice chooseTake (const WordTakes*, double beatPpq, double lengthPpq, double earliestPpq, double bpm);

// Schedules a cue in musical time and plays its words, choking the previous word on each new one.
class CueEngine
{
public:
    void prepare (double sampleRate);
    void trigger (int slot, double notePpq, const BlockTime&, const BarLayout&, const CueShape&);
    void followBar (const BlockTime&, const BarLayout&); // once per block, before render
    void cancel();
    void render (float* bus, int from, int to, const BlockTime&, const EngineConfig*);

    bool isSounding() const { return voices[0].take != nullptr || voices[1].take != nullptr; }
    bool isActive() const { return isSounding() || nextEvent < schedule.size; }
    int currentSlot() const { return isActive() ? slot : NO_SLOT; }

private:
    struct WordVoice
    {
        const WordTake* take = nullptr;
        double position = 0.0;
        double step = 1.0;
        float gain = 1.0f;
        float fadeStep = 0.0f;
    };

    void replan (const BlockTime&, const BarLayout&);
    void startWord (const WordTake&);
    void renderVoices (float* bus, int from, int to);
    static void renderVoice (WordVoice&, float* bus, int from, int to);

    double sampleRate = 44100.0;
    float chokeStep = 0.0f;
    CueSequence schedule;
    int nextEvent = 0;
    double earliestPpq = 0.0;
    int slot = NO_SLOT;
    CueShape shape;
    BarLayout layout;        // the bar the remaining events were planned with
    double barStart = 0.0;   // start of the cue's bar the playhead is in, or heading for
    int barIndex = 0;        // which bar of the cue that is
    bool onBarLines = false; // the cue starts on a downbeat, so its bars are the host's bars
    std::array<WordVoice, 2> voices; // [0] current word, [1] the word being choked
};

} // namespace clickmaker
