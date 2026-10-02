#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace clickmaker
{

// The editor's copy of the non-automatable settings. Edits go straight to the worker;
// changes made elsewhere (project loads, presets) are noticed through the revision.
class SettingsSync
{
public:
    explicit SettingsSync (ClickMakerProcessor&);

    Settings& edit() { return local; }
    const Settings& current() const { return local; }
    void push();
    bool pullExternalChanges();

private:
    ClickMakerProcessor& processor;
    Settings local;
    std::uint64_t revision = 0;
};

class CuePanel final : public juce::Component
{
public:
    CuePanel (ClickMakerProcessor&, SettingsSync&);
    ~CuePanel() override;

    void showSettings();
    void showStatus (const SpeechStatus&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class SlotRow;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void populateVoices();
    void voiceChosen();
    void countWordsEdited();
    void useCountWords (const std::array<std::string, NUMBER_WORD_COUNT>&);
    int defaultCountBars() const;
    int defaultNameBeats() const;

    ClickMakerProcessor& owner;
    SettingsSync& sync;
    std::vector<SystemVoice> voices;

    juce::ToggleButton cueOn;
    juce::ComboBox countLength;
    juce::ComboBox direction;
    juce::ComboBox nameLength;
    juce::ComboBox voice;
    juce::Slider speed;
    juce::Slider level;
    juce::Slider pan;
    juce::TextEditor countWords;
    juce::TextButton englishWords { "EN" };
    juce::TextButton koreanWords;
    juce::Label countCaption, directionCaption, nameLengthCaption, voiceCaption, speedCaption, levelCaption, panCaption, wordsCaption;
    juce::Label noteHeader, nameHeader, lengthHeader, modeHeader, placementHeader;
    juce::Viewport slotView;
    juce::Component slotList;
    std::array<std::unique_ptr<SlotRow>, SLOT_COUNT> rows;

    std::unique_ptr<ButtonAttachment> cueOnAttachment;
    std::unique_ptr<ComboBoxAttachment> countLengthAttachment;
    std::unique_ptr<ComboBoxAttachment> directionAttachment;
    std::unique_ptr<ComboBoxAttachment> nameLengthAttachment;
    std::unique_ptr<SliderAttachment> levelAttachment;
    std::unique_ptr<SliderAttachment> panAttachment;
};

} // namespace clickmaker
