// Friction - musical demo renders (before/after), with measured friction reduction
#include "Harness.h"

using namespace ft;

namespace
{

Buf alignOut (const Buf& out, int lat)
{
    Buf r (out.size(), 0.0f);
    for (size_t i = 0; i + (size_t) lat < out.size(); ++i)
        r[i] = out[i + (size_t) lat];
    return r;
}

/** Piano-like note: slightly stretched partials, per-partial decay, soft hammer noise. */
void addPianoNote (Buf& dst, double fs, size_t at, double f0, double amp, double dur)
{
    const int H = 14;
    const double B = 0.00025;
    std::mt19937 rng ((unsigned) (f0 * 100));
    std::uniform_real_distribution<double> u (-1.0, 1.0);
    const size_t n = (size_t) (dur * fs);
    for (int k = 1; k <= H; ++k)
    {
        const double fk = f0 * k * std::sqrt (1.0 + B * k * k);
        if (fk > 0.45 * fs) break;
        const double ak = amp * std::pow (0.72, k - 1) * (k == 1 ? 0.8 : 1.0);
        const double tau = 1.6 / (1.0 + 0.35 * k);
        const double ph = u (rng) * kPi;
        for (size_t i = 0; i < n && at + i < dst.size(); ++i)
        {
            const double t = (double) i / fs;
            const double tEnd = (double) (n - i) / fs;
            const double fadeOut = tEnd < 0.05 ? 0.5 - 0.5 * std::cos (kPi * tEnd / 0.05) : 1.0; // no step at the end
            const double env = std::exp (-t / tau) * std::min (1.0, t / 0.003) * fadeOut;
            dst[at + i] += (float) (ak * env * std::sin (2.0 * kPi * fk * t + ph));
        }
    }
    for (size_t i = 0; i < (size_t) (0.01 * fs) && at + i < dst.size(); ++i)
        dst[at + i] += (float) (0.05 * amp * u (rng) * std::exp (-(double) i / (0.002 * fs)));
}

/** Worst 5 ms window where the output has more energy above 6 kHz than the input had in any 5 ms window
    starting within +-2 ms of it (dB). Clicks, splice artefacts and STFT pre-echo (which spreads up to a
    window length before an onset) show up here; sub-millisecond timing differences of the whole-signal
    shifter (onset alignment tolerance 0.3 ms, interpolator pre-ringing 0.33 ms) do not. */
double addedHfDb (const Buf& in, const Buf& out, double fs)
{
    const Buf hi = highPass4 (in, fs, 6000.0), ho = highPass4 (out, fs, 6000.0);
    const double floorRms = std::max (1e-6, rms (in) * 1e-3); // ignore windows 60 dB below the signal
    const size_t w = (size_t) (0.005 * fs), step = (size_t) (0.0005 * fs);
    const int tol = 4; // steps of 0.5 ms
    std::vector<double> inR, outR;
    for (size_t i = 0; i + w <= in.size(); i += step)
    {
        inR.push_back (rms (hi, i, i + w));
        outR.push_back (rms (ho, i, i + w));
    }
    double worst = -300.0;
    for (int k = 0; k < (int) inR.size(); ++k)
    {
        const size_t i = (size_t) k * step;
        if (i < (size_t) fs || i + w >= in.size() - (size_t) (0.2 * fs))
            continue;
        double a = floorRms;
        for (int j = std::max (0, k - tol); j <= std::min ((int) inR.size() - 1, k + tol); ++j)
            a = std::max (a, inR[(size_t) j]);
        worst = std::max (worst, db (outR[(size_t) k] / a));
    }
    return worst;
}

/** Total friction of a rendered target against the reference, averaged over the run (analysis only). */
double meanFriction (double fs, const Buf& target, const Buf& ref)
{
    fr::Params p;
    p.amount = 0.0f;
    auto e = makeEngine (fs, 512, 1, p);
    double acc = 0.0;
    int n = 0, last = -1;
    RunOptions o;
    o.afterBlock = [&] (std::int64_t pos, fr::FrictionEngine& eng)
    {
        const auto& pr = eng.probe();
        if (pr.frames != last && (double) pos > fs)
        {
            last = pr.frames;
            acc += pr.totalFriction;
            ++n;
        }
    };
    runEngine (*e, Chans { target }, Chans { ref }, o);
    return n > 0 ? acc / n : 0.0;
}

} // namespace

void ft::testsDemos (Context& ctx)
{
    const double fs = 48000.0;

    // ------------------------------------------------ demo 1: detuned piano-like part vs an in-tune pad
    {
        const double secs = 10.0;
        Buf piano ((size_t) (secs * fs), 0.0f), pad ((size_t) (secs * fs), 0.0f);
        const double detune = std::pow (2.0, 18.0 / 1200.0); // piano 18 cents sharp
        struct Chord { double t; std::vector<double> notes; };
        const std::vector<Chord> prog = { { 0.0, { 220.0, 277.18, 329.63 } }, { 2.5, { 196.0, 246.94, 293.66 } },
                                          { 5.0, { 174.61, 220.0, 261.63 } }, { 7.5, { 196.0, 246.94, 329.63 } } };
        for (const auto& c : prog)
        {
            for (double f : c.notes)
            {
                addPianoNote (piano, fs, (size_t) (c.t * fs), f * detune, 0.11, 2.6);
                addPianoNote (piano, fs, (size_t) ((c.t + 1.25) * fs), f * 2.0 * detune, 0.05, 1.3);
            }
            // pad: soft saw chord, slow attack
            for (double f : c.notes)
            {
                Buf s = saw (fs, 2.5, f, 0.045, 40, f * 0.003);
                for (size_t i = 0; i < s.size(); ++i)
                {
                    const double t = (double) i / fs;
                    s[i] *= (float) (std::min (1.0, t / 0.25) * std::min (1.0, (2.5 - t) / 0.1));
                }
                addInto (pad, s, 1.0, (size_t) (c.t * fs));
            }
        }
        const double fBefore = meanFriction (fs, piano, pad);
        ctx.wav ("demo1_piano_vs_pad_before.wav", { mixOf (piano, pad) }, fs, 0.0, 10.0);
        std::string meas = fmt ("mean friction before %.2f; ", fBefore);
        struct V { const char* name; const char* file; fr::Mode m; fr::RetuneStyle rs; };
        const V variants[] = { { "Smooth", "demo1_piano_vs_pad_after_smooth.wav", fr::Mode::Smooth, fr::RetuneStyle::PerPartial },
                               { "Retune per-partial", "demo1_piano_vs_pad_after_retune_partial.wav", fr::Mode::Retune, fr::RetuneStyle::PerPartial },
                               { "Retune whole auto", "demo1_piano_vs_pad_after_retune_whole.wav", fr::Mode::Retune, fr::RetuneStyle::WholeAuto } };
        bool ok = true;
        for (const auto& v : variants)
        {
            fr::Params p;
            p.mode = v.m;
            p.retuneStyle = v.rs;
            p.amount = 1.0f;
            p.sensitivity = 0.7f;
            p.attackMs = 15.0f;
            p.releaseMs = 200.0f;
            auto e = makeEngine (fs, 512, 1, p);
            Chans o = runEngine (*e, Chans { piano }, Chans { pad }, RunOptions());
            const Buf al = alignOut (o[0], e->currentLatency());
            const double fAfter = meanFriction (fs, al, pad);
            const double hf = addedHfDb (piano, al, fs);
            ctx.wav (v.file, { mixOf (al, pad) }, fs, 0.0, 10.0);
            meas += fmt ("%s: %.2f (%.0f %%), added HF worst %+.1f dB; ", v.name, fAfter, 100.0 * (fAfter - fBefore) / fBefore, hf);
            if (! (fAfter < fBefore) || hf > 3.0) ok = false;
        }
        ctx.add ("Demos", "Demo 1: piano-like part (+18 c) vs pad", ok, meas,
                 "friction reduced by every mode, no added HF bursts > +3 dB (5 ms windows, +-2 ms timing tolerance; renders/demo1_*.wav)");
    }

    // ------------------------------------------------ demo 2: kick vs bass, Smooth and Advisor-guided kick retune
    {
        const double secs = 10.0;
        KickParams kp;
        kp.fEnd = 52.0;
        Buf kick ((size_t) (secs * fs), 0.0f), bass ((size_t) (secs * fs), 0.0f);
        for (double h = 0.05; h < secs - 0.6; h += 0.5)
            addKick (kick, fs, (size_t) (h * fs), kp);
        const double pattern[4] = { 55.0, 55.0, 65.406, 48.999 };
        double ph1 = 0.0, ph2 = 0.0;
        for (size_t n = 0; n < bass.size(); ++n)
        {
            const double t = (double) n / fs;
            const double f = pattern[(int) t % 4];
            const double env = std::min (1.0, std::min ((t - std::floor (t)) / 0.005, (std::floor (t) + 1.0 - t) / 0.005));
            ph1 += 2.0 * kPi * f / fs;
            ph2 += 2.0 * kPi * 2.0 * f / fs;
            bass[n] = (float) (env * (0.35 * std::sin (ph1) + 0.12 * std::sin (ph2)));
        }
        const double fBefore = meanFriction (fs, kick, bass);
        ctx.wav ("demo2_kick_vs_bass_before.wav", { mixOf (kick, bass) }, fs, 0.0, 8.0);

        // Smooth, Focus Low
        fr::Params p;
        p.amount = 1.0f;
        p.sensitivity = 0.75f;
        p.focus = fr::Focus::Low;
        p.attackMs = 5.0f;
        p.releaseMs = 120.0f;
        auto e = makeEngine (fs, 512, 1, p);
        Chans o = runEngine (*e, Chans { kick }, Chans { bass }, RunOptions());
        const Buf sm = alignOut (o[0], e->currentLatency());
        const double fSmooth = meanFriction (fs, sm, bass);
        ctx.wav ("demo2_kick_vs_bass_after_smooth.wav", { mixOf (sm, bass) }, fs, 0.0, 8.0);

        // Advisor: learn, then apply the low-band offset as Whole Fixed retune of the kick
        fr::Params pl;
        pl.learn = true;
        pl.amount = 0.0f;
        auto el = makeEngine (fs, 512, 1, pl);
        fr::Advisor adv;
        RunOptions lo;
        lo.afterBlock = [&] (std::int64_t, fr::FrictionEngine& eng)
        {
            fr::AdvisorFrame f;
            while (eng.advisorFifo().pop (f)) adv.consume (f);
        };
        runEngine (*el, Chans { kick }, Chans { bass }, lo);
        const auto res = adv.finish();
        fr::Params pr;
        pr.mode = fr::Mode::Retune;
        pr.retuneStyle = fr::RetuneStyle::WholeFixed;
        pr.wholeOffsetCents = res.lowBestCents;
        pr.amount = 1.0f;
        pr.attackMs = 5.0f;
        auto er = makeEngine (fs, 512, 1, pr);
        Chans o2 = runEngine (*er, Chans { kick }, Chans { bass }, RunOptions());
        const Buf rt = alignOut (o2[0], er->currentLatency());
        const double fRetune = meanFriction (fs, rt, bass);
        ctx.wav ("demo2_kick_vs_bass_after_advisor_retune.wav", { mixOf (rt, bass) }, fs, 0.0, 8.0);
        const double hfS = addedHfDb (kick, sm, fs), hfR = addedHfDb (kick, rt, fs);
        ctx.add ("Demos", "Demo 2: kick (52 Hz) vs bass line", fSmooth < fBefore && fRetune < fBefore && hfS <= 3.0 && hfR <= 3.0,
                 fmt ("mean friction before %.3f; Smooth (Focus Low) %.3f (%.0f %%); Advisor offset %+.0f c applied as Whole Fixed: %.3f (%.0f %%)",
                      fBefore, fSmooth, 100.0 * (fSmooth - fBefore) / fBefore, res.lowBestCents, fRetune, 100.0 * (fRetune - fBefore) / fBefore)
                     + fmt ("; added HF worst: Smooth %+.1f dB, Retune %+.1f dB", hfS, hfR),
                 "friction reduced, no added HF bursts > +3 dB (5 ms windows, +-2 ms timing tolerance; renders/demo2_*.wav)");
    }
}
