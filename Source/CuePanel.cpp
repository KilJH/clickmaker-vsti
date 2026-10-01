#include "CuePanel.h"

#include "ParamIds.h"

namespace clickmaker
{

namespace
{
constexpr int NOTE_WIDTH = 44;
constexpr int MODE_WIDTH = 82;
constexpr int PLACEMENT_WIDTH = 70;
constexpr int DOT_WIDTH = 18;
constexpr int PREVIEW_WIDTH = 34;
constexpr int CAPTION_WIDTH = 46;
constexpr int SHORT_CAPTION_WIDTH = 30;
constexpr int SPEED_CAPTION_WIDTH = 62;
constexpr int SPEED_WIDTH = 170;
constexpr int TOGGLE_WIDTH = 96;
constexpr int COUNT_WIDTH = 130;
constexpr int DIRECTION_WIDTH = 140;
constexpr int PRESET_WIDTH = 38;
constexpr int SCROLLBAR_WIDTH = 10;
constexpr int MAX_COUNT_WORDS_LENGTH = 200;
constexpr int VOICE_DEFAULT_ID = 1;
constexpr int VOICE_MISSING_ID = 2;
constexpr int VOICE_FIRST_ID = 10;
constexpr int MIDDLE_C_OCTAVE = 3; // Logic names MIDI 60 "C3"
constexpr int ENHANCED_QUALITY = 2;
constexpr float DOT_SIZE = 9.0f;

bool isBlackKey (int note)
{
    const int pitchClass = note % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
}

juce::String fromUtf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

juce::Colour colourFor (WordState state)
{
    switch (state)
    {
        case WordState::ready:   return theme::READY;
        case WordState::pending: return theme::PENDING;
        case WordState::failed:  return theme::FAILED;
        case WordState::unused:  break;
    }

    return theme::EDGE;
}

int countBarsFor (CountMode mode, int defaultBars)
{
    switch (mode)
    {
        case CountMode::nameOnly:   return 0;
        case CountMode::oneBar:     return 1;
        case CountMode::twoBars:    return 2;
        case CountMode::useDefault: break;
    }

    return defaultBars;
}
} // namespace

SettingsSync::SettingsSync (ClickMakerProcessor& processorToUse) : processor (processorToUse)
{
    std::tie (local, revision) = processor.settingsSnapshot();
}

void SettingsSync::push()
{
    revision = processor.setSettings (local);
}

bool SettingsSync::pullExternalChanges()
{
    if (processor.settingsRevision() == revision)
        return false;

    std::tie (local, revision) = processor.settingsSnapshot();
    return true;
}

class CuePanel::SlotRow final : public juce::Component
{
public:
    SlotRow (CuePanel& panel, int slotIndex) : owner (panel), slot (slotIndex)
    {
        const int noteNumber = FIRST_SLOT_NOTE + slot;
        note.setText (juce::MidiMessage::getMidiNoteName (noteNumber, true, true, MIDDLE_C_OCTAVE), juce::dontSendNotification);
        note.setFont (juce::FontOptions (theme::SMALL_TEXT_SIZE, juce::Font::bold));
        note.setColour (juce::Label::textColourId, isBlackKey (noteNumber) ? theme::DIM_TEXT : theme::TEXT);
        addAndMakeVisible (note);

        name.setTextToShowWhenEmpty (theme::utf8 ("(카운트만)"), theme::DIM_TEXT);
        name.setInputRestrictions (MAX_TEXT_LENGTH);
        name.onTextChange = [this]
        {
            owner.sync.edit().slots[(size_t) slot].name = cleanText (name.getText());
            owner.sync.push();
        };
        addAndMakeVisible (name);

        mode.addItemList ({ theme::utf8 ("기본"), theme::utf8 ("이름만"), "4", "8" }, 1);
        mode.onChange = [this]
        {
            owner.sync.edit().slots[(size_t) slot].countMode = (CountMode) (mode.getSelectedId() - 1);
            owner.sync.push();
        };
        addAndMakeVisible (mode);

        placement.setFont (juce::FontOptions (theme::SMALL_TEXT_SIZE));
        placement.setColour (juce::Label::textColourId, theme::DIM_TEXT);
        placement.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (placement);

        preview.setButtonText (theme::utf8 ("▶"));
        preview.onClick = [this] { owner.owner.requestPreview (slot); };
        addAndMakeVisible (preview);
    }

    void showSettings (const SlotSetting& setting)
    {
        // Never overwrite what the user is typing.
        if (! name.hasKeyboardFocus (true))
            name.setText (fromUtf8 (setting.name), juce::dontSendNotification);

        mode.setSelectedId ((int) setting.countMode + 1, juce::dontSendNotification);
    }

    void showStatus (juce::Colour state, int barsBefore)
    {
        placement.setText (barsBefore > 0 ? juce::String (barsBefore) + theme::utf8 ("마디 전") : juce::String ("-"),
                           juce::dontSendNotification);

        if (state != stateColour)
        {
            stateColour = state;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (isBlackKey (FIRST_SLOT_NOTE + slot))
        {
            g.setColour (theme::BACKGROUND.withAlpha (0.4f));
            g.fillRect (getLocalBounds());
        }

        g.setColour (stateColour);
        g.fillEllipse (dotBounds.toFloat().withSizeKeepingCentre (DOT_SIZE, DOT_SIZE));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (2, 2);
        note.setBounds (area.removeFromLeft (NOTE_WIDTH));
        preview.setBounds (area.removeFromRight (PREVIEW_WIDTH));
        dotBounds = area.removeFromRight (DOT_WIDTH);
        placement.setBounds (area.removeFromRight (PLACEMENT_WIDTH));
        mode.setBounds (area.removeFromRight (MODE_WIDTH).reduced (3, 0));
        name.setBounds (area);
    }

private:
    CuePanel& owner;
    const int slot;
    juce::Label note;
    juce::TextEditor name;
    juce::ComboBox mode;
    juce::Label placement;
    juce::TextButton preview;
    juce::Rectangle<int> dotBounds;
    juce::Colour stateColour { theme::EDGE };
};

CuePanel::CuePanel (ClickMakerProcessor& processorToUse, SettingsSync& settingsSync)
    : owner (processorToUse), sync (settingsSync)
{
    using namespace theme;
    auto& state = owner.parameters;

    cueOn.setButtonText (utf8 ("큐 켜기"));
    cueOnAttachment = std::make_unique<ButtonAttachment> (state, param::CUE_ON, cueOn);

    setupCaption (countCaption, utf8 ("카운트"));
    countLength.addItemList ({ utf8 ("4박 (1마디)"), utf8 ("8박 (2마디)") }, 1);
    countLengthAttachment = std::make_unique<ComboBoxAttachment> (state, param::COUNT_LENGTH, countLength);

    setupCaption (directionCaption, utf8 ("방향"));
    direction.addItemList ({ utf8 ("4 3 2 1 (하행)"), utf8 ("1 2 3 4 (상행)") }, 1);
    directionAttachment = std::make_unique<ComboBoxAttachment> (state, param::COUNT_DIRECTION, direction);

    setupCaption (voiceCaption, utf8 ("음성"));
    populateVoices();
    voice.onChange = [this] { voiceChosen(); };

    setupCaption (speedCaption, utf8 ("말 빠르기"));
    setupSlider (speed, CUE_ACCENT);
    speed.setRange (MIN_RATE_PERCENT, MAX_RATE_PERCENT, 1.0);
    speed.onValueChange = [this]
    {
        sync.edit().ratePercent = (int) speed.getValue();
        sync.push();
    };

    setupCaption (levelCaption, utf8 ("레벨"));
    setupSlider (level, CUE_ACCENT, " dB");
    levelAttachment = std::make_unique<SliderAttachment> (state, param::CUE_LEVEL, level);

    setupCaption (panCaption, utf8 ("팬"));
    setupSlider (pan, CUE_ACCENT);
    panAttachment = std::make_unique<SliderAttachment> (state, param::CUE_PAN, pan);

    setupCaption (wordsCaption, utf8 ("숫자"));
    countWords.setInputRestrictions (MAX_COUNT_WORDS_LENGTH);
    countWords.setTextToShowWhenEmpty (utf8 ("One, Two, Three, Four ..."), DIM_TEXT);
    countWords.onTextChange = [this] { countWordsEdited(); };
    englishWords.onClick = [this] { useCountWords (englishCountWords()); };
    koreanWords.setButtonText (utf8 ("한"));
    koreanWords.onClick = [this] { useCountWords (koreanCountWords()); };

    setupCaption (noteHeader, utf8 ("노트"));
    setupCaption (nameHeader, utf8 ("이름 (비우면 카운트만)"));
    setupCaption (modeHeader, utf8 ("카운트"));
    setupCaption (placementHeader, utf8 ("노트 위치"));
    placementHeader.setJustificationType (juce::Justification::centredRight);

    for (int slot = 0; slot < SLOT_COUNT; ++slot)
    {
        rows[(size_t) slot] = std::make_unique<SlotRow> (*this, slot);
        slotList.addAndMakeVisible (*rows[(size_t) slot]);
    }

    slotView.setViewedComponent (&slotList, false);
    slotView.setScrollBarsShown (true, false);
    slotView.setScrollBarThickness (SCROLLBAR_WIDTH);

    for (auto* child : std::initializer_list<juce::Component*> {
             &cueOn, &countCaption, &countLength, &directionCaption, &direction, &voiceCaption, &voice, &speedCaption,
             &speed, &levelCaption, &level, &panCaption, &pan, &wordsCaption, &countWords, &englishWords, &koreanWords,
             &noteHeader, &nameHeader, &modeHeader, &placementHeader, &slotView })
        addAndMakeVisible (child);

    showSettings();
}

CuePanel::~CuePanel() = default;

void CuePanel::populateVoices()
{
    voices = listVoices();
    voice.clear (juce::dontSendNotification);

    const auto system = defaultVoice();
    voice.addItem (theme::utf8 ("시스템 기본") + " (" + fromUtf8 (system.name) + ")", VOICE_DEFAULT_ID);

    juce::String language;
    for (size_t i = 0; i < voices.size(); ++i)
    {
        const auto voiceLanguage = fromUtf8 (voices[i].language);
        if (voiceLanguage != language)
        {
            voice.addSectionHeading (voiceLanguage);
            language = voiceLanguage;
        }

        auto label = fromUtf8 (voices[i].name);
        if (voices[i].quality >= ENHANCED_QUALITY)
            label << theme::utf8 (" · 고품질");

        voice.addItem (label, VOICE_FIRST_ID + (int) i);
    }
}

void CuePanel::voiceChosen()
{
    const int id = voice.getSelectedId();

    if (id == VOICE_DEFAULT_ID)
        sync.edit().voice = {};
    else if (id >= VOICE_FIRST_ID)
        sync.edit().voice = { voices[(size_t) (id - VOICE_FIRST_ID)].id, voices[(size_t) (id - VOICE_FIRST_ID)].name,
                              voices[(size_t) (id - VOICE_FIRST_ID)].language };
    else
        return; // the placeholder for a voice this Mac does not have

    sync.push();
}

void CuePanel::countWordsEdited()
{
    const auto parts = juce::StringArray::fromTokens (countWords.getText(), ",", "");
    auto& words = sync.edit().countWords;

    for (int i = 0; i < NUMBER_WORD_COUNT; ++i)
        words[(size_t) i] = i < parts.size() ? cleanText (parts[i]) : std::string();

    sync.push();
}

void CuePanel::useCountWords (const std::array<std::string, NUMBER_WORD_COUNT>& words)
{
    sync.edit().countWords = words;
    sync.push();
    countWords.giveAwayKeyboardFocus();
    showSettings();
}

int CuePanel::defaultCountBars() const
{
    return juce::roundToInt (owner.parameters.getRawParameterValue (param::COUNT_LENGTH)->load()) + 1;
}

void CuePanel::showSettings()
{
    const auto& settings = sync.current();
    const auto& chosen = settings.voice;

    if (chosen.id.empty())
    {
        voice.setSelectedId (VOICE_DEFAULT_ID, juce::dontSendNotification);
    }
    else
    {
        const auto found = std::find_if (voices.begin(), voices.end(), [&] (const SystemVoice& v)
        {
            return v.id == chosen.id || (v.name == chosen.name && v.language == chosen.language);
        });

        if (found != voices.end())
        {
            voice.setSelectedId (VOICE_FIRST_ID + (int) (found - voices.begin()), juce::dontSendNotification);
        }
        else
        {
            // Saved audio still plays; only re-rendering would need the voice.
            const auto missing = fromUtf8 (chosen.name) + theme::utf8 (" (이 Mac에 없음)");

            if (voice.indexOfItemId (VOICE_MISSING_ID) >= 0)
                voice.changeItemText (VOICE_MISSING_ID, missing);
            else
                voice.addItem (missing, VOICE_MISSING_ID);

            voice.setSelectedId (VOICE_MISSING_ID, juce::dontSendNotification);
        }
    }

    speed.setValue (settings.ratePercent, juce::dontSendNotification);

    if (! countWords.hasKeyboardFocus (true))
    {
        juce::StringArray words;
        for (const auto& word : settings.countWords)
            words.add (fromUtf8 (word));

        while (! words.isEmpty() && words[words.size() - 1].isEmpty())
            words.remove (words.size() - 1);

        countWords.setText (words.joinIntoString (", "), juce::dontSendNotification);
        countWords.moveCaretToTop (false);
    }

    for (int slot = 0; slot < SLOT_COUNT; ++slot)
        rows[(size_t) slot]->showSettings (settings.slots[(size_t) slot]);
}

void CuePanel::showStatus (const SpeechStatus& status)
{
    const auto& settings = sync.current();
    const int defaultBars = defaultCountBars();

    // A count-only slot is ready once every number it may speak is ready.
    bool anyFailed = false, anyPending = false, anyReady = false;
    for (int number = 1; number <= NUMBER_WORD_COUNT; ++number)
    {
        const auto state = status.words[(size_t) numberWordId (number)];
        anyFailed = anyFailed || state == WordState::failed;
        anyPending = anyPending || state == WordState::pending;
        anyReady = anyReady || state == WordState::ready;
    }

    const auto numbers = anyFailed ? WordState::failed : anyPending ? WordState::pending
                       : anyReady  ? WordState::ready  : WordState::unused;

    for (int slot = 0; slot < SLOT_COUNT; ++slot)
    {
        const auto& setting = settings.slots[(size_t) slot];
        const bool hasName = ! setting.name.empty();
        const int barsBefore = (hasName ? 1 : 0) + countBarsFor (setting.countMode, defaultBars);
        const auto state = hasName ? status.words[(size_t) nameWordId (slot)] : numbers;
        rows[(size_t) slot]->showStatus (colourFor (state), barsBefore);
    }
}

void CuePanel::paint (juce::Graphics& g)
{
    theme::paintPanel (g, getLocalBounds(), "CUE", theme::CUE_ACCENT);
}

void CuePanel::resized()
{
    using namespace theme;
    auto area = getLocalBounds().reduced (PADDING);
    area.removeFromTop (TITLE_HEIGHT);

    const auto nextRow = [&area]
    {
        auto row = area.removeFromTop (ROW_HEIGHT);
        area.removeFromTop (GAP - 2);
        return row;
    };

    auto first = nextRow();
    cueOn.setBounds (first.removeFromLeft (TOGGLE_WIDTH));
    countCaption.setBounds (first.removeFromLeft (CAPTION_WIDTH));
    countLength.setBounds (first.removeFromLeft (COUNT_WIDTH));
    first.removeFromLeft (GAP * 2);
    directionCaption.setBounds (first.removeFromLeft (SHORT_CAPTION_WIDTH + 6));
    direction.setBounds (first.removeFromLeft (DIRECTION_WIDTH));

    auto second = nextRow();
    voiceCaption.setBounds (second.removeFromLeft (CAPTION_WIDTH));
    speed.setBounds (second.removeFromRight (SPEED_WIDTH));
    speedCaption.setBounds (second.removeFromRight (SPEED_CAPTION_WIDTH));
    voice.setBounds (second.withTrimmedRight (GAP));

    auto third = nextRow();
    levelCaption.setBounds (third.removeFromLeft (CAPTION_WIDTH));
    level.setBounds (third.removeFromLeft (third.getWidth() / 2).withTrimmedRight (GAP));
    panCaption.setBounds (third.removeFromLeft (SHORT_CAPTION_WIDTH));
    pan.setBounds (third);

    auto fourth = nextRow();
    wordsCaption.setBounds (fourth.removeFromLeft (CAPTION_WIDTH));
    koreanWords.setBounds (fourth.removeFromRight (PRESET_WIDTH));
    fourth.removeFromRight (4);
    englishWords.setBounds (fourth.removeFromRight (PRESET_WIDTH));
    countWords.setBounds (fourth.withTrimmedRight (GAP));

    area.removeFromTop (GAP / 2);
    auto header = area.removeFromTop (ROW_HEIGHT - 6);
    header.removeFromRight (SCROLLBAR_WIDTH);
    noteHeader.setBounds (header.removeFromLeft (NOTE_WIDTH + 2));
    header.removeFromRight (PREVIEW_WIDTH + DOT_WIDTH + 2);
    placementHeader.setBounds (header.removeFromRight (PLACEMENT_WIDTH));
    modeHeader.setBounds (header.removeFromRight (MODE_WIDTH).withTrimmedLeft (3));
    nameHeader.setBounds (header);

    slotView.setBounds (area);
    const int listWidth = area.getWidth() - SCROLLBAR_WIDTH;
    slotList.setSize (listWidth, ROW_HEIGHT * SLOT_COUNT);

    for (int slot = 0; slot < SLOT_COUNT; ++slot)
        rows[(size_t) slot]->setBounds (0, slot * ROW_HEIGHT, listWidth, ROW_HEIGHT);
}

} // namespace clickmaker
