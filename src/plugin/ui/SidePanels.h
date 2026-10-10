// Friction - side panels: worst-clash readout and Tuning Advisor
#pragma once

#include "Advisor.h"
#include "DisplayData.h"
#include "Theme.h"

namespace friction
{

class FrictionAudioProcessor;

class ClashPanel : public juce::Component
{
public:
    void setFrame (const fr::DisplayFrame& f, bool showSelf, float cpu);
    void paint (juce::Graphics&) override;

    /** "Target 112.4 Hz vs. Ref 110.0 Hz | +37 cents | 2.4 Hz beat" */
    static juce::String clashText (const fr::Clash& c, bool self);

private:
    fr::DisplayFrame frame;
    bool hasFrame = false, self = false;
    float cpuLoad = 0.0f;
    float meterSm = 0.0f;
};

class AdvisorPanel : public juce::Component
{
public:
    explicit AdvisorPanel (FrictionAudioProcessor& p);
    void update();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Rectangle<float> curveArea() const;

    FrictionAudioProcessor& proc;
    juce::TextButton learnButton { "Learn" };
    juce::TextButton applyButton { "Apply" };
    juce::TextButton applyLowButton { "Apply kick/low" };
    fr::AdvisorResult result;
    double seconds = 0.0;
    bool finalResult = false;
    bool hasResult = false;
    bool learning = false;
};

} // namespace friction
