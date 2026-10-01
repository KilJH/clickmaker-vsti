#pragma once

#include "Timeline.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace clickmaker
{

constexpr int SLOT_COUNT = 24;
constexpr int FIRST_SLOT_NOTE = 36; // C1 in Logic's naming (middle C = C3)
constexpr int NUMBER_WORD_COUNT = 12;
constexpr int WORD_COUNT = NUMBER_WORD_COUNT + SLOT_COUNT;
constexpr int NO_SLOT = -1;
constexpr int TAKE_COUNT = 3;
constexpr int MIN_RATE_PERCENT = 30;
constexpr int MAX_RATE_PERCENT = 70;
constexpr int DEFAULT_RATE_PERCENT = 50;

constexpr int numberWordId (int number) { return number - 1; }
constexpr int nameWordId (int slot) { return NUMBER_WORD_COUNT + slot; }

enum class CountMode { useDefault, nameOnly, oneBar, twoBars };

struct WordTake
{
    std::vector<float> samples;
    double sampleRate = 22050.0;
    int onsetFrame = 0; // where the vowel starts, i.e. where the listener hears the beat

    double durationSeconds() const { return (double) samples.size() / sampleRate; }
    double onsetSeconds() const { return onsetFrame / sampleRate; }
};

using WordTakes = std::vector<WordTake>; // longest (slowest) first

std::optional<WordTake> prepareTake (std::span<const float> raw, double sampleRate);
void addTake (WordTakes&, WordTake);

// Immutable snapshot read by the audio thread.
struct EngineConfig
{
    std::array<std::shared_ptr<const WordTakes>, WORD_COUNT> words;
    std::uint32_t namedSlots = 0;
    std::array<CountMode, SLOT_COUNT> countModes {};
    std::vector<GroupPattern> groups;

    const WordTakes* takesFor (int wordId) const;
    bool hasName (int slot) const { return ((namedSlots >> slot) & 1u) != 0; }
};

// Lock-free single-publisher handoff. The audio thread only moves pointers; the publisher frees them.
class ConfigHandoff
{
public:
    ConfigHandoff() = default;
    ~ConfigHandoff();

    void publish (std::unique_ptr<EngineConfig> fresh);  // publisher thread only
    void adoptPending();                                 // audio thread only, while nothing reads current()
    const EngineConfig* current() const { return active; }

private:
    EngineConfig* active = nullptr;
    std::atomic<EngineConfig*> pending { nullptr };
    std::atomic<EngineConfig*> retired { nullptr };

    static_assert (std::atomic<EngineConfig*>::is_always_lock_free);
    JUCE_DECLARE_NON_COPYABLE (ConfigHandoff)
};

// Identity of rendered audio. An empty voiceId means the system default voice.
struct WordKey
{
    std::string text;
    std::string voiceId;
    int ratePercent = DEFAULT_RATE_PERCENT;

    auto operator<=> (const WordKey&) const = default;
};

struct CachedWord
{
    std::shared_ptr<const WordTakes> takes;
    int stepsDone = 0;
    int timeouts = 0;
    bool failed = false;
};

using WordCache = std::map<WordKey, CachedWord>;

inline const juce::Identifier AUDIO_TREE_TYPE { "AUDIO" };

juce::ValueTree wordCacheToTree (const WordCache&);
WordCache wordCacheFromTree (const juce::ValueTree&);

} // namespace clickmaker
