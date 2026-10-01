#include "VoiceBank.h"

#include <algorithm>
#include <cmath>

namespace clickmaker
{

namespace
{
constexpr float SILENT_PEAK = 1.0e-3f;
constexpr float TRIM_THRESHOLD = 0.0056234f; // -45 dB relative to the peak
constexpr float TARGET_PEAK = 0.8912509f;    // -1 dBFS
constexpr double LEAD_KEEP_SECONDS = 0.005;
constexpr double TAIL_KEEP_SECONDS = 0.030;
constexpr double FADE_IN_SECONDS = 0.002;
constexpr double FADE_OUT_SECONDS = 0.010;
constexpr double ONSET_LOWPASS_HZ = 1000.0;  // ignores fricatives and sibilants
constexpr double ONSET_SMOOTHING_SECONDS = 0.010;
constexpr float ONSET_THRESHOLD = 0.3f;
constexpr double MIN_SHORTENING = 0.95;      // a faster take must be at least 5% shorter to be useful
constexpr double MIN_STORED_SAMPLE_RATE = 8000.0;
constexpr double MAX_STORED_SAMPLE_RATE = 192000.0;
constexpr double MAX_TAKE_SECONDS = 10.0;
constexpr float INT16_SCALE = 32767.0f;

const juce::Identifier WORD_TYPE ("WORD");
const juce::Identifier TAKE_TYPE ("TAKE");
const juce::Identifier TEXT_PROPERTY ("text");
const juce::Identifier VOICE_PROPERTY ("voice");
const juce::Identifier RATE_PROPERTY ("rate");
const juce::Identifier SAMPLE_RATE_PROPERTY ("sampleRate");
const juce::Identifier ONSET_PROPERTY ("onset");
const juce::Identifier PCM_PROPERTY ("pcm");

void applyFades (std::vector<float>& samples, double sampleRate)
{
    const auto size = (int) samples.size();
    const int fadeIn = std::min (size / 2, (int) (FADE_IN_SECONDS * sampleRate));
    const int fadeOut = std::min (size / 2, (int) (FADE_OUT_SECONDS * sampleRate));

    for (int i = 0; i < fadeIn; ++i)
        samples[(size_t) i] *= (float) i / (float) fadeIn;

    for (int i = 0; i < fadeOut; ++i)
        samples[(size_t) (size - 1 - i)] *= (float) i / (float) fadeOut;
}

int findVowelOnset (const std::vector<float>& samples, double sampleRate)
{
    const auto lowpass = (float) std::exp (-juce::MathConstants<double>::twoPi * ONSET_LOWPASS_HZ / sampleRate);
    const auto smoothing = (float) (1.0 - std::exp (-1.0 / (ONSET_SMOOTHING_SECONDS * sampleRate)));

    std::vector<float> envelope (samples.size());
    float filtered = 0.0f;
    float level = 0.0f;

    for (size_t i = 0; i < samples.size(); ++i)
    {
        filtered = (1.0f - lowpass) * samples[i] + lowpass * filtered;
        level += (std::abs (filtered) - level) * smoothing;
        envelope[i] = level;
    }

    const float threshold = *std::max_element (envelope.begin(), envelope.end()) * ONSET_THRESHOLD;
    const auto onset = std::find_if (envelope.begin(), envelope.end(), [threshold] (float e) { return e >= threshold; });
    return (int) (onset - envelope.begin());
}

juce::MemoryBlock encodePcm (const std::vector<float>& samples)
{
    juce::MemoryBlock block (samples.size() * sizeof (std::int16_t));
    auto* bytes = static_cast<std::uint8_t*> (block.getData());

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const auto value = (std::int16_t) std::lround (juce::jlimit (-1.0f, 1.0f, samples[i]) * INT16_SCALE);
        const auto bits = (std::uint16_t) value;
        bytes[2 * i] = (std::uint8_t) (bits & 0xff);
        bytes[2 * i + 1] = (std::uint8_t) (bits >> 8);
    }

    return block;
}

std::vector<float> decodePcm (const juce::MemoryBlock& block)
{
    const auto* bytes = static_cast<const std::uint8_t*> (block.getData());
    std::vector<float> samples (block.getSize() / sizeof (std::int16_t));

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const auto bits = (std::uint16_t) (bytes[2 * i] | (bytes[2 * i + 1] << 8));
        samples[i] = (float) (std::int16_t) bits / INT16_SCALE;
    }

    return samples;
}
} // namespace

std::optional<WordTake> prepareTake (std::span<const float> raw, double sampleRate)
{
    float peak = 0.0f;
    for (const auto sample : raw)
        peak = std::max (peak, std::abs (sample));

    // A silent render means the voice could not speak the text (e.g. Korean text with an English voice).
    if (peak < SILENT_PEAK || sampleRate <= 0.0)
        return std::nullopt;

    const float threshold = peak * TRIM_THRESHOLD;
    const auto audible = [threshold] (float sample) { return std::abs (sample) >= threshold; };
    const auto first = (int) (std::find_if (raw.begin(), raw.end(), audible) - raw.begin());
    const auto last = (int) (raw.rend() - std::find_if (raw.rbegin(), raw.rend(), audible)) - 1;
    const int begin = std::max (0, first - (int) (LEAD_KEEP_SECONDS * sampleRate));
    const int end = std::min ((int) raw.size(), last + 1 + (int) (TAIL_KEEP_SECONDS * sampleRate));

    WordTake take;
    take.sampleRate = sampleRate;
    take.samples.assign (raw.begin() + begin, raw.begin() + end);

    const float gain = TARGET_PEAK / peak;
    for (auto& sample : take.samples)
        sample *= gain;

    applyFades (take.samples, sampleRate);
    take.onsetFrame = findVowelOnset (take.samples, sampleRate);
    return take;
}

void addTake (WordTakes& takes, WordTake take)
{
    takes.push_back (std::move (take));
    std::sort (takes.begin(), takes.end(),
               [] (const auto& a, const auto& b) { return a.durationSeconds() > b.durationSeconds(); });

    WordTakes kept;
    for (auto& candidate : takes)
        if (kept.empty() || candidate.durationSeconds() <= kept.back().durationSeconds() * MIN_SHORTENING)
            kept.push_back (std::move (candidate));

    takes = std::move (kept);
}

const WordTakes* EngineConfig::takesFor (int wordId) const
{
    if (wordId < 0 || wordId >= WORD_COUNT)
        return nullptr;

    return words[(size_t) wordId].get();
}

ConfigHandoff::~ConfigHandoff()
{
    delete active;
    delete pending.load();
    delete retired.load();
}

void ConfigHandoff::publish (std::unique_ptr<EngineConfig> fresh)
{
    // Pending first, then retired: the next idle audio block can always adopt the new config.
    std::unique_ptr<EngineConfig> unseen { pending.exchange (fresh.release(), std::memory_order_acq_rel) };
    std::unique_ptr<EngineConfig> collected { retired.exchange (nullptr, std::memory_order_acq_rel) };
}

void ConfigHandoff::adoptPending()
{
    // The previous retiree has not been collected yet; storing over it would leak it.
    if (retired.load (std::memory_order_acquire) != nullptr)
        return;

    if (auto* fresh = pending.exchange (nullptr, std::memory_order_acq_rel))
    {
        retired.store (active, std::memory_order_release);
        active = fresh;
    }
}

juce::ValueTree wordCacheToTree (const WordCache& cache)
{
    juce::ValueTree audio (AUDIO_TREE_TYPE);

    for (const auto& [key, word] : cache)
    {
        if (word.takes == nullptr || word.takes->empty())
            continue;

        juce::ValueTree node (WORD_TYPE);
        node.setProperty (TEXT_PROPERTY, juce::String::fromUTF8 (key.text.c_str()), nullptr);
        node.setProperty (VOICE_PROPERTY, juce::String::fromUTF8 (key.voiceId.c_str()), nullptr);
        node.setProperty (RATE_PROPERTY, key.ratePercent, nullptr);

        for (const auto& take : *word.takes)
        {
            juce::ValueTree takeNode (TAKE_TYPE);
            takeNode.setProperty (SAMPLE_RATE_PROPERTY, take.sampleRate, nullptr);
            takeNode.setProperty (ONSET_PROPERTY, take.onsetFrame, nullptr);
            takeNode.setProperty (PCM_PROPERTY, encodePcm (take.samples), nullptr);
            node.appendChild (takeNode, nullptr);
        }

        audio.appendChild (node, nullptr);
    }

    return audio;
}

WordCache wordCacheFromTree (const juce::ValueTree& audio)
{
    WordCache cache;

    for (const auto& node : audio)
    {
        if (! node.hasType (WORD_TYPE))
            continue;

        auto takes = std::make_shared<WordTakes>();

        for (const auto& takeNode : node)
        {
            const double sampleRate = takeNode[SAMPLE_RATE_PROPERTY];
            const auto* pcm = takeNode[PCM_PROPERTY].getBinaryData();

            if (! takeNode.hasType (TAKE_TYPE) || pcm == nullptr || pcm->getSize() % sizeof (std::int16_t) != 0
                || sampleRate < MIN_STORED_SAMPLE_RATE || sampleRate > MAX_STORED_SAMPLE_RATE)
                continue;

            const auto frames = (int) (pcm->getSize() / sizeof (std::int16_t));

            if (frames == 0 || frames > MAX_TAKE_SECONDS * sampleRate)
                continue;

            WordTake take;
            take.sampleRate = sampleRate;
            take.samples = decodePcm (*pcm);
            take.onsetFrame = juce::jlimit (0, frames - 1, (int) takeNode[ONSET_PROPERTY]);
            takes->push_back (std::move (take));
        }

        if (takes->empty())
            continue;

        std::sort (takes->begin(), takes->end(),
                   [] (const auto& a, const auto& b) { return a.durationSeconds() > b.durationSeconds(); });

        const WordKey key { node[TEXT_PROPERTY].toString().toStdString(),
                            node[VOICE_PROPERTY].toString().toStdString(),
                            juce::jlimit (MIN_RATE_PERCENT, MAX_RATE_PERCENT, (int) node[RATE_PROPERTY]) };

        // Restored words are final: re-rendering could pick a different voice on another Mac.
        cache[key] = { std::move (takes), TAKE_COUNT, 0, false };
    }

    return cache;
}

} // namespace clickmaker
