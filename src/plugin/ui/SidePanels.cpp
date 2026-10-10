#include "SidePanels.h"
#include "../PluginProcessor.h"

namespace friction
{

// ============================================================================ ClashPanel

juce::String ClashPanel::clashText (const fr::Clash& c, bool selfMode)
{
    if (! c.valid || c.roughness < 0.005f)
        return {};
    const juce::String other = selfMode ? "Self " : "Ref ";
    juce::String cents = juce::String (c.cents >= 0 ? "+" : "") + juce::String (juce::roundToInt (c.cents)) + " cents";
    return "Target " + juce::String (c.targetHz, 1) + " Hz vs. " + other + juce::String (c.refHz, 1) + " Hz | " + cents + " | "
           + (c.beatHz < 10.0f ? juce::String (c.beatHz, 1) : juce::String (juce::roundToInt (c.beatHz))) + " Hz beat";
}

void ClashPanel::setFrame (const fr::DisplayFrame& f, bool showSelf, float cpu)
{
    frame = f;
    hasFrame = true;
    self = showSelf || f.selfAnalysis;
    cpuLoad = cpu;
    const float total = self ? f.totalSelf : f.totalFriction;
    const float v = 100.0f * std::tanh (total * 0.5f);
    meterSm += (v > meterSm ? 0.5f : 0.1f) * (v - meterSm);
    repaint();
}

void ClashPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    r = r.reduced (12.0f, 10.0f);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true));
    g.drawText (self ? "WORST CLASH (SELF)" : "WORST CLASH", r.removeFromTop (16), juce::Justification::centredLeft);

    const fr::Clash& c = self ? frame.worstSelf : frame.worst;
    const juce::String txt = hasFrame ? clashText (c, self) : juce::String();
    if (txt.isEmpty())
    {
        g.setColour (theme::text);
        g.setFont (theme::font (15.0f, true));
        g.drawText ("No clash above the noise", r.removeFromTop (40), juce::Justification::centredLeft);
    }
    else
    {
        const auto parts = juce::StringArray::fromTokens (txt, "|", "");
        g.setColour (theme::text);
        g.setFont (theme::font (14.5f, true));
        g.drawFittedText (parts[0].trim(), r.removeFromTop (20).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setColour (theme::heat (c.roughness));
        g.setFont (theme::font (20.0f, true));
        g.drawFittedText (parts[1].trim() + "  |  " + parts[2].trim(), r.removeFromTop (28).toNearestInt(), juce::Justification::centredLeft, 1);
    }

    r.removeFromTop (6);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true));
    g.drawText ("TOTAL FRICTION", r.removeFromTop (16), juce::Justification::centredLeft);
    auto meter = r.removeFromTop (14);
    g.setColour (theme::panelRaised);
    g.fillRoundedRectangle (meter, 3.0f);
    juce::ColourGradient grad (theme::heatLow, meter.getX(), 0, theme::heatHigh, meter.getRight(), 0, false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (meter.withWidth (meter.getWidth() * juce::jlimit (0.0f, 1.0f, meterSm / 100.0f)), 3.0f);
    g.setColour (theme::text);
    g.setFont (theme::font (11.0f, true));
    g.drawText (juce::String (juce::roundToInt (meterSm)), meter.withTrimmedRight (4), juce::Justification::centredRight);
    r.removeFromTop (8);

    auto row = [&] (const juce::String& k, const juce::String& v, juce::Colour vc)
    {
        auto line = r.removeFromTop (17);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText (k, line, juce::Justification::centredLeft);
        g.setColour (vc);
        g.setFont (theme::font (12.0f, true));
        g.drawText (v, line, juce::Justification::centredRight);
    };

    const juce::String sc = frame.sidechain == 2 ? "Active" : (frame.sidechain == 1 ? "Silent - no processing" : "No sidechain - Self mode");
    row ("Sidechain", sc, frame.sidechain == 2 ? theme::reference : theme::accent);
    if (frame.mode == 0 || frame.latencyMode == 1)
        row ("Deepest cut", juce::String (-frame.maxGainReductionDb, 1) + " dB", theme::gain);
    else if (frame.mode == 1)
    {
        if (frame.retuneStyle == 0)
        {
            float maxShift = 0.0f;
            for (int i = 0; i < frame.numTarget; ++i)
                if (std::abs (frame.target[i].shiftCents) > std::abs (maxShift))
                    maxShift = frame.target[i].shiftCents;
            row ("Largest partial shift", juce::String (maxShift, 1) + " ct", theme::shift);
        }
        else
            row ("Whole-signal shift", juce::String (frame.wholeShiftCents, 1) + " ct", theme::shift);
    }
    else
        row ("Tension voices", juce::String (frame.synthVoices), theme::tension);

    auto f0 = [] (bool v, float hz) { return v ? juce::String (hz, 1) + " Hz" : juce::String ("-"); };
    row ("Low band f0 (target / ref)", f0 (frame.lfTargetVoiced, frame.lfTargetF0) + " / " + f0 (frame.lfRefVoiced, frame.lfRefF0), theme::text);
    row ("Latency", frame.latencySamples == 0 ? juce::String ("0 ms (Track)")
                                               : juce::String (1000.0 * frame.latencySamples / juce::jmax (1.0f, frame.sampleRate), 1) + " ms (Mix)",
         theme::text);
    row ("CPU (this instance)", juce::String (cpuLoad * 100.0f, 1) + " %", theme::text);
}

// ============================================================================ AdvisorPanel

AdvisorPanel::AdvisorPanel (FrictionAudioProcessor& p) : proc (p)
{
    learnButton.setClickingTogglesState (false);
    learnButton.setColour (juce::TextButton::buttonOnColourId, theme::heatHigh);
    learnButton.onClick = [this]
    {
        proc.setLearning (! proc.isLearning());
        update();
    };
    applyButton.onClick = [this] { proc.applyAdvisorOffset (false); };
    applyLowButton.onClick = [this] { proc.applyAdvisorOffset (true); };
    applyButton.setTooltip ("Set Retune to Whole Fixed with the best overall offset (Amount 100 %)");
    applyLowButton.setTooltip ("Set Retune to Whole Fixed with the best low-band (kick/bass) offset");
    learnButton.setTooltip ("Play the song, press Stop when done. The Advisor accumulates target-vs-reference partials.");
    addAndMakeVisible (learnButton);
    addAndMakeVisible (applyButton);
    addAndMakeVisible (applyLowButton);
}

void AdvisorPanel::update()
{
    learning = proc.isLearning();
    learnButton.setButtonText (learning ? "Stop" : "Learn");
    learnButton.setToggleState (learning, juce::dontSendNotification);
    hasResult = proc.advisor().hasResult();
    if (hasResult)
        result = proc.advisor().getResult (seconds, finalResult);
    applyButton.setEnabled (hasResult && result.valid && ! learning);
    applyLowButton.setEnabled (hasResult && result.lowValid && ! learning);
    repaint();
}

void AdvisorPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto top = r.removeFromTop (24);
    top.removeFromLeft (110);
    learnButton.setBounds (top.removeFromLeft (70));
    auto bottom = r.removeFromBottom (26);
    applyButton.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2 - 3));
    bottom.removeFromLeft (6);
    applyLowButton.setBounds (bottom);
}

juce::Rectangle<float> AdvisorPanel::curveArea() const
{
    auto r = getLocalBounds().toFloat().reduced (12.0f, 10.0f);
    r.removeFromTop (24 + 46);
    r.removeFromBottom (26 + 6 + (result.kickDetected || result.numNotes > 0 ? 50 : 0));
    return r;
}

void AdvisorPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    r = r.reduced (12.0f, 10.0f);

    auto head = r.removeFromTop (24);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true));
    g.drawText ("TUNING ADVISOR", head, juce::Justification::centredLeft);

    auto status = r.removeFromTop (46);
    g.setFont (theme::font (12.5f));
    if (learning)
    {
        g.setColour (theme::heatHigh);
        g.drawText ("Learning... " + juce::String (seconds, 1) + " s", status.removeFromTop (18), juce::Justification::centredLeft);
    }
    if (hasResult && result.valid)
    {
        char buf[64];
        fr::Advisor::formatOffset (result.bestCents, buf, sizeof (buf));
        g.setColour (theme::text);
        g.setFont (theme::font (15.0f, true));
        g.drawText ("Best offset " + juce::String (buf), status.removeFromTop (20), juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.0f));
        g.drawText ("Expected friction reduction " + juce::String (juce::roundToInt (result.reductionPct)) + " %"
                        + (finalResult ? "" : " (interim)"),
                    status.removeFromTop (16), juce::Justification::centredLeft);
    }
    else if (! learning)
    {
        g.setColour (theme::textDim);
        g.drawFittedText ("Press Learn, play the passage, then Stop. Reports the global target offset that minimises friction.",
                          status.toNearestInt(), juce::Justification::topLeft, 3);
    }

    // curve
    const auto ca = curveArea();
    g.setColour (theme::panelRaised);
    g.fillRect (ca);
    if (hasResult && result.valid && ca.getHeight() > 20.0f)
    {
        const int range = 300; // display +-300 cents around 0
        const int n = fr::AdvisorResult::kN, mid = fr::AdvisorResult::kRange;
        float maxV = 1.0f;
        for (int c = -range; c <= range; ++c)
        {
            maxV = std::max (maxV, result.curve[mid + c]);
            if (result.lowValid)
                maxV = std::max (maxV, result.lowCurve[mid + c]);
        }
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (ca.toNearestInt());
        auto plotCurve = [&] (const float* curve, juce::Colour col)
        {
            juce::Path p;
            for (int c = -range; c <= range; ++c)
            {
                const float x = ca.getX() + (float) (c + range) / (2 * range) * ca.getWidth();
                const float y = ca.getBottom() - curve[mid + c] / maxV * ca.getHeight() * 0.9f;
                if (c == -range) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (col);
            g.strokePath (p, juce::PathStrokeType (1.4f));
        };
        (void) n;
        g.setColour (theme::grid);
        for (int c = -300; c <= 300; c += 100)
            g.drawVerticalLine ((int) (ca.getX() + (float) (c + range) / (2 * range) * ca.getWidth()), ca.getY(), ca.getBottom());
        if (result.lowValid)
            plotCurve (result.lowCurve, theme::reference.withAlpha (0.7f));
        plotCurve (result.curve, theme::accent);
        const float bx = ca.getX() + (juce::jlimit ((float) -range, (float) range, result.bestCents) + range) / (2 * range) * ca.getWidth();
        g.setColour (theme::shift);
        g.drawVerticalLine ((int) bx, ca.getY(), ca.getBottom());
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f));
        g.drawText ("-300", ca.withHeight (12).translated (2, ca.getHeight() - 12), juce::Justification::centredLeft);
        g.drawText ("+300 ct", ca.withHeight (12).translated (-2, ca.getHeight() - 12), juce::Justification::centredRight);
        g.drawText ("friction vs offset", ca.withHeight (12).translated (4, 2), juce::Justification::centredLeft);
    }

    // kick vs bass report
    if (hasResult && (result.kickDetected || result.numNotes > 0))
    {
        auto kr = juce::Rectangle<float> (ca.getX(), ca.getBottom() + 4, ca.getWidth(), 48);
        g.setFont (theme::font (11.5f));
        g.setColour (theme::text);
        juce::String l1;
        if (result.kickDetected)
        {
            char nm[8];
            fr::midiNoteName ((int) std::lround (fr::hzToMidi (std::max (1.0f, result.kickNewF0))), nm);
            l1 = "Kick " + juce::String (result.kickF0, 1) + " Hz -> " + juce::String (result.kickNewF0, 1) + " Hz (" + juce::String (nm) + ", "
                 + juce::String (result.lowBestCents >= 0 ? "+" : "") + juce::String (juce::roundToInt (result.lowBestCents)) + " ct, -"
                 + juce::String (juce::roundToInt (result.lowReductionPct)) + " % low friction)";
        }
        else
            l1 = "Low band best " + juce::String (juce::roundToInt (result.lowBestCents)) + " ct";
        g.drawFittedText (l1, kr.removeFromTop (16).toNearestInt(), juce::Justification::centredLeft, 1);
        juce::String l2 = "Clashed with: ";
        for (int i = 0; i < result.numNotes; ++i)
        {
            char nm[8];
            fr::midiNoteName (result.notes[i].midi, nm);
            l2 += juce::String (nm) + " " + juce::String (juce::roundToInt (result.notes[i].share * 100.0f)) + "%  ";
        }
        g.setColour (theme::textDim);
        g.drawFittedText (l2, kr.removeFromTop (30).toNearestInt(), juce::Justification::topLeft, 2);
    }
}

} // namespace friction
