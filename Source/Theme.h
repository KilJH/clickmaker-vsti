#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace clickmaker::theme
{

inline const juce::Colour BACKGROUND { 0xff15171c };
inline const juce::Colour PANEL { 0xff1f232b };
inline const juce::Colour FIELD { 0xff2a2f39 };
inline const juce::Colour EDGE { 0xff353b47 };
inline const juce::Colour TEXT { 0xffe8eaed };
inline const juce::Colour DIM_TEXT { 0xff8a919c };
inline const juce::Colour CLICK_ACCENT { 0xfff2a33a };
inline const juce::Colour CUE_ACCENT { 0xff4fb3ff };
inline const juce::Colour READY { 0xff4cc38a };
inline const juce::Colour PENDING { 0xffe6c35c };
inline const juce::Colour FAILED { 0xffe5534b };

constexpr int ROW_HEIGHT = 26;
constexpr int GAP = 8;
constexpr int PADDING = 14;
constexpr int TITLE_HEIGHT = 26;
constexpr float CORNER = 8.0f;
constexpr float TEXT_SIZE = 14.0f;
constexpr float SMALL_TEXT_SIZE = 12.0f;

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, BACKGROUND);
        setColour (juce::Label::textColourId, TEXT);
        setColour (juce::Slider::backgroundColourId, FIELD);
        setColour (juce::Slider::trackColourId, CLICK_ACCENT.withAlpha (0.8f));
        setColour (juce::Slider::thumbColourId, TEXT);
        setColour (juce::Slider::textBoxTextColourId, TEXT);
        setColour (juce::Slider::textBoxBackgroundColourId, FIELD);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::backgroundColourId, FIELD);
        setColour (juce::ComboBox::textColourId, TEXT);
        setColour (juce::ComboBox::outlineColourId, EDGE);
        setColour (juce::ComboBox::arrowColourId, DIM_TEXT);
        setColour (juce::PopupMenu::backgroundColourId, PANEL);
        setColour (juce::PopupMenu::textColourId, TEXT);
        setColour (juce::PopupMenu::headerTextColourId, DIM_TEXT);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, EDGE);
        setColour (juce::PopupMenu::highlightedTextColourId, TEXT);
        setColour (juce::TextEditor::backgroundColourId, FIELD);
        setColour (juce::TextEditor::textColourId, TEXT);
        setColour (juce::TextEditor::outlineColourId, EDGE);
        setColour (juce::TextEditor::focusedOutlineColourId, CUE_ACCENT);
        setColour (juce::TextEditor::highlightColourId, CUE_ACCENT.withAlpha (0.35f));
        setColour (juce::CaretComponent::caretColourId, TEXT);
        setColour (juce::ToggleButton::textColourId, TEXT);
        setColour (juce::ToggleButton::tickColourId, TEXT);
        setColour (juce::ToggleButton::tickDisabledColourId, DIM_TEXT);
        setColour (juce::TextButton::buttonColourId, FIELD);
        setColour (juce::TextButton::buttonOnColourId, EDGE);
        setColour (juce::TextButton::textColourOffId, TEXT);
        setColour (juce::TextButton::textColourOnId, TEXT);
        setColour (juce::ScrollBar::thumbColourId, EDGE);
    }
};

// JUCE's const char* constructor assumes ASCII; UI text here is UTF-8.
inline juce::String utf8 (const char* text)
{
    return juce::String::fromUTF8 (text);
}

inline void paintPanel (juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title, juce::Colour accent)
{
    g.setColour (PANEL);
    g.fillRoundedRectangle (bounds.toFloat(), CORNER);
    g.setColour (accent);
    g.setFont (juce::FontOptions (TEXT_SIZE, juce::Font::bold));
    g.drawText (title, bounds.reduced (PADDING, 0).withHeight (TITLE_HEIGHT + PADDING / 2), juce::Justification::bottomLeft);
}

inline void setupCaption (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::FontOptions (SMALL_TEXT_SIZE));
    label.setColour (juce::Label::textColourId, DIM_TEXT);
    label.setJustificationType (juce::Justification::centredLeft);
}

inline void setupSlider (juce::Slider& slider, juce::Colour track, const juce::String& suffix = {})
{
    // The value box copies these colours when it is created, so set them on the slider first.
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, FIELD);
    slider.setColour (juce::Slider::textBoxTextColourId, TEXT);
    slider.setColour (juce::Slider::trackColourId, track.withAlpha (0.8f));
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextValueSuffix (suffix);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 76, ROW_HEIGHT - 6);
}

} // namespace clickmaker::theme
