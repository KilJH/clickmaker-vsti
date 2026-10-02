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
constexpr double PPQ_EPSILON = 1.0e-6;

bool sameBeats (const BarLayout& a, const BarLayout& b)
{
    if (a.numBeats != b.numBeats || std::abs (a.barPpq - b.barPpq) > PPQ_EPSILON)
        return false;

    for (int i = 0; i < a.numBeats; ++i)
        if (std::abs (a.beatPpq[(size_t) i] - b.beatPpq[(size_t) i]) > PPQ_EPSILON)
            return false;

    return true;
}
} // namespace

CueSequence buildCueSequence (const BarLayout& bar, int slot, const CueShape& shape)
{
    const int beats = bar.numBeats;
    const bool nameBar = shape.hasName && (shape.nameBeats == 0 || shape.countBars == 0);

    const auto numberAt = [&] (int index)
    {
        return shape.direction == CountDirection::down ? beats - index : index + 1;
    };

    // A name inside the count speaks over its first beats; a one-bar count always keeps its "1".
    double nameEnd = 0.0;

    if (shape.hasName && ! nameBar)
    {
        const int nameBeats = std::min (shape.nameBeats, shape.countBars == 1 ? std::max (1, beats - 1) : beats);
        nameEnd = nameBeats < beats ? bar.beatPpq[(size_t) nameBeats] : bar.barPpq;
    }

    CueSequence numbers;

    // Numbers above the spoken range or under the name stay silent but keep their place in time.
    const auto pushNumber = [&] (double ppq, double length, int number)
    {
        if (number >= 1 && number <= NUMBER_WORD_COUNT && ppq >= nameEnd - PPQ_EPSILON)
            numbers.push ({ ppq, length, numberWordId (number) });
    };

    double start = nameBar ? bar.barPpq : 0.0;

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

    CueSequence sequence;

    // Inside the count, the name may run until the first number left after it.
    if (shape.hasName)
    {
        const double nameLength = nameBar           ? bar.barPpq
                                : numbers.size > 0 ? numbers.events[0].ppq
                                                   : shape.countBars * bar.barPpq;
        sequence.push ({ 0.0, nameLength, nameWordId (slot) });
    }

    for (int i = 0; i < numbers.size; ++i)
        sequence.push (numbers.events[(size_t) i]);

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

void CueEngine::trigger (int newSlot, double notePpq, const BlockTime& time, const BarLayout& bar, const CueShape& newShape)
{
    cancel();

    const double start = cueStartPpq (notePpq, time.barStartPpq, bar.barPpq);
    schedule = buildCueSequence (bar, newSlot, newShape);

    for (int i = 0; i < schedule.size; ++i)
        schedule.events[(size_t) i].ppq += start;

    nextEvent = 0;
    earliestPpq = notePpq;
    slot = newSlot;
    shape = newShape;
    layout = bar;
    barStart = start;
    barIndex = 0;
}

void CueEngine::followBar (const BlockTime& time, const BarLayout& bar)
{
    if (nextEvent >= schedule.size)
        return;

    // Meters change only at bar lines, so each bar of the cue starts where the planned one ends.
    while (time.barStartPpq >= barStart + layout.barPpq - PPQ_EPSILON)
    {
        barStart += layout.barPpq;
        ++barIndex;
    }

    // The plan assumed every bar of the cue looks like the first; a 2/4 bar before a chorus does not.
    if (time.barStartPpq >= barStart - PPQ_EPSILON && ! sameBeats (bar, layout))
        replan (time, bar);
}

void CueEngine::replan (const BlockTime& time, const BarLayout& bar)
{
    const auto fresh = buildCueSequence (bar, slot, shape);
    const double offset = barStart - barIndex * bar.barPpq;
    const auto fired = nextEvent > 0 ? std::optional (schedule.events[(size_t) nextEvent - 1]) : std::nullopt;
    CueSequence rest;

    for (int i = 0; i < fresh.size; ++i)
    {
        auto event = fresh.events[(size_t) i];
        event.ppq += offset;

        if (event.ppq < barStart - PPQ_EPSILON)
            continue; // an earlier bar of the cue

        // Words start ahead of their beat, so the old plan may already have started this one.
        if (fired.has_value() && std::abs (fired->ppq - event.ppq) < PPQ_EPSILON)
        {
            const bool replacesWrongWord = fired->wordId != event.wordId && time.ppqStart < event.ppq + event.lengthPpq;
            if (! replacesWrongWord)
                continue;
        }
        else if (event.ppq < time.ppqStart - PPQ_EPSILON)
        {
            continue; // its beat has passed
        }

        rest.push (event);
    }

    schedule = rest;
    nextEvent = 0;
    layout = bar;
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
