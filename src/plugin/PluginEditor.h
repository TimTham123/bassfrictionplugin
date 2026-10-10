// Friction - editor
#pragma once

#include "PluginProcessor.h"
#include "ui/FrictionLookAndFeel.h"
#include "ui/FrictionMap.h"
#include "ui/SidePanels.h"

namespace friction
{

/** Segmented control bound to a choice parameter. */
class ChoiceButtons : public juce::Component
{
public:
    ChoiceButtons (juce::AudioProcessorValueTreeState& s, const char* paramId, juce::Colour onColour);
    void resized() override;
    int selected() const noexcept { return current; }

private:
    void setFromParam (float v);
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    int current = 0;
};

/** Rotary knob with a caption, bound to a float parameter. */
struct Knob
{
    Knob (juce::AudioProcessorValueTreeState& s, const char* id, const juce::String& caption, juce::Colour colour);
    void addTo (juce::Component& parent);
    void setBounds (juce::Rectangle<int> r);
    void setVisible (bool v);

    juce::Slider slider;
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class FrictionAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit FrictionAudioProcessorEditor (FrictionAudioProcessor&);
    ~FrictionAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** For tests/screenshots: pull the latest display frame and advisor state now. */
    void refreshNow();

private:
    void timerCallback() override;
    void updateModeVisibility();

    FrictionAudioProcessor& proc;
    FrictionLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    ChoiceButtons modeButtons, latencyButtons, referenceButtons;
    juce::ToggleButton deltaButton { "Delta" }, bypassButton { "Bypass" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deltaAtt, bypassAtt;
    juce::ComboBox presetBox;
    juce::Label presetLabel;

    FrictionMap map;
    HistoryStrip strip;
    ClashPanel clash;
    AdvisorPanel advisorPanel;

    Knob amount, sensitivity, attack, release, mix, trim, rangeLow, rangeHigh, crossover, maxCents, wholeOffset, beatRate;
    juce::ComboBox focusBox, retuneStyleBox, tensionStyleBox;
    juce::Label focusLabel, retuneStyleLabel, tensionStyleLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> focusAtt, retuneStyleAtt, tensionStyleAtt;

    fr::DisplayFrame frame;
    bool showSelf = false;
    int advisorTick = 0;
    int lastMode = -1, lastLatency = -1, lastStyle = -1;
    juce::Rectangle<int> modePanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FrictionAudioProcessorEditor)
};

} // namespace friction
