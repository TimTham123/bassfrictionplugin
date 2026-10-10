// Friction - live friction map: target and reference spectra, friction heat, cuts and shifts
#pragma once

#include "DisplayData.h"
#include "Theme.h"

namespace friction
{

class FrictionMap : public juce::Component
{
public:
    FrictionMap();

    void setFrame (const fr::DisplayFrame& f);
    void setOverlays (float rangeLowHz, float rangeHighHz, float crossoverHz, bool showSelf);
    bool isShowingSelf() const noexcept { return showSelf; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    std::function<void (bool)> onSelfToggled;

private:
    float xOf (float hz) const noexcept;
    float yOf (float db) const noexcept;
    float hzAtX (float x) const noexcept;
    juce::Rectangle<float> plot() const noexcept;

    fr::DisplayFrame frame;
    static constexpr int N = fr::DisplayFrame::kN;
    float tSm[N] {}, rSm[N] {}, fSm[N] {}, sSm[N] {}, gSm[N] {};
    bool hasFrame = false;
    float rangeLo = 20.0f, rangeHi = 20000.0f, xover = 200.0f;
    bool showSelf = false;
    juce::Point<float> mouse { -1.0f, -1.0f };
    static constexpr float kDbTop = 0.0f, kDbBottom = -100.0f;
};

/** Scrolling friction-over-time strip (frequency vertical, time horizontal) with a total-friction line. */
class HistoryStrip : public juce::Component
{
public:
    HistoryStrip();
    void push (const fr::DisplayFrame& f, bool useSelf);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int kCols = 420;  // ~14 s at 30 frames/s
    static constexpr int kRows = 96;
    juce::Image img { juce::Image::ARGB, kCols, kRows, true };
    int writeCol = 0;
    float totals[kCols] {};
    float threshold = 0.1f;
};

} // namespace friction
