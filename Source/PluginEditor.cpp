#include "PluginEditor.h"

#include "ParamIds.h"

namespace clickmaker
{

namespace
{
constexpr int EDITOR_WIDTH = 1060;
constexpr int EDITOR_HEIGHT = 660;
constexpr int MARGIN = 12;
constexpr int HEADER_HEIGHT = 48;
constexpr int FOOTER_HEIGHT = 38;
constexpr int CLICK_PANEL_WIDTH = 400;
constexpr int TITLE_WIDTH = 150;
constexpr int LIGHTS_WIDTH = 250;
constexpr int STATUS_WIDTH = 270;
constexpr int SPLIT_WIDTH = 190;
constexpr int CAPTION_WIDTH = 48;
constexpr int TOGGLE_WIDTH = 100;
constexpr int SWING_TOGGLE_WIDTH = 70;
constexpr int GROUPS_CAPTION_WIDTH = 70;
constexpr int DISPLAY_RATE_HZ = 20;
constexpr int STATUS_EVERY_TICKS = 5; // 4 Hz is plenty for render progress
constexpr int MAX_LIGHTS = 16;
constexpr float TITLE_SIZE = 22.0f;
constexpr float LIGHT_FILL = 0.55f;
constexpr float DIM_ALPHA = 0.15f;

const char* const HELP_TEXT =
    "노트(C1~B2)는 큐가 시작될 마디 첫 박의 16분 앞에 짧게 찍어야 첫 단어가 박에 맞습니다. "
    "'노트 위치'는 섹션 몇 마디 전인지 알려주고, 마디 중간에 누르면 다음 마디부터 나옵니다.\n"
    "트랙에 곡 전체를 덮는 MIDI 리전 1개를 두세요 · 정지 중 미리듣기는 트랙을 선택한 상태에서 · "
    "큐를 따로 받으려면 Multi Output으로 넣고 '큐 분리 출력'을 켜세요.";
} // namespace

void BeatLights::show (const BeatDisplay& display)
{
    if (display == shown)
        return;

    shown = display;
    repaint();
}

void BeatLights::paint (juce::Graphics& g)
{
    const int count = juce::jlimit (1, MAX_LIGHTS, shown.beatsPerBar);
    const float spacing = (float) getWidth() / (float) MAX_LIGHTS;
    const float size = std::min (spacing, (float) getHeight()) * LIGHT_FILL;

    for (int i = 0; i < count; ++i)
    {
        const auto colour = i == 0 ? theme::CLICK_ACCENT : theme::TEXT;
        g.setColour (i == shown.beat ? colour : colour.withAlpha (DIM_ALPHA));
        g.fillEllipse (juce::Rectangle<float> (size, size).withCentre ({ spacing * ((float) i + 0.5f), (float) getHeight() * 0.5f }));
    }
}

ClickMakerEditor::ClickMakerEditor (ClickMakerProcessor& processorToUse)
    : AudioProcessorEditor (processorToUse),
      owner (processorToUse),
      sync (processorToUse),
      cuePanel (processorToUse, sync)
{
    using namespace theme;
    setLookAndFeel (&lookAndFeel);
    auto& state = owner.parameters;

    const auto attachSlider = [&] (juce::Slider& slider, const char* id, const char* suffix)
    {
        setupSlider (slider, CLICK_ACCENT, suffix);
        addAndMakeVisible (slider);
        sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, id, slider));
    };
    const auto attachToggle = [&] (juce::ToggleButton& button, const char* text, const char* id)
    {
        button.setButtonText (utf8 (text));
        addAndMakeVisible (button);
        buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, id, button));
    };
    const auto attachCombo = [&] (juce::ComboBox& combo, const juce::StringArray& items, const char* id)
    {
        combo.addItemList (items, 1);
        addAndMakeVisible (combo);
        comboAttachments.push_back (std::make_unique<ComboBoxAttachment> (state, id, combo));
    };
    const auto caption = [&] (juce::Label& label, const char* text)
    {
        setupCaption (label, utf8 (text));
        addAndMakeVisible (label);
    };

    title.setText ("ClickMaker", juce::dontSendNotification);
    title.setFont (juce::FontOptions (TITLE_SIZE, juce::Font::bold));
    addAndMakeVisible (title);
    addAndMakeVisible (beatLights);

    cueNow.setFont (juce::FontOptions (TEXT_SIZE, juce::Font::bold));
    cueNow.setColour (juce::Label::textColourId, CUE_ACCENT);
    addAndMakeVisible (cueNow);

    speechStatus.setFont (juce::FontOptions (SMALL_TEXT_SIZE));
    speechStatus.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (speechStatus);
    attachToggle (splitOutputs, "큐 분리 출력 (Out 3-4)", param::CUE_TO_AUX);

    attachToggle (clickOn, "클릭 켜기", param::CLICK_ON);
    caption (soundCaption, "사운드");
    attachCombo (sound, { "Beep", "Wood", "Cowbell" }, param::CLICK_SOUND);
    caption (gridCaption, "쪼개기");
    attachCombo (grid, { utf8 ("박"), utf8 ("8분"), utf8 ("8분 셋잇단"), utf8 ("16분"), utf8 ("16분 셋잇단") }, param::CLICK_GRID);
    attachToggle (swingOn, "스윙", param::SWING_ON);
    attachSlider (swingAmount, param::SWING_AMOUNT, " %");
    attachToggle (accentOn, "강세", param::ACCENT_ON);
    attachToggle (compound, "6/8 · 12/8은 점4분이 한 박", param::COMPOUND);

    caption (groupsCaption, "강세 그룹");
    groups.setTextToShowWhenEmpty (utf8 ("예: 2+2+3, 3+2"), DIM_TEXT);
    groups.setInputRestrictions (MAX_TEXT_LENGTH);
    groups.setText (juce::String::fromUTF8 (sync.current().accentGroups.c_str()), juce::dontSendNotification);
    groups.onTextChange = [this]
    {
        sync.edit().accentGroups = cleanText (groups.getText());
        sync.push();
        showGroups();
    };
    releaseFocusOnReturn (groups);
    addAndMakeVisible (groups);
    caption (groupsHint, "");

    caption (pitchHeader, "음정");
    caption (gainHeader, "볼륨");
    const std::array<const char*, 3> toneNames { "강세", "박", "쪼갬" };
    const std::array<const char*, 3> pitchIds { param::ACCENT_PITCH, param::BEAT_PITCH, param::SUB_PITCH };
    const std::array<const char*, 3> gainIds { param::ACCENT_GAIN, param::BEAT_GAIN, param::SUB_GAIN };

    for (size_t i = 0; i < toneNames.size(); ++i)
    {
        caption (toneCaptions[i], toneNames[i]);
        attachSlider (pitches[i], pitchIds[i], " Hz");
        attachSlider (gains[i], gainIds[i], ""); // the parameter writes its own unit, or Off
    }

    caption (decayCaption, "길이");
    attachSlider (decay, param::CLICK_DECAY, " ms");
    caption (levelCaption, "레벨");
    attachSlider (level, param::CLICK_LEVEL, " dB");
    caption (panCaption, "팬");
    attachSlider (pan, param::CLICK_PAN, "");

    setupCaption (help, utf8 (HELP_TEXT));
    help.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (help);
    addAndMakeVisible (cuePanel);

    showGroups();
    showSpeechStatus();
    setSize (EDITOR_WIDTH, EDITOR_HEIGHT);
    addMouseListener (this, true);
    startTimerHz (DISPLAY_RATE_HZ);
}

ClickMakerEditor::~ClickMakerEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void ClickMakerEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::BACKGROUND);
    theme::paintPanel (g, clickPanelBounds, "CLICK", theme::CLICK_ACCENT);
}

void ClickMakerEditor::resized()
{
    auto area = getLocalBounds().reduced (MARGIN);
    auto header = area.removeFromTop (HEADER_HEIGHT);
    title.setBounds (header.removeFromLeft (TITLE_WIDTH));
    splitOutputs.setBounds (header.removeFromRight (SPLIT_WIDTH));
    speechStatus.setBounds (header.removeFromRight (STATUS_WIDTH).withTrimmedRight (theme::GAP * 2));
    beatLights.setBounds (header.removeFromLeft (LIGHTS_WIDTH).reduced (0, theme::GAP));
    cueNow.setBounds (header.withTrimmedLeft (theme::GAP * 2));

    help.setBounds (area.removeFromBottom (FOOTER_HEIGHT));
    area.removeFromBottom (theme::GAP);

    clickPanelBounds = area.removeFromLeft (CLICK_PANEL_WIDTH);
    area.removeFromLeft (theme::GAP + 4);
    cuePanel.setBounds (area);
    layoutClickPanel (clickPanelBounds);
}

void ClickMakerEditor::mouseDown (const juce::MouseEvent& event)
{
    // A clicked button keeps Return and a clicked combo box keeps the arrow keys, which Logic needs to go to
    // the start and to change tracks. JUCE has already moved the focus by now, so only text fields keep it.
    const auto* clicked = event.eventComponent;
    const bool inTextField = dynamic_cast<const juce::TextEditor*> (clicked) != nullptr
                          || clicked->findParentComponentOfClass<juce::TextEditor>() != nullptr;

    if (auto* focused = getCurrentlyFocusedComponent(); ! inTextField && focused != nullptr && isParentOf (focused))
        focused->giveAwayKeyboardFocus();
}

void ClickMakerEditor::layoutClickPanel (juce::Rectangle<int> bounds)
{
    using namespace theme;
    auto area = bounds.reduced (PADDING);
    area.removeFromTop (TITLE_HEIGHT);

    const auto nextRow = [&area]
    {
        auto row = area.removeFromTop (ROW_HEIGHT);
        area.removeFromTop (GAP - 2);
        return row;
    };

    auto first = nextRow();
    clickOn.setBounds (first.removeFromLeft (TOGGLE_WIDTH));
    soundCaption.setBounds (first.removeFromLeft (CAPTION_WIDTH));
    sound.setBounds (first);

    auto second = nextRow();
    second.removeFromLeft (TOGGLE_WIDTH);
    gridCaption.setBounds (second.removeFromLeft (CAPTION_WIDTH));
    grid.setBounds (second);

    auto third = nextRow();
    swingOn.setBounds (third.removeFromLeft (SWING_TOGGLE_WIDTH));
    swingAmount.setBounds (third);

    auto fourth = nextRow();
    accentOn.setBounds (fourth.removeFromLeft (SWING_TOGGLE_WIDTH));
    compound.setBounds (fourth);

    auto fifth = nextRow();
    groupsCaption.setBounds (fifth.removeFromLeft (GROUPS_CAPTION_WIDTH));
    groups.setBounds (fifth);
    groupsHint.setBounds (nextRow().withTrimmedLeft (GROUPS_CAPTION_WIDTH));

    area.removeFromTop (GAP / 2);
    auto header = nextRow().withTrimmedLeft (CAPTION_WIDTH);
    pitchHeader.setBounds (header.removeFromLeft (header.getWidth() / 2));
    gainHeader.setBounds (header);

    for (size_t i = 0; i < pitches.size(); ++i)
    {
        auto row = nextRow();
        toneCaptions[i].setBounds (row.removeFromLeft (CAPTION_WIDTH));
        pitches[i].setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (GAP));
        gains[i].setBounds (row);
    }

    area.removeFromTop (GAP / 2);
    const std::array<std::pair<juce::Label*, juce::Slider*>, 3> lastRows { { { &decayCaption, &decay },
                                                                           { &levelCaption, &level },
                                                                           { &panCaption, &pan } } };
    for (const auto& [label, slider] : lastRows)
    {
        auto row = nextRow();
        label->setBounds (row.removeFromLeft (CAPTION_WIDTH));
        slider->setBounds (row);
    }
}

void ClickMakerEditor::timerCallback()
{
    const auto display = owner.beatDisplay();
    beatLights.show (display);
    cueNow.setText (display.cueSlot == NO_SLOT ? juce::String() : theme::utf8 ("▶ ") + cueName (display.cueSlot),
                    juce::dontSendNotification);

    if (++ticks % STATUS_EVERY_TICKS != 0)
        return;

    // A project load or preset change replaced the settings while the editor was open.
    if (sync.pullExternalChanges())
    {
        cuePanel.showSettings();

        if (! groups.hasKeyboardFocus (true))
            groups.setText (juce::String::fromUTF8 (sync.current().accentGroups.c_str()), juce::dontSendNotification);

        showGroups();
    }

    showSpeechStatus();
}

void ClickMakerEditor::showGroups()
{
    using theme::utf8;
    const auto patterns = parseGroupPatterns (groups.getText());

    if (patterns.empty())
    {
        groupsHint.setText (groups.isEmpty() ? utf8 ("홀수 박자를 그룹으로 나눠 강세 (예: 7/8 = 2+2+3)")
                                             : utf8 ("형식: 2+2+3 처럼 +로 잇고, 여러 개는 쉼표로"),
                            juce::dontSendNotification);
        return;
    }

    juce::StringArray applied;
    for (const auto& pattern : patterns)
    {
        juce::StringArray sizes;
        for (const auto size : pattern.sizes)
            sizes.add (juce::String (size));

        applied.add (juce::String (pattern.total()) + utf8 ("박 마디 = ") + sizes.joinIntoString ("+"));
    }

    groupsHint.setText (applied.joinIntoString ("  ·  "), juce::dontSendNotification);
}

void ClickMakerEditor::showSpeechStatus()
{
    using theme::utf8;
    const auto status = owner.speechStatus();
    cuePanel.showStatus (status);

    if (status.wordsFailed > 0)
    {
        speechStatus.setText (utf8 ("음성 실패 ") + juce::String (status.wordsFailed) + utf8 ("개 · 음성이나 글자를 확인하세요"),
                              juce::dontSendNotification);
        speechStatus.setColour (juce::Label::textColourId, theme::FAILED);
    }
    else if (status.wordsReady < status.wordsTotal)
    {
        speechStatus.setText (utf8 ("음성 만드는 중 ") + juce::String (status.wordsReady) + "/" + juce::String (status.wordsTotal),
                              juce::dontSendNotification);
        speechStatus.setColour (juce::Label::textColourId, theme::PENDING);
    }
    else
    {
        speechStatus.setText (status.complete ? utf8 ("음성 준비됨") : utf8 ("음성 준비됨 · 빠른 버전 만드는 중"),
                              juce::dontSendNotification);
        speechStatus.setColour (juce::Label::textColourId, theme::READY);
    }
}

juce::String ClickMakerEditor::cueName (int slot) const
{
    const auto& name = sync.current().slots[(size_t) slot].name;
    return name.empty() ? theme::utf8 ("카운트") : juce::String::fromUTF8 (name.c_str());
}

} // namespace clickmaker
