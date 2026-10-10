#include "PluginEditor.h"
#include "Presets.h"

namespace friction
{

// ============================================================================ ChoiceButtons

ChoiceButtons::ChoiceButtons (juce::AudioProcessorValueTreeState& s, const char* paramId, juce::Colour onColour)
{
    auto* param = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (paramId));
    jassert (param != nullptr);
    for (int i = 0; i < param->choices.size(); ++i)
    {
        auto* b = buttons.add (new juce::TextButton (param->choices[i]));
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, onColour);
        b->onClick = [this, i] { if (attachment) attachment->setValueAsCompleteGesture ((float) i); };
        addAndMakeVisible (b);
    }
    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float v) { setFromParam (v); }, nullptr);
    attachment->sendInitialUpdate();
}

void ChoiceButtons::setFromParam (float v)
{
    current = juce::jlimit (0, buttons.size() - 1, juce::roundToInt (v));
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState (i == current, juce::dontSendNotification);
}

void ChoiceButtons::resized()
{
    auto r = getLocalBounds();
    const int w = r.getWidth() / juce::jmax (1, buttons.size());
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setBounds (i == buttons.size() - 1 ? r : r.removeFromLeft (w).withTrimmedRight (2));
}

// ============================================================================ Knob

Knob::Knob (juce::AudioProcessorValueTreeState& s, const char* id, const juce::String& caption, juce::Colour colour)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxTextColourId, theme::text);
    slider.setPopupDisplayEnabled (false, false, nullptr);
    label.setText (caption, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (theme::font (11.5f, true));
    label.setColour (juce::Label::textColourId, theme::textDim);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
    if (auto* p = s.getParameter (id))
        slider.setTooltip (p->getName (64));
}

void Knob::addTo (juce::Component& parent)
{
    parent.addAndMakeVisible (slider);
    parent.addAndMakeVisible (label);
}

void Knob::setBounds (juce::Rectangle<int> r)
{
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

void Knob::setVisible (bool v)
{
    slider.setVisible (v);
    label.setVisible (v);
}

// ============================================================================ editor

FrictionAudioProcessorEditor::FrictionAudioProcessorEditor (FrictionAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      modeButtons (p.state(), ids::mode, theme::accent),
      latencyButtons (p.state(), ids::latency, theme::reference),
      referenceButtons (p.state(), ids::reference, theme::reference),
      advisorPanel (p),
      amount (p.state(), ids::amount, "AMOUNT", theme::accent),
      sensitivity (p.state(), ids::sensitivity, "SENSITIVITY", theme::heatHigh),
      attack (p.state(), ids::attack, "ATTACK", theme::text),
      release (p.state(), ids::release, "RELEASE", theme::text),
      mix (p.state(), ids::mix, "MIX", theme::text),
      trim (p.state(), ids::trim, "TRIM", theme::text),
      rangeLow (p.state(), ids::rangeLow, "RANGE LOW", theme::reference),
      rangeHigh (p.state(), ids::rangeHigh, "RANGE HIGH", theme::reference),
      crossover (p.state(), ids::crossover, "CROSSOVER", theme::reference),
      maxCents (p.state(), ids::maxCents, "MAX CENTS", theme::shift),
      wholeOffset (p.state(), ids::wholeOffset, "OFFSET", theme::shift),
      beatRate (p.state(), ids::beatRate, "BEAT RATE", theme::tension)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (modeButtons);
    addAndMakeVisible (latencyButtons);
    addAndMakeVisible (referenceButtons);
    deltaButton.setColour (juce::ToggleButton::tickColourId, theme::gain);
    bypassButton.setColour (juce::ToggleButton::tickColourId, theme::heatHigh);
    deltaButton.setTooltip ("Delta: hear only what is removed or added (wet - dry)");
    bypassButton.setTooltip ("Bypass (latency compensated)");
    addAndMakeVisible (deltaButton);
    addAndMakeVisible (bypassButton);
    deltaAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.state(), ids::delta, deltaButton);
    bypassAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.state(), ids::bypass, bypassButton);

    const auto& presets = factoryPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
        presetBox.addItem (presets[(size_t) i].name, i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
    addAndMakeVisible (presetBox);
    presetLabel.setText ("PRESET", juce::dontSendNotification);
    presetLabel.setFont (theme::font (11.0f, true));
    presetLabel.setColour (juce::Label::textColourId, theme::textDim);
    addAndMakeVisible (presetLabel);

    map.onSelfToggled = [this] (bool s) { showSelf = s; };
    addAndMakeVisible (map);
    addAndMakeVisible (strip);
    addAndMakeVisible (clash);
    addAndMakeVisible (advisorPanel);

    for (Knob* k : { &amount, &sensitivity, &attack, &release, &mix, &trim, &rangeLow, &rangeHigh, &crossover, &maxCents, &wholeOffset, &beatRate })
        k->addTo (*this);

    auto setupCombo = [&] (juce::ComboBox& box, juce::Label& lbl, const char* id, const juce::String& caption,
                           std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& att)
    {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p.state().getParameter (id)))
            box.addItemList (c->choices, 1);
        att = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.state(), id, box);
        lbl.setText (caption, juce::dontSendNotification);
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setFont (theme::font (11.5f, true));
        lbl.setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (box);
        addAndMakeVisible (lbl);
    };
    setupCombo (focusBox, focusLabel, ids::focus, "FOCUS", focusAtt);
    setupCombo (retuneStyleBox, retuneStyleLabel, ids::retuneStyle, "RETUNE STYLE", retuneStyleAtt);
    setupCombo (tensionStyleBox, tensionStyleLabel, ids::tensionStyle, "TENSION STYLE", tensionStyleAtt);

    setResizable (true, true);
    setResizeLimits (860, 560, 2400, 1600);
    setSize (proc.editorWidth, proc.editorHeight);
    updateModeVisibility();
    startTimerHz (30);
}

FrictionAudioProcessorEditor::~FrictionAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void FrictionAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    // wordmark
    auto head = getLocalBounds().removeFromTop (48).reduced (14, 8);
    auto logo = head.removeFromLeft (150).toFloat();
    g.setColour (theme::accent);
    const float cy = logo.getCentreY();
    juce::Path waves;
    for (int k = 0; k < 2; ++k)
    {
        juce::Path w;
        for (int i = 0; i <= 24; ++i)
        {
            const float x = logo.getX() + (float) i;
            const float y = cy + std::sin ((float) i * (k == 0 ? 0.55f : 0.62f)) * 6.0f;
            if (i == 0) w.startNewSubPath (x, y); else w.lineTo (x, y);
        }
        g.setColour (k == 0 ? theme::reference : theme::heatHigh);
        g.strokePath (w, juce::PathStrokeType (2.0f));
    }
    g.setColour (theme::text);
    g.setFont (theme::font (21.0f, true));
    g.drawText ("FRICTION", logo.withTrimmedLeft (32), juce::Justification::centredLeft);
}

void FrictionAudioProcessorEditor::resized()
{
    proc.editorWidth = getWidth();
    proc.editorHeight = getHeight();

    auto r = getLocalBounds();
    auto head = r.removeFromTop (48).reduced (14, 9);
    head.removeFromLeft (160);
    modeButtons.setBounds (head.removeFromLeft (270));
    head.removeFromLeft (12);
    latencyButtons.setBounds (head.removeFromLeft (130));
    head.removeFromLeft (12);
    referenceButtons.setBounds (head.removeFromLeft (170));
    head.removeFromLeft (12);
    presetBox.setBounds (head.removeFromRight (220));
    presetLabel.setBounds (head.removeFromRight (56));
    head.removeFromRight (8);
    bypassButton.setBounds (head.removeFromRight (90));
    head.removeFromRight (6);
    deltaButton.setBounds (head.removeFromRight (84));

    auto foot = r.removeFromBottom (128).reduced (14, 6);
    auto body = r.reduced (14, 4);
    const int sideW = juce::jlimit (280, 420, getWidth() / 4);
    auto side = body.removeFromRight (sideW);
    body.removeFromRight (10);
    map.setBounds (body.removeFromTop ((int) (body.getHeight() * 0.68f)));
    body.removeFromTop (8);
    strip.setBounds (body);
    clash.setBounds (side.removeFromTop ((int) (side.getHeight() * 0.44f)));
    side.removeFromTop (8);
    advisorPanel.setBounds (side);

    // knob row
    const int nSlots = 12;
    const int slotW = foot.getWidth() / nSlots;
    auto slot = [&] () { return foot.removeFromLeft (slotW).reduced (4, 0); };
    amount.setBounds (slot());
    sensitivity.setBounds (slot());
    attack.setBounds (slot());
    release.setBounds (slot());
    mix.setBounds (slot());
    trim.setBounds (slot());
    rangeLow.setBounds (slot());
    rangeHigh.setBounds (slot());
    crossover.setBounds (slot());
    {
        auto s = slot();
        focusLabel.setBounds (s.removeFromTop (16));
        focusBox.setBounds (s.removeFromTop (26).reduced (0, 1));
    }
    // mode-specific (two slots)
    auto ms1 = slot(), ms2 = slot();
    maxCents.setBounds (ms1);
    beatRate.setBounds (ms1);
    {
        auto s = ms2;
        auto top = s.removeFromTop (60);
        retuneStyleLabel.setBounds (top.removeFromTop (16));
        retuneStyleBox.setBounds (top.removeFromTop (26).reduced (0, 1));
        tensionStyleLabel.setBounds (retuneStyleLabel.getBounds());
        tensionStyleBox.setBounds (retuneStyleBox.getBounds());
        wholeOffset.setBounds (s.withTrimmedTop (0));
    }
    updateModeVisibility();
}

void FrictionAudioProcessorEditor::updateModeVisibility()
{
    const int mode = modeButtons.selected();
    const bool track = latencyButtons.selected() == 1;
    const bool retune = mode == 1 && ! track;
    const bool tension = mode == 2 && ! track;
    maxCents.setVisible (retune || (tension && false));
    retuneStyleBox.setVisible (retune);
    retuneStyleLabel.setVisible (retune);
    beatRate.setVisible (tension);
    tensionStyleBox.setVisible (tension);
    tensionStyleLabel.setVisible (tension);
    // the offset knob shares space under the style box: only meaningful for Whole Fixed
    const bool fixed = retune && retuneStyleBox.getSelectedId() == 3;
    wholeOffset.setVisible (fixed);
    if (wholeOffset.slider.isVisible())
    {
        auto b = retuneStyleBox.getBounds();
        wholeOffset.slider.setBounds (b.getX(), b.getBottom() + 2, b.getWidth(), juce::jmax (20, getHeight() - b.getBottom() - 10));
        wholeOffset.label.setVisible (false);
        wholeOffset.slider.setSliderStyle (juce::Slider::LinearBar);
    }
    modeButtons.setEnabled (true);
}

void FrictionAudioProcessorEditor::refreshNow()
{
    timerCallback();
}

void FrictionAudioProcessorEditor::timerCallback()
{
    auto& s = proc.state();
    if (proc.engine().display().fetch())
    {
        frame = proc.engine().display().front();
        map.setOverlays (s.getRawParameterValue (ids::rangeLow)->load(), s.getRawParameterValue (ids::rangeHigh)->load(),
                         s.getRawParameterValue (ids::crossover)->load(), showSelf || frame.selfAnalysis);
        map.setFrame (frame);
        strip.push (frame, showSelf || frame.selfAnalysis);
        clash.setFrame (frame, showSelf, proc.cpuLoad.load());
    }
    if (++advisorTick >= 6)
    {
        advisorTick = 0;
        advisorPanel.update();
    }
    const int mode = modeButtons.selected(), lat = latencyButtons.selected();
    if (mode != lastMode || lat != lastLatency)
    {
        lastMode = mode;
        lastLatency = lat;
        updateModeVisibility();
    }
    if (presetBox.getSelectedId() != proc.getCurrentProgram() + 1)
        presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    proc.syncLatency();
}

} // namespace friction
