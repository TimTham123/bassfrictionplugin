// Friction - Tension mode tests
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
}

void ft::testsTension (Context& ctx)
{
    const double fs = 48000.0;

    // ---- synth voices: beat rate follows the Beat Rate control (silent target: output = synthesized partials)
    {
        const double secs = 5.0;
        const size_t a = (size_t) (2.0 * fs), b = (size_t) (4.5 * fs);
        Buf ref = sine (fs, secs, 440.0, 0.3);
        std::string meas;
        bool pass = true;
        for (double br : { 0.0, 0.5, 1.0 })
        {
            fr::Params p;
            p.mode = fr::Mode::Tension;
            p.tensionStyle = fr::TensionStyle::Synth;
            p.amount = 1.0f;
            p.beatRate = (float) br;
            auto eng = makeEngine (fs, 512, 1, p);
            const int lat = eng->currentLatency();
            Buf silent (ref.size(), 0.0f);
            Chans out = runEngine (*eng, Chans { silent }, Chans { ref }, RunOptions());
            const Buf o = alignOut (out[0], lat);
            const double fOut = bandFrequency (o, fs, 380.0, 500.0, a, b);
            const double beat = std::abs (fOut - 440.0);
            const double expect = 0.04 * std::pow (25.0, br) * fr::Roughness::peakDeltaHz (440.0f);
            const double level = db (rms (o, a, b) / rms (ref, a, b));
            if (std::abs (beat - expect) > 0.05 * expect + 0.05 || std::abs (level) > 1.0) pass = false;
            meas += fmt ("BeatRate %.0f%%: beat %.2f Hz (expected %.2f), level %.2f dB re ref; ", br * 100.0, beat, expect, level);
            if (br == 0.5)
            {
                ctx.wav ("tension_synth_ref440_mix.wav", { mixOf (o, ref) }, fs);
            }
        }
        ctx.add ("Tension", "Synth voices: beat rate follows Beat Rate (slow wobble -> grit)", pass, meas,
                 "beat = 0.04*25^b * peak distance (+-5 %), Amount 100 % = equal level");
    }

    // ---- follows the reference harmony as it changes
    {
        const double secs = 6.0;
        Buf ref ((size_t) (secs * fs), 0.0f);
        double ph = 0.0;
        for (size_t i = 0; i < ref.size(); ++i)
        {
            const double t = (double) i / fs;
            const double f = t < 3.0 ? 220.0 : 293.66; // A3 -> D4
            ph += 2.0 * kPi * f / fs;
            ref[i] = (float) (0.3 * std::sin (ph));
        }
        fr::Params p;
        p.mode = fr::Mode::Tension;
        p.amount = 0.8f;
        p.beatRate = 0.3f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Buf silent (ref.size(), 0.0f);
        Chans out = runEngine (*eng, Chans { silent }, Chans { ref }, RunOptions());
        const Buf o = alignOut (out[0], lat);
        const double f1 = bandFrequency (o, fs, 180, 260, (size_t) (1.5 * fs), (size_t) (2.8 * fs));
        const double f2 = bandFrequency (o, fs, 250, 340, (size_t) (4.5 * fs), (size_t) (5.8 * fs));
        const double b1 = 0.04 * std::pow (25.0, 0.3) * fr::Roughness::peakDeltaHz (220.0f);
        const double b2 = 0.04 * std::pow (25.0, 0.3) * fr::Roughness::peakDeltaHz (293.66f);
        const bool ok = std::abs (std::abs (f1 - 220.0) - b1) < 0.2 && std::abs (std::abs (f2 - 293.66) - b2) < 0.2;
        ctx.add ("Tension", "Synth voices follow the reference harmony (A3 -> D4)", ok,
                 fmt ("segment 1: voice %.2f Hz (ref 220, beat %.2f, expected %.2f); segment 2: voice %.2f Hz (ref 293.66, beat %.2f, expected %.2f)",
                      f1, std::abs (f1 - 220.0), b1, f2, std::abs (f2 - 293.66), b2),
                 "beat within 0.2 Hz of target in both segments");
    }

    // ---- push: unison target partials pushed toward the max-roughness spacing
    {
        const double secs = 6.0;
        const size_t a = (size_t) (2.5 * fs), b = (size_t) (5.5 * fs);
        Buf t = saw (fs, secs, 220.0, 0.2);
        Buf r = saw (fs, secs, 220.0, 0.2, 400, 0.8);
        fr::Params p;
        p.mode = fr::Mode::Tension;
        p.tensionStyle = fr::TensionStyle::Push;
        p.amount = 1.0f;
        p.beatRate = 0.5f;
        p.maxCents = 50.0f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, Chans { t }, Chans { r }, RunOptions());
        const Buf o = alignOut (out[0], lat);
        std::string per;
        bool ok = true;
        for (int h = 1; h <= 4; ++h)
        {
            const double fr0 = 220.0 * h;
            const double fOut = bandFrequency (o, fs, fr0 - 40, fr0 + 40, a, b);
            const double expectHz = std::min (0.04 * std::pow (25.0, 0.5) * fr::Roughness::peakDeltaHz ((float) fr0),
                                              fr0 * (std::pow (2.0, 50.0 / 1200.0) - 1.0));
            if (std::abs (std::abs (fOut - fr0) - expectHz) > 0.15 * expectHz + 0.1) ok = false;
            per += fmt ("h%d %+.2f Hz (exp +-%.2f); ", h, fOut - fr0, expectHz);
        }
        ctx.add ("Tension", "Push: unison partials moved to the rough spacing", ok, per, "within 15 % of 0.2 x peak distance (capped by Max Cents)");
        ctx.wav ("tension_push_saws_after_mix.wav", { mixOf (o, r) }, fs);
    }
}
