#include "FrictionMap.h"

namespace friction
{

using fr::DisplayGrid;

FrictionMap::FrictionMap()
{
    for (int i = 0; i < N; ++i)
        tSm[i] = rSm[i] = -200.0f;
    setOpaque (true);
}

void FrictionMap::setFrame (const fr::DisplayFrame& f)
{
    frame = f;
    for (int i = 0; i < N; ++i)
    {
        // fast attack, ~36 dB/s release for spectra; friction and gain curves with mild smoothing
        tSm[i] = f.targetDb[i] > tSm[i] ? f.targetDb[i] : tSm[i] - 1.2f;
        rSm[i] = f.refDb[i] > rSm[i] ? f.refDb[i] : rSm[i] - 1.2f;
        fSm[i] += (f.friction[i] > fSm[i] ? 0.6f : 0.15f) * (f.friction[i] - fSm[i]);
        sSm[i] += (f.selfFriction[i] > sSm[i] ? 0.6f : 0.15f) * (f.selfFriction[i] - sSm[i]);
        gSm[i] += 0.5f * (f.gainDb[i] - gSm[i]);
    }
    hasFrame = true;
    repaint();
}

void FrictionMap::setOverlays (float rangeLowHz, float rangeHighHz, float crossoverHz, bool self)
{
    rangeLo = rangeLowHz;
    rangeHi = rangeHighHz;
    xover = crossoverHz;
    showSelf = self;
}

juce::Rectangle<float> FrictionMap::plot() const noexcept
{
    return getLocalBounds().toFloat().reduced (8.0f).withTrimmedBottom (16.0f).withTrimmedRight (28.0f);
}

float FrictionMap::xOf (float hz) const noexcept
{
    const auto p = plot();
    const float t = std::log (juce::jlimit (DisplayGrid::kMinHz, DisplayGrid::kMaxHz, hz) / DisplayGrid::kMinHz)
                    / std::log (DisplayGrid::kMaxHz / DisplayGrid::kMinHz);
    return p.getX() + t * p.getWidth();
}

float FrictionMap::hzAtX (float x) const noexcept
{
    const auto p = plot();
    const float t = juce::jlimit (0.0f, 1.0f, (x - p.getX()) / p.getWidth());
    return DisplayGrid::kMinHz * std::pow (DisplayGrid::kMaxHz / DisplayGrid::kMinHz, t);
}

float FrictionMap::yOf (float db) const noexcept
{
    const auto p = plot();
    const float t = (kDbTop - juce::jlimit (kDbBottom, kDbTop, db)) / (kDbTop - kDbBottom);
    return p.getY() + t * p.getHeight();
}

void FrictionMap::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    const auto p = plot();

    // grid
    g.setFont (theme::font (10.5f));
    const float freqs[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (float f : freqs)
    {
        const float x = xOf (f);
        g.setColour (theme::grid);
        g.drawVerticalLine ((int) x, p.getY(), p.getBottom());
        g.setColour (theme::textDim);
        const juce::String lbl = f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f);
        g.drawText (lbl, juce::Rectangle<float> (x - 20, p.getBottom() + 2, 40, 13), juce::Justification::centred);
    }
    for (float db = -96.0f; db <= 0.0f; db += 24.0f)
    {
        const float y = yOf (db);
        g.setColour (theme::grid);
        g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
        g.setColour (theme::textDim);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (p.getRight() + 2, y - 7, 26, 14), juce::Justification::centredLeft);
    }

    // out-of-range shading and crossover
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    if (rangeLo > DisplayGrid::kMinHz)
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (p.getX(), p.getY(), xOf (rangeLo), p.getBottom()));
    if (rangeHi < DisplayGrid::kMaxHz)
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOf (rangeHi), p.getY(), p.getRight(), p.getBottom()));
    {
        const float x = xOf (xover);
        const float dashes[] = { 4.0f, 4.0f };
        g.setColour (theme::textDim.withAlpha (0.6f));
        g.drawDashedLine (juce::Line<float> (x, p.getY(), x, p.getBottom()), dashes, 2, 1.0f);
        g.drawText ("x-over", juce::Rectangle<float> (x + 3, p.getY() + 2, 60, 12), juce::Justification::centredLeft);
    }

    if (! hasFrame)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
        g.drawText ("Waiting for audio...", p, juce::Justification::centred);
        return;
    }

    const float binW = p.getWidth() / (float) (N - 1);

    // friction heat (bottom-up bars)
    const float* fr = showSelf ? sSm : fSm;
    for (int i = 0; i < N; ++i)
    {
        const float f = fr[i];
        if (f < 0.01f)
            continue;
        const float x = p.getX() + (float) i * binW;
        const float h = juce::jlimit (0.0f, 1.0f, std::sqrt (f / 1.5f)) * p.getHeight() * 0.55f;
        juce::ColourGradient grad (theme::heat (f, 0.85f), x, p.getBottom(), theme::heat (f, 0.0f), x, p.getBottom() - h, false);
        g.setGradientFill (grad);
        g.fillRect (juce::Rectangle<float> (x - binW * 0.5f, p.getBottom() - h, binW + 1.0f, h));
    }

    auto spectrumPath = [&] (const float* v, bool close)
    {
        juce::Path path;
        bool started = false;
        for (int i = 0; i < N; ++i)
        {
            const float x = p.getX() + (float) i * binW;
            const float y = yOf (v[i]);
            if (! started) { path.startNewSubPath (x, y); started = true; }
            else path.lineTo (x, y);
        }
        if (close)
        {
            path.lineTo (p.getRight(), p.getBottom());
            path.lineTo (p.getX(), p.getBottom());
            path.closeSubPath();
        }
        return path;
    };

    if (frame.sidechain != 0)
    {
        g.setColour (theme::reference.withAlpha (0.12f));
        g.fillPath (spectrumPath (rSm, true));
        g.setColour (theme::reference.withAlpha (0.9f));
        g.strokePath (spectrumPath (rSm, false), juce::PathStrokeType (1.3f));
    }
    g.setColour (theme::target.withAlpha (0.06f));
    g.fillPath (spectrumPath (tSm, true));
    g.setColour (theme::target.withAlpha (0.85f));
    g.strokePath (spectrumPath (tSm, false), juce::PathStrokeType (1.4f));

    // gain-change curve (cuts), drawn from the top edge: 0 dB at the top, -30 dB at 40 % height
    {
        bool any = false;
        for (int i = 0; i < N; ++i) any = any || gSm[i] < -0.1f;
        if (any)
        {
            juce::Path gp;
            gp.startNewSubPath (p.getX(), p.getY());
            for (int i = 0; i < N; ++i)
                gp.lineTo (p.getX() + (float) i * binW, p.getY() + (-gSm[i] / 30.0f) * p.getHeight() * 0.4f);
            gp.lineTo (p.getRight(), p.getY());
            g.setColour (theme::gain.withAlpha (0.18f));
            g.fillPath (gp);
            g.setColour (theme::gain);
            g.strokePath (gp, juce::PathStrokeType (1.5f));
        }
    }

    // reference partial ticks (only the stronger ones, to keep the map readable)
    float refMax = 0.0f;
    for (int i = 0; i < frame.numRef; ++i)
        refMax = juce::jmax (refMax, frame.ref[i].amp);
    g.setColour (theme::reference);
    for (int i = 0; i < frame.numRef; ++i)
    {
        const auto& m = frame.ref[i];
        if (m.amp < refMax * 0.03f)
            continue;
        const float x = xOf (m.freq), y = yOf (fr::gainToDb (m.amp, -120.0f));
        juce::Path d;
        d.addTriangle (x - 3.0f, y - 6.5f, x + 3.0f, y - 6.5f, x, y - 2.0f);
        g.fillPath (d);
    }

    // target partial markers coloured by friction, cents-shift arrows
    for (int i = 0; i < frame.numTarget; ++i)
    {
        const auto& m = frame.target[i];
        const float fval = showSelf ? m.self : m.friction;
        const float x = xOf (m.freq), y = yOf (fr::gainToDb (m.amp, -120.0f));
        const float rad = 2.5f + 3.5f * juce::jlimit (0.0f, 1.0f, fval);
        g.setColour (fval > 0.02f ? theme::heat (fval) : theme::target.withAlpha (0.6f));
        g.fillEllipse (x - rad, y - rad, rad * 2.0f, rad * 2.0f);
        if (std::abs (m.shiftCents) > 0.5f)
        {
            const float x2 = xOf (m.freq * std::exp2 (m.shiftCents / 1200.0f));
            const float ya = y - 12.0f;
            g.setColour (theme::shift);
            g.drawArrow (juce::Line<float> (x, ya, x2 + (x2 > x ? 0.01f : -0.01f), ya), 1.5f, 6.0f, 6.0f);
            g.setFont (theme::font (10.0f));
            g.drawText ((m.shiftCents > 0 ? "+" : "") + juce::String (m.shiftCents, 1) + "c",
                        juce::Rectangle<float> (juce::jmin (x, x2) - 10, ya - 14, 50, 12), juce::Justification::centredLeft);
        }
    }

    // worst clash connector
    const fr::Clash& wc = showSelf ? frame.worstSelf : frame.worst;
    if (wc.valid && wc.roughness > 0.02f)
    {
        float ty = p.getCentreY(), ry = p.getCentreY();
        for (int i = 0; i < frame.numTarget; ++i)
            if (std::abs (frame.target[i].freq - wc.targetHz) < 0.01f)
                ty = yOf (fr::gainToDb (frame.target[i].amp, -120.0f));
        for (int i = 0; i < frame.numRef; ++i)
            if (std::abs (frame.ref[i].freq - wc.refHz) < 0.01f)
                ry = yOf (fr::gainToDb (frame.ref[i].amp, -120.0f));
        if (showSelf)
            ry = ty;
        const float x1 = xOf (wc.targetHz), x2 = xOf (wc.refHz);
        g.setColour (theme::heat (wc.roughness).withAlpha (0.9f));
        g.drawLine (x1, ty, x2, ry - 4.0f, 1.2f);
        g.drawEllipse (x1 - 9.0f, ty - 9.0f, 18.0f, 18.0f, 1.5f);
    }

    // whole-signal shift banner
    if (frame.mode == 1 && frame.retuneStyle != 0)
    {
        g.setColour (theme::shift);
        g.setFont (theme::font (13.0f, true));
        g.drawText ("Whole-signal shift " + juce::String (frame.wholeShiftCents >= 0 ? "+" : "") + juce::String (frame.wholeShiftCents, 1) + " ct",
                    p.withHeight (20.0f).translated (0.0f, 4.0f), juce::Justification::centredRight);
    }

    // legend
    {
        auto lg = juce::Rectangle<float> (p.getX() + 6, p.getY() + 6, 110, 14);
        auto item = [&] (juce::Colour c, const juce::String& s)
        {
            g.setColour (c);
            g.fillRoundedRectangle (lg.withWidth (10).reduced (0, 3), 2.0f);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            g.drawText (s, lg.withTrimmedLeft (14), juce::Justification::centredLeft);
            lg = lg.translated (0, 15);
        };
        item (theme::target, "Target");
        if (frame.sidechain != 0)
            item (theme::reference, "Reference");
        item (theme::heatHigh, showSelf ? "Self friction" : "Friction");
        if (frame.mode == 0)
            item (theme::gain, "Cut");
        else if (frame.mode == 1)
            item (theme::shift, "Shift");
    }

    // hover readout
    if (mouse.x >= p.getX() && mouse.x <= p.getRight() && mouse.y >= p.getY() && mouse.y <= p.getBottom())
    {
        const float hz = hzAtX (mouse.x);
        int best = -1;
        float bestD = 1e9f;
        for (int i = 0; i < frame.numTarget; ++i)
        {
            const float d = std::abs (xOf (frame.target[i].freq) - mouse.x);
            if (d < bestD && d < 10.0f) { bestD = d; best = i; }
        }
        juce::String s = theme::hzString (hz);
        if (best >= 0)
        {
            const auto& m = frame.target[best];
            s = "Partial " + theme::hzString (m.freq) + "  " + juce::String (fr::gainToDb (m.amp, -120.0f), 1) + " dB  friction "
                + juce::String (showSelf ? m.self : m.friction, 2);
            if (m.gainDb < -0.1f) s += "  cut " + juce::String (m.gainDb, 1) + " dB";
            if (std::abs (m.shiftCents) > 0.1f) s += "  shift " + juce::String (m.shiftCents, 1) + " ct";
        }
        g.setColour (theme::textDim.withAlpha (0.5f));
        g.drawVerticalLine ((int) mouse.x, p.getY(), p.getBottom());
        g.setColour (theme::panelRaised.withAlpha (0.9f));
        auto box = juce::Rectangle<float> (0, 0, 330, 18).withCentre ({ juce::jlimit (p.getX() + 165, p.getRight() - 165, mouse.x), p.getBottom() - 14 });
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (11.5f));
        g.drawText (s, box, juce::Justification::centred);
    }

    g.setColour (theme::outline);
    g.drawRect (getLocalBounds());
}

void FrictionMap::mouseMove (const juce::MouseEvent& e)
{
    mouse = e.position;
    repaint();
}

void FrictionMap::mouseExit (const juce::MouseEvent&)
{
    mouse = { -1.0f, -1.0f };
    repaint();
}

void FrictionMap::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem (1, "Show friction against reference", true, ! showSelf);
        m.addItem (2, "Show target-internal (Self) friction", true, showSelf);
        m.showMenuAsync (juce::PopupMenu::Options(), [this] (int r)
        {
            if (r == 1 || r == 2)
            {
                showSelf = r == 2;
                if (onSelfToggled) onSelfToggled (showSelf);
                repaint();
            }
        });
    }
}

// ============================================================================ HistoryStrip

HistoryStrip::HistoryStrip()
{
    setOpaque (true);
    img.clear (img.getBounds(), theme::panel);
}

void HistoryStrip::resized() {}

void HistoryStrip::push (const fr::DisplayFrame& f, bool useSelf)
{
    const float* curve = useSelf ? f.selfFriction : f.friction;
    for (int r = 0; r < kRows; ++r)
    {
        // row 0 = top = 20 kHz
        const int gi = juce::jlimit (0, fr::DisplayFrame::kN - 1, (int) std::lround ((1.0 - (double) r / (kRows - 1)) * (fr::DisplayFrame::kN - 1)));
        const float v = curve[gi];
        const juce::Colour c = v < 0.01f ? theme::panel : theme::panel.interpolatedWith (theme::heat (v), juce::jlimit (0.0f, 1.0f, std::sqrt (v / 1.2f)));
        img.setPixelAt (writeCol, r, c);
    }
    totals[writeCol] = useSelf ? f.totalSelf : f.totalFriction;
    threshold = f.threshold;
    writeCol = (writeCol + 1) % kCols;
    repaint();
}

void HistoryStrip::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (8.0f, 6.0f).withTrimmedRight (28.0f);
    // draw oldest..newest left to right
    const float wOld = (float) (kCols - writeCol) / kCols * r.getWidth();
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.drawImage (img, r.getX(), r.getY(), wOld, r.getHeight(), writeCol, 0, kCols - writeCol, kRows);
    if (writeCol > 0)
        g.drawImage (img, r.getX() + wOld, r.getY(), r.getWidth() - wOld, r.getHeight(), 0, 0, writeCol, kRows);

    // total friction line (display scale: 100 * tanh(R / 2))
    juce::Path line;
    for (int i = 0; i < kCols; ++i)
    {
        const int idx = (writeCol + i) % kCols;
        const float v = 100.0f * std::tanh (totals[idx] * 0.5f);
        const float x = r.getX() + (float) i / (kCols - 1) * r.getWidth();
        const float y = r.getBottom() - v / 100.0f * r.getHeight();
        if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
    }
    g.setColour (theme::text.withAlpha (0.75f));
    g.strokePath (line, juce::PathStrokeType (1.2f));

    g.setColour (theme::textDim);
    g.setFont (theme::font (10.5f));
    g.drawText ("now", juce::Rectangle<float> (r.getRight() + 2, r.getBottom() - 12, 28, 12), juce::Justification::centredLeft);
    g.drawText ("20k", juce::Rectangle<float> (r.getRight() + 2, r.getY(), 28, 12), juce::Justification::centredLeft);
    g.drawText ("20", juce::Rectangle<float> (r.getRight() + 2, r.getBottom() - 26, 28, 12), juce::Justification::centredLeft);
    g.drawText ("Friction over time (" + juce::String (kCols / 30) + " s)", r.withHeight (14).translated (4, 2), juce::Justification::centredLeft);
    g.setColour (theme::outline);
    g.drawRect (getLocalBounds());
}

} // namespace friction
