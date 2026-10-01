#include "SpeechWorker.h"

namespace clickmaker
{

namespace
{
constexpr std::uint32_t RENDER_DEBOUNCE_MS = 600;
constexpr int STOP_TIMEOUT_MS = 4000;
constexpr int RETRY_DELAY_MS = 2000;
constexpr int MAX_TIMEOUTS = 3;
constexpr int PRIORITY_NUMBER_LIMIT = 4;
constexpr int PRIORITY_CLASSES = 2;
constexpr const char* KOREAN_LANGUAGE = "ko-KR";

// Added to the base rate for each take; the speed-up per step depends on the voice, so takes are
// measured and sorted afterwards rather than assumed.
constexpr std::array<float, TAKE_COUNT> TAKE_RATE_OFFSETS { 0.0f, 0.05f, 0.12f };

const juce::Identifier COUNT_TYPE ("COUNT");
const juce::Identifier SLOT_TYPE ("SLOT");
const juce::Identifier VOICE_ID_PROPERTY ("voiceId");
const juce::Identifier VOICE_NAME_PROPERTY ("voiceName");
const juce::Identifier VOICE_LANGUAGE_PROPERTY ("voiceLanguage");
const juce::Identifier RATE_PROPERTY ("rate");
const juce::Identifier GROUPS_PROPERTY ("accentGroups");
const juce::Identifier INDEX_PROPERTY ("index");
const juce::Identifier TEXT_PROPERTY ("text");
const juce::Identifier MODE_PROPERTY ("mode");

juce::String toJuce (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

bool containsHangul (const std::string& text)
{
    for (auto pointer = juce::CharPointer_UTF8 (text.c_str()); ! pointer.isEmpty(); ++pointer)
    {
        const auto c = (juce::uint32) *pointer;
        if ((c >= 0xac00 && c <= 0xd7a3) || (c >= 0x1100 && c <= 0x11ff) || (c >= 0x3130 && c <= 0x318f))
            return true;
    }

    return false;
}
} // namespace

std::array<std::string, NUMBER_WORD_COUNT> englishCountWords()
{
    return { "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven", "Twelve" };
}

std::array<std::string, NUMBER_WORD_COUNT> koreanCountWords()
{
    return { "하나", "둘", "셋", "넷", "다섯", "여섯", "일곱", "여덟", "아홉", "열", "열하나", "열둘" };
}

Settings defaultSettings()
{
    Settings settings;
    settings.countWords = englishCountWords();

    // White keys only, so the defaults are easy to hit in the piano roll; C1 stays a count-only cue.
    const std::array<std::pair<int, const char*>, 13> names { {
        { 2, "Intro" }, { 4, "Verse" }, { 5, "Pre-Chorus" }, { 7, "Chorus" }, { 9, "Bridge" },
        { 11, "Interlude" }, { 12, "Solo" }, { 14, "Instrumental" }, { 16, "Tag" }, { 17, "Outro" },
        { 19, "Ending" }, { 21, "Last Chorus" }, { 23, "Breakdown" },
    } };

    for (const auto& [slot, name] : names)
        settings.slots[(size_t) slot].name = name;

    return settings;
}

std::string cleanText (const juce::String& text)
{
    return text.trim().substring (0, MAX_TEXT_LENGTH).trim().toStdString();
}

juce::ValueTree settingsToTree (const Settings& settings)
{
    juce::ValueTree tree (SETTINGS_TREE_TYPE);
    tree.setProperty (VOICE_ID_PROPERTY, toJuce (settings.voice.id), nullptr);
    tree.setProperty (VOICE_NAME_PROPERTY, toJuce (settings.voice.name), nullptr);
    tree.setProperty (VOICE_LANGUAGE_PROPERTY, toJuce (settings.voice.language), nullptr);
    tree.setProperty (RATE_PROPERTY, settings.ratePercent, nullptr);
    tree.setProperty (GROUPS_PROPERTY, toJuce (settings.accentGroups), nullptr);

    for (int i = 0; i < NUMBER_WORD_COUNT; ++i)
    {
        juce::ValueTree word (COUNT_TYPE);
        word.setProperty (INDEX_PROPERTY, i, nullptr);
        word.setProperty (TEXT_PROPERTY, toJuce (settings.countWords[(size_t) i]), nullptr);
        tree.appendChild (word, nullptr);
    }

    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        juce::ValueTree slot (SLOT_TYPE);
        slot.setProperty (INDEX_PROPERTY, i, nullptr);
        slot.setProperty (TEXT_PROPERTY, toJuce (settings.slots[(size_t) i].name), nullptr);
        slot.setProperty (MODE_PROPERTY, (int) settings.slots[(size_t) i].countMode, nullptr);
        tree.appendChild (slot, nullptr);
    }

    return tree;
}

Settings settingsFromTree (const juce::ValueTree& tree)
{
    Settings settings;

    if (! tree.hasType (SETTINGS_TREE_TYPE))
        return defaultSettings();

    settings.voice = { tree[VOICE_ID_PROPERTY].toString().toStdString(),
                       tree[VOICE_NAME_PROPERTY].toString().toStdString(),
                       tree[VOICE_LANGUAGE_PROPERTY].toString().toStdString() };
    settings.ratePercent = juce::jlimit (MIN_RATE_PERCENT, MAX_RATE_PERCENT, (int) tree.getProperty (RATE_PROPERTY, DEFAULT_RATE_PERCENT));
    settings.accentGroups = cleanText (tree[GROUPS_PROPERTY].toString());

    for (const auto& child : tree)
    {
        const int index = child[INDEX_PROPERTY];
        const auto text = cleanText (child[TEXT_PROPERTY].toString());

        if (child.hasType (COUNT_TYPE) && index >= 0 && index < NUMBER_WORD_COUNT)
            settings.countWords[(size_t) index] = text;

        if (child.hasType (SLOT_TYPE) && index >= 0 && index < SLOT_COUNT)
        {
            const int mode = juce::jlimit ((int) CountMode::useDefault, (int) CountMode::twoBars, (int) child[MODE_PROPERTY]);
            settings.slots[(size_t) index] = { text, (CountMode) mode };
        }
    }

    return settings;
}

SpeechWorker::SpeechWorker (ConfigHandoff& configHandoff, SpeechRenderer speechRenderer)
    : juce::Thread ("ClickMaker speech"),
      handoff (configHandoff),
      renderer (std::move (speechRenderer)),
      current (defaultSettings()),
      lastChangeMs (juce::Time::getMillisecondCounter())
{
    startThread (juce::Thread::Priority::low);
}

SpeechWorker::~SpeechWorker()
{
    signalThreadShouldExit();
    notify();
    stopThread (STOP_TIMEOUT_MS);
}

std::uint64_t SpeechWorker::setSettings (Settings next)
{
    std::uint64_t revisionNow = 0;

    {
        const std::scoped_lock lock (mutex);

        if (next == current)
            return settingsRevision;

        current = std::move (next);
        revisionNow = ++settingsRevision;
        lastChangeMs = juce::Time::getMillisecondCounter();
    }

    notify();
    return revisionNow;
}

void SpeechWorker::restore (Settings next, WordCache restored)
{
    {
        // One lock for both, so a publish in between cannot prune the restored words.
        const std::scoped_lock lock (mutex);
        current = std::move (next);

        for (auto& [key, word] : restored)
            cache[key] = std::move (word);

        ++settingsRevision;
        lastChangeMs = juce::Time::getMillisecondCounter();
    }

    notify();
}

Settings SpeechWorker::settings() const
{
    const std::scoped_lock lock (mutex);
    return current;
}

std::uint64_t SpeechWorker::revision() const
{
    const std::scoped_lock lock (mutex);
    return settingsRevision;
}

std::pair<Settings, std::uint64_t> SpeechWorker::snapshot() const
{
    const std::scoped_lock lock (mutex);
    return { current, settingsRevision };
}

WordCache SpeechWorker::wordsInUse() const
{
    const std::scoped_lock lock (mutex);
    WordCache used;

    for (const auto& word : requiredWords (current))
        if (const auto found = cache.find (word.key); found != cache.end() && found->second.takes != nullptr)
            used.insert (*found);

    return used;
}

SpeechStatus SpeechWorker::status() const
{
    const std::scoped_lock lock (mutex);
    SpeechStatus status;

    for (const auto& word : requiredWords (current))
    {
        auto state = WordState::pending;

        if (const auto found = cache.find (word.key); found != cache.end())
            state = found->second.takes != nullptr ? WordState::ready
                  : found->second.failed           ? WordState::failed
                                                   : WordState::pending;

        status.words[(size_t) word.wordId] = state;
        ++status.wordsTotal;
        status.wordsReady += state == WordState::ready ? 1 : 0;
        status.wordsFailed += state == WordState::failed ? 1 : 0;
    }

    status.complete = publishedRevision == settingsRevision && ! cacheChanged && ! nextJob().has_value();
    return status;
}

std::vector<SpeechWorker::RequiredWord> SpeechWorker::requiredWords (const Settings& settings)
{
    std::vector<RequiredWord> words;

    for (int number = 1; number <= NUMBER_WORD_COUNT; ++number)
        if (const auto& text = settings.countWords[(size_t) number - 1]; ! text.empty())
            words.push_back ({ numberWordId (number), { text, settings.voice.id, settings.ratePercent },
                               number <= PRIORITY_NUMBER_LIMIT ? 0 : 1 });

    for (int slot = 0; slot < SLOT_COUNT; ++slot)
        if (const auto& text = settings.slots[(size_t) slot].name; ! text.empty())
            words.push_back ({ nameWordId (slot), { text, settings.voice.id, settings.ratePercent }, 0 });

    return words;
}

std::optional<SpeechWorker::Job> SpeechWorker::nextJob() const
{
    const auto words = requiredWords (current);

    // Every word's natural take first, so cues become usable quickly; faster takes follow.
    for (int step = 0; step < TAKE_COUNT; ++step)
        for (int priority = 0; priority < PRIORITY_CLASSES; ++priority)
            for (const auto& word : words)
            {
                if (word.priority != priority)
                    continue;

                const auto found = cache.find (word.key);
                const int stepsDone = found != cache.end() ? found->second.stepsDone : 0;
                const bool failed = found != cache.end() && found->second.failed;

                if (! failed && stepsDone == step)
                    return Job { word.key, step };
            }

    return std::nullopt;
}

void SpeechWorker::run()
{
    while (! threadShouldExit())
    {
        Settings snapshot;
        std::uint64_t revisionNow = 0;
        std::uint32_t changedAt = 0;
        bool needsPublish = false;

        {
            const std::scoped_lock lock (mutex);
            snapshot = current;
            revisionNow = settingsRevision;
            changedAt = lastChangeMs;
            needsPublish = revisionNow != publishedRevision || cacheChanged;
        }

        // Publishing never waits for speech: names, count modes and groups apply immediately.
        if (needsPublish)
        {
            publish();
            continue;
        }

        const auto quietFor = juce::Time::getMillisecondCounter() - changedAt;

        if (quietFor < RENDER_DEBOUNCE_MS)
        {
            wait ((int) (RENDER_DEBOUNCE_MS - quietFor));
            continue;
        }

        std::optional<Job> job;
        {
            const std::scoped_lock lock (mutex);
            if (settingsRevision == revisionNow)
                job = nextJob();
        }

        if (! job.has_value())
        {
            wait (-1);
            continue;
        }

        auto outcome = renderTake (*job, snapshot.voice, revisionNow);

        if (threadShouldExit())
            break;

        bool retryLater = false;
        {
            const std::scoped_lock lock (mutex);

            // An edit or a restored state interrupted the render; publish that first.
            if (! outcome.take.has_value() && settingsRevision != revisionNow)
                continue;

            auto& entry = cache[job->key];

            if (outcome.take.has_value())
            {
                auto takes = entry.takes != nullptr ? std::make_shared<WordTakes> (*entry.takes) : std::make_shared<WordTakes>();
                addTake (*takes, std::move (*outcome.take));
                entry.takes = std::move (takes);
                entry.stepsDone = job->step + 1;
            }
            else if (outcome.retryable && ++entry.timeouts < MAX_TIMEOUTS)
            {
                retryLater = true;
            }
            else if (job->step == 0)
            {
                entry.failed = true;
            }
            else
            {
                entry.stepsDone = TAKE_COUNT; // faster takes are optional; stop trying
            }

            cacheChanged = true;
        }

        if (retryLater)
            wait (RETRY_DELAY_MS);
    }
}

SpeechWorker::RenderOutcome SpeechWorker::renderTake (const Job& job, const VoiceChoice& voice, std::uint64_t revisionAtStart)
{
    const float rate = std::min (1.0f, (float) job.key.ratePercent / 100.0f + TAKE_RATE_OFFSETS[(size_t) job.step]);

    // A render can block for seconds while the host's main thread is busy; settings must not wait for it.
    const auto abort = [this, revisionAtStart]
    {
        const std::scoped_lock lock (mutex);
        return threadShouldExit() || settingsRevision != revisionAtStart;
    };

    const auto render = [&] (const SpeechRequest& request) -> RenderOutcome
    {
        const auto rendered = renderer (request, abort);

        if (! rendered.has_value())
            return { std::nullopt, true };

        return { prepareTake (rendered->samples, rendered->sampleRate), false };
    };

    auto outcome = render ({ job.key.text, voice.id, voice.name, voice.language, rate });

    // A voice that cannot read Hangul renders silence rather than an error.
    if (! outcome.take.has_value() && ! outcome.retryable && containsHangul (job.key.text) && ! abort())
        outcome = render ({ job.key.text, {}, {}, KOREAN_LANGUAGE, rate });

    return outcome;
}

void SpeechWorker::publish()
{
    auto config = std::make_unique<EngineConfig>();
    Settings settings;
    std::uint64_t revisionNow = 0;

    {
        // Settings and cache are read together so pruning can never drop words restore() just added.
        const std::scoped_lock lock (mutex);
        settings = current;
        revisionNow = settingsRevision;
        const auto words = requiredWords (settings);

        for (const auto& word : words)
            if (const auto found = cache.find (word.key); found != cache.end())
                config->words[(size_t) word.wordId] = found->second.takes;

        // Drop words the current settings no longer use.
        std::erase_if (cache, [&] (const auto& entry)
        {
            return std::none_of (words.begin(), words.end(), [&] (const auto& word) { return word.key == entry.first; });
        });
    }

    for (int slot = 0; slot < SLOT_COUNT; ++slot)
    {
        const auto& setting = settings.slots[(size_t) slot];
        config->countModes[(size_t) slot] = setting.countMode;

        if (! setting.name.empty())
            config->namedSlots |= 1u << slot;
    }

    config->groups = parseGroupPatterns (toJuce (settings.accentGroups));
    handoff.publish (std::move (config));

    // Only this thread sets cacheChanged, so clearing it after the handoff cannot lose an update.
    const std::scoped_lock lock (mutex);
    publishedRevision = revisionNow;
    cacheChanged = false;
}

} // namespace clickmaker
