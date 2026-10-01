#pragma once

#include "CuePanel.h"

namespace clickmaker
{

class BeatLights final : public juce::Component
{
public:
    void show (const BeatDisplay&);
    void paint (juce::Graphics&) override;

private:
    BeatDisplay shown;
};

class ClickMakerEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ClickMakerEditor (ClickMakerProcessor&);
    ~ClickMakerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void timerCallback() override;
    void layoutClickPanel (juce::Rectangle<int>);
    void showGroups();
    void showSpeechStatus();
    juce::String cueName (int slot) const;

    ClickMakerProcessor& owner;
    theme::LookAndFeel lookAndFeel; // declared before every component so it outlives them
    SettingsSync sync;

    BeatLights beatLights;
    juce::Label title, cueNow, speechStatus, help;
    juce::ToggleButton splitOutputs;

    juce::ToggleButton clickOn, swingOn, accentOn, compound;
    juce::ComboBox sound, grid;
    juce::Slider swingAmount, decay, level, pan;
    std::array<juce::Slider, 3> pitches;
    std::array<juce::Slider, 3> gains;
    std::array<juce::Label, 3> toneCaptions;
    juce::Label soundCaption, gridCaption, groupsCaption, groupsHint, pitchHeader, gainHeader;
    juce::Label decayCaption, levelCaption, panCaption;
    juce::TextEditor groups;

    CuePanel cuePanel;
    juce::Rectangle<int> clickPanelBounds;
    int ticks = 0;

    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;
    std::vector<std::unique_ptr<ComboBoxAttachment>> comboAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClickMakerEditor)
};

} // namespace clickmaker
