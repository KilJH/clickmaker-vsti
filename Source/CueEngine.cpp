#include "CueEngine.h"

#include <algorithm>
#include <cmath>

namespace clickmaker
{

namespace
{
constexpr double FIT_RATIO = 0.9;               // leave a little air before the next word
constexpr double MAX_ONSET_LEAD_SECONDS = 0.15;
constexpr double CHOKE_SECONDS = 0.005;
} // namespace

CueSequence buildCueSequence (const BarLayout& bar, int slot, const CueShape& shape)
{
    CueSequence sequence;
    const int beats = bar.numBeats;

    const auto numberAt = [&] (int index)
    {
        return shape.direction == CountDirection::down ? beats - index : index + 1;
    };

    // Numbers above the spoken range stay silent but keep their place in time.
    const auto pushNumber = [&] (double ppq, double length, int number)
    {
        if (number >= 1 && number <= NUMBER_WORD_COUNT)
            sequence.push ({ ppq, length, numberWordId (number) });
    };

    double start = 0.0;

    if (shape.hasName)
    {
        sequence.push ({ 0.0, bar.barPpq, nameWordId (slot) });
        start = bar.barPpq;
    }

    if (shape.countBars == 2)
    {
        // "4 . 3 . | 4 3 2 1": the lead-in bar speaks on the half-bar points of even meters.
        const std::array<int, 2> strongBeats { 0, beats / 2 };
        const int strongCount = beats % 2 == 0 && beats >= 2 ? 2 : 1;

        for (int j = 0; j < strongCount; ++j)
        {
            const double from = bar.beatPpq[(size_t) strongBeats[(size_t) j]];
            const double to = j + 1 < strongCount ? bar.beatPpq[(size_t) strongBeats[(size_t) j + 1]] : bar.barPpq;
            pushNumber (start + from, to - from, numberAt (j));
        }

        start += bar.barPpq;
    }

    if (shape.countBars >= 1)
        for (int i = 0; i < beats; ++i)
            pushNumber (start + bar.beatPpq[(size_t) i], bar.beatEndPpq (i) - bar.beatPpq[(size_t) i], numberAt (i));

    return sequence;
}

TakeChoice chooseTake (const WordTakes* takes, double beatPpq, double lengthPpq, double earliestPpq, double bpm)
{
    const double secondsPerPpq = 60.0 / bpm;
    TakeChoice choice { nullptr, std::max (beatPpq, earliestPpq) };

    if (takes == nullptr)
        return choice;

    for (const auto& take : *takes)
    {
        // Start early by the consonant so the vowel, which the ear hears as the beat, lands on it.
        const double leadPpq = std::min (take.onsetSeconds(), MAX_ONSET_LEAD_SECONDS) / secondsPerPpq;
        const double firePpq = std::max (beatPpq - leadPpq, earliestPpq);
        choice = { &take, firePpq };

        const double availableSeconds = (beatPpq + lengthPpq - firePpq) * secondsPerPpq * FIT_RATIO;
        if (take.durationSeconds() <= availableSeconds)
            break;
    }

    return choice;
}

void CueEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    chokeStep = (float) (1.0 / (CHOKE_SECONDS * sampleRate));
    voices = {};
    schedule.size = 0;
    nextEvent = 0;
    slot = NO_SLOT;
}

void CueEngine::trigger (int newSlot, double notePpq, const BlockTime& time, const BarLayout& bar, const CueShape& shape)
{
    cancel();

    const double start = snapToNearestBeat (notePpq, time.barStartPpq, bar);
    schedule = buildCueSequence (bar, newSlot, shape);

    for (int i = 0; i < schedule.size; ++i)
        schedule.events[(size_t) i].ppq += start;

    nextEvent = 0;
    earliestPpq = notePpq;
    slot = newSlot;
}

void CueEngine::cancel()
{
    if (voices[0].take != nullptr && voices[0].fadeStep == 0.0f)
        voices[0].fadeStep = chokeStep;

    schedule.size = 0;
    nextEvent = 0;
    slot = NO_SLOT;
}

void CueEngine::render (float* bus, int from, int to, const BlockTime& time, const EngineConfig* config)
{
    int position = from;

    while (time.isPlaying && nextEvent < schedule.size)
    {
        const auto& event = schedule.events[(size_t) nextEvent];
        const auto* takes = config != nullptr ? config->takesFor (event.wordId) : nullptr;
        const auto choice = chooseTake (takes, event.ppq, event.lengthPpq, earliestPpq, time.bpm);
        const double offset = (choice.firePpq - time.ppqStart) / time.ppqPerSample;

        if (offset >= to - 0.5)
            break; // fires in a later block, using that block's tempo

        const int fireAt = std::max (position, (int) std::lround (offset));
        renderVoices (bus, position, fireAt);
        position = fireAt;

        // A word without audio (still rendering or failed) stays silent; the timing is unaffected.
        if (choice.take != nullptr)
            startWord (*choice.take);

        ++nextEvent;
    }

    renderVoices (bus, position, to);
}

void CueEngine::startWord (const WordTake& take)
{
    voices[1] = voices[0];

    if (voices[1].take != nullptr && voices[1].fadeStep == 0.0f)
        voices[1].fadeStep = chokeStep;

    voices[0] = { &take, 0.0, take.sampleRate / sampleRate, 1.0f, 0.0f };
}

void CueEngine::renderVoices (float* bus, int from, int to)
{
    for (auto& voice : voices)
        renderVoice (voice, bus, from, to);
}

void CueEngine::renderVoice (WordVoice& voice, float* bus, int from, int to)
{
    if (voice.take == nullptr)
        return;

    const auto& samples = voice.take->samples;
    const auto length = (int) samples.size();
    const auto at = [&] (int index) { return index >= 0 && index < length ? samples[(size_t) index] : 0.0f; };

    for (int i = from; i < to; ++i)
    {
        const auto index = (int) voice.position;

        if (index >= length)
        {
            voice.take = nullptr;
            return;
        }

        // Catmull-Rom: takes stay at the voice's native rate, so host rate changes need no re-rendering.
        const auto t = (float) (voice.position - index);
        const float y0 = at (index - 1), y1 = at (index), y2 = at (index + 1), y3 = at (index + 2);
        const float value = y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3
                                                         + t * (3.0f * (y1 - y2) + y3 - y0)));
        bus[i] += value * voice.gain;
        voice.position += voice.step;

        if (voice.fadeStep > 0.0f)
        {
            voice.gain -= voice.fadeStep;

            if (voice.gain <= 0.0f)
            {
                voice.take = nullptr;
                return;
            }
        }
    }
}

} // namespace clickmaker
