#pragma once

#include "SpeechSynth.h"
#include "VoiceBank.h"

#include <mutex>

namespace clickmaker
{

constexpr int MAX_TEXT_LENGTH = 40;

struct VoiceChoice
{
    std::string id; // all empty: the system default voice
    std::string name;
    std::string language;

    bool operator== (const VoiceChoice&) const = default;
};

struct SlotSetting
{
    std::string name;
    CountMode countMode = CountMode::useDefault;

    bool operator== (const SlotSetting&) const = default;
};

// Everything that is not an automatable parameter.
struct Settings
{
    VoiceChoice voice;
    int ratePercent = DEFAULT_RATE_PERCENT;
    std::array<std::string, NUMBER_WORD_COUNT> countWords;
    std::array<SlotSetting, SLOT_COUNT> slots;
    std::string accentGroups;

    bool operator== (const Settings&) const = default;
};

inline const juce::Identifier SETTINGS_TREE_TYPE { "SETTINGS" };

Settings defaultSettings();
std::array<std::string, NUMBER_WORD_COUNT> englishCountWords();
std::array<std::string, NUMBER_WORD_COUNT> koreanCountWords();
std::string cleanText (const juce::String&); // trims and limits user text
juce::ValueTree settingsToTree (const Settings&);
Settings settingsFromTree (const juce::ValueTree&);

enum class WordState { unused, pending, ready, failed };

struct SpeechStatus
{
    int wordsTotal = 0;
    int wordsReady = 0;
    int wordsFailed = 0;
    bool complete = false; // every take rendered and published
    std::array<WordState, WORD_COUNT> words {};
};

using SpeechRenderer = std::function<std::optional<RenderedSpeech> (const SpeechRequest&, const AbortCheck&)>;

// Owns the settings and the rendered words. Publishes a fresh EngineConfig immediately on every change,
// then renders missing words in the background once edits have settled.
class SpeechWorker final : private juce::Thread
{
public:
    SpeechWorker (ConfigHandoff&, SpeechRenderer);
    ~SpeechWorker() override;

    std::uint64_t setSettings (Settings);
    void restore (Settings, WordCache);
    Settings settings() const;
    std::uint64_t revision() const;
    std::pair<Settings, std::uint64_t> snapshot() const; // both under one lock, for change detection
    WordCache wordsInUse() const;
    SpeechStatus status() const;

private:
    struct RequiredWord
    {
        int wordId = 0;
        WordKey key;
        int priority = 0; // 0 first: names and the numbers of a 4/4 count
    };

    struct Job
    {
        WordKey key;
        int step = 0;
    };

    struct RenderOutcome
    {
        std::optional<WordTake> take;
        bool retryable = false; // no answer in time (e.g. a busy main thread), as opposed to a voice that cannot read the text
    };

    void run() override;
    void publish();
    RenderOutcome renderTake (const Job&, const VoiceChoice&, std::uint64_t revisionAtStart);
    std::optional<Job> nextJob() const;
    static std::vector<RequiredWord> requiredWords (const Settings&);

    ConfigHandoff& handoff;
    SpeechRenderer renderer;

    mutable std::mutex mutex;
    Settings current;
    WordCache cache;
    std::uint64_t settingsRevision = 1;
    std::uint64_t publishedRevision = 0;
    std::uint32_t lastChangeMs = 0;
    bool cacheChanged = false;

    JUCE_DECLARE_NON_COPYABLE (SpeechWorker)
};

} // namespace clickmaker
