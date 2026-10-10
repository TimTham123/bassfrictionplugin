#include "FrictionLookAndFeel.h"

namespace friction
{

FrictionLookAndFeel::FrictionLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, theme::background);
    setColour (juce::Slider::textBoxTextColourId, theme::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, theme::text);
    setColour (juce::ComboBox::backgroundColourId, theme::panelRaised);
    setColour (juce::ComboBox::outlineColourId, theme::outline);
    setColour (juce::ComboBox::textColourId, theme::text);
    setColour (juce::ComboBox::arrowColourId, theme::textDim);
    setColour (juce::PopupMenu::backgroundColourId, theme::panelRaised);
    setColour (juce::PopupMenu::textColourId, theme::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, theme::text);
    setColour (juce::TextButton::buttonColourId, theme::panelRaised);
    setColour (juce::TextButton::buttonOnColourId, theme::accent);
    setColour (juce::TextButton::textColourOffId, theme::textDim);
    setColour (juce::TextButton::textColourOnId, theme::background);
    setColour (juce::ToggleButton::textColourId, theme::text);
    setColour (juce::TooltipWindow::backgroundColourId, theme::panelRaised);
    setColour (juce::TooltipWindow::textColourId, theme::text);
    setColour (juce::TooltipWindow::outlineColourId, theme::outline);
}

void FrictionLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float startAngle,
                                            float endAngle, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float thick = juce::jmax (2.5f, r * 0.13f);
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float zeroAngle = bipolar ? startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle) : startAngle;

    juce::Path track;
    track.addCentredArc (c.x, c.y, r - thick, r - thick, 0.0f, startAngle, endAngle, true);
    g.setColour (theme::outline);
    g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path val;
    val.addCentredArc (c.x, c.y, r - thick, r - thick, 0.0f, juce::jmin (zeroAngle, angle), juce::jmax (zeroAngle, angle), true);
    const juce::Colour col = s.findColour (juce::Slider::rotarySliderFillColourId);
    g.setColour (s.isEnabled() ? col : col.withAlpha (0.35f));
    g.strokePath (val, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float knobR = r - thick * 2.2f;
    g.setColour (theme::panelRaised);
    g.fillEllipse (c.x - knobR, c.y - knobR, knobR * 2.0f, knobR * 2.0f);
    g.setColour (theme::outline);
    g.drawEllipse (c.x - knobR, c.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.0f);
    juce::Path ptr;
    ptr.addRoundedRectangle (-1.5f, -knobR + 3.0f, 3.0f, knobR * 0.45f, 1.5f);
    g.setColour (theme::text.withAlpha (s.isEnabled() ? 0.9f : 0.4f));
    g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void FrictionLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    juce::Colour fill = on ? b.findColour (juce::TextButton::buttonOnColourId) : b.findColour (juce::TextButton::buttonColourId);
    if (highlighted && ! on) fill = fill.brighter (0.08f);
    if (down) fill = fill.darker (0.1f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (on ? fill.brighter (0.2f) : theme::outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void FrictionLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                     .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
}

juce::Font FrictionLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
{
    return theme::font (juce::jlimit (11.0f, 16.0f, (float) h * 0.48f), true);
}

void FrictionLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const juce::Colour onCol = b.findColour (juce::ToggleButton::tickColourId);
    g.setColour (on ? onCol.withAlpha (0.22f) : (highlighted ? theme::panelRaised.brighter (0.05f) : theme::panelRaised));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (on ? onCol : theme::outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    const float d = juce::jmin (8.0f, r.getHeight() * 0.3f);
    g.setColour (on ? onCol : theme::textDim.withAlpha (0.5f));
    g.fillEllipse (r.getX() + 8.0f, r.getCentreY() - d * 0.5f, d, d);
    g.setColour (on ? theme::text : theme::textDim);
    g.setFont (theme::font (juce::jlimit (11.0f, 15.0f, r.getHeight() * 0.48f), true));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (14.0f + d).toNearestInt(), juce::Justification::centredLeft, 1);
}

void FrictionLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    juce::Path arrow;
    const float ax = (float) w - 14.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.fillPath (arrow);
}

juce::Font FrictionLookAndFeel::getComboBoxFont (juce::ComboBox& b)
{
    return theme::font (juce::jlimit (11.0f, 15.0f, (float) b.getHeight() * 0.5f));
}

juce::Label* FrictionLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (theme::font (12.0f));
    l->setJustificationType (juce::Justification::centred);
    l->setColour (juce::Label::textColourId, theme::text);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    return l;
}

void FrictionLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (theme::panelRaised);
    g.setColour (theme::outline);
    g.drawRect (0, 0, w, h);
}

} // namespace friction
