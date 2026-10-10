// Friction - Smooth mode, null and latency tests
#include "Harness.h"

using namespace ft;

namespace
{

/** Out aligned to the input timeline (drops the first `lat` samples). */
Buf align (const Buf& out, int lat)
{
    Buf r (out.size(), 0.0f);
    for (size_t i = 0; i + (size_t) lat < out.size(); ++i)
        r[i] = out[i + (size_t) lat];
    return r;
}

/** Robust peak-to-peak modulation depth (5th/95th percentile) of a band envelope. */
double bandDepth (const Buf& sig, double fs, double fLo, double fHi, size_t a, size_t b)
{
    Buf env = bandEnvelope (sig, fs, fLo, fHi);
    b = std::min (b, env.size());
    std::vector<float> v (env.begin() + (long) a, env.begin() + (long) b);
    std::sort (v.begin(), v.end());
    const double lo = v[(size_t) (0.02 * (double) (v.size() - 1))];
    const double hi = v[(size_t) (0.98 * (double) (v.size() - 1))];
    return (hi + lo) > 0 ? (hi - lo) / (hi + lo) : 0.0;
}

struct BeatReport
{
    double meanDb = 0.0;      // mean change of per-harmonic modulation depth (dB, negative = less beating)
    double fundDb = 0.0;
    double broadbandDb = 0.0; // change of broadband RMS-envelope modulation
    std::string perHarm;
};

BeatReport beatingChange (const Buf& before, const Buf& after, double fs, double f0, int harmonics, size_t a, size_t b)
{
    BeatReport r;
    double acc = 0.0;
    int n = 0;
    for (int h = 1; h <= harmonics; ++h)
    {
        const double fc = f0 * h;
        const double d0 = bandDepth (before, fs, fc - 50.0, fc + 50.0, a, b);
        const double d1 = bandDepth (after, fs, fc - 50.0, fc + 50.0, a, b);
        const double change = db (std::max (d1, 1e-6) / std::max (d0, 1e-6));
        if (h == 1) r.fundDb = change;
        if (h <= 6) r.perHarm += fmt ("h%d %.1f; ", h, change);
        acc += change;
        ++n;
    }
    r.meanDb = acc / n;
    const int win = (int) (0.01 * fs);
    Buf e0 = rmsEnvelope (before, win), e1 = rmsEnvelope (after, win);
    const double m0 = modulationDepthPP (e0, a, b), m1 = modulationDepthPP (e1, a, b);
    r.broadbandDb = db (std::max (m1, 1e-6) / std::max (m0, 1e-6));
    return r;
}

void nullTests (Context& ctx)
{
    const double fs = 48000.0;
    // clashing material so that processing *would* happen
    Buf t1 = saw (fs, 3.0, 220.0 * std::pow (2.0, 15.0 / 1200.0), 0.3);
    Buf t2 = saw (fs, 3.0, 221.0 * std::pow (2.0, 15.0 / 1200.0), 0.3, 400, 0.5);
    Buf r = saw (fs, 3.0, 220.0, 0.3);
    Chans tgt { t1, t2 };

    struct Case { const char* name; fr::Mode mode; fr::LatencyMode lat; fr::RetuneStyle style; float amount, mix; };
    const Case cases[] = {
        { "Smooth Mix, Amount 0", fr::Mode::Smooth, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, 0.0f, 1.0f },
        { "Smooth Mix, Mix 0", fr::Mode::Smooth, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, 1.0f, 0.0f },
        { "Smooth Track, Amount 0", fr::Mode::Smooth, fr::LatencyMode::Track, fr::RetuneStyle::PerPartial, 0.0f, 1.0f },
        { "Smooth Track, Mix 0", fr::Mode::Smooth, fr::LatencyMode::Track, fr::RetuneStyle::PerPartial, 1.0f, 0.0f },
        { "Retune per-partial, Amount 0", fr::Mode::Retune, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, 0.0f, 1.0f },
        { "Retune whole auto, Amount 0", fr::Mode::Retune, fr::LatencyMode::Mix, fr::RetuneStyle::WholeAuto, 0.0f, 1.0f },
        { "Retune whole fixed +50 c, Amount 0", fr::Mode::Retune, fr::LatencyMode::Mix, fr::RetuneStyle::WholeFixed, 0.0f, 1.0f },
        { "Tension synth, Amount 0", fr::Mode::Tension, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, 0.0f, 1.0f },
        { "Tension synth, Mix 0", fr::Mode::Tension, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, 1.0f, 0.0f },
    };
    for (const auto& c : cases)
    {
        fr::Params p;
        p.mode = c.mode;
        p.latencyMode = c.lat;
        p.retuneStyle = c.style;
        p.wholeOffsetCents = 50.0f;
        p.amount = c.amount;
        p.mix = c.mix;
        p.sensitivity = 1.0f;
        auto eng = makeEngine (fs, 512, 2, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, tgt, Chans { r }, RunOptions());
        double worst = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            worst = std::max (worst, maxAbsDiff (align (out[(size_t) ch], lat), tgt[(size_t) ch], 0, tgt[0].size() - (size_t) lat));
        ctx.add ("Null tests", c.name, worst <= 1e-5, fmt ("max |out - dry(t-L)| = %s (L = %d)", worst == 0.0 ? "0 (bit-exact)" : fmt ("%.1f dBFS", db (worst)).c_str(), lat),
                 "<= -100 dBFS");
    }

    // silent sidechain: bus present, all zeros -> no processing in any mode
    for (fr::Mode m : { fr::Mode::Smooth, fr::Mode::Retune, fr::Mode::Tension })
    {
        fr::Params p;
        p.mode = m;
        p.retuneStyle = fr::RetuneStyle::WholeAuto;
        p.amount = 1.0f;
        p.sensitivity = 1.0f;
        auto eng = makeEngine (fs, 512, 2, p);
        const int lat = eng->currentLatency();
        Buf silent (t1.size(), 0.0f);
        Chans out = runEngine (*eng, tgt, Chans { silent, silent }, RunOptions());
        double worst = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            worst = std::max (worst, maxAbsDiff (align (out[(size_t) ch], lat), tgt[(size_t) ch], 0, tgt[0].size() - (size_t) lat));
        const char* nm = m == fr::Mode::Smooth ? "Silent sidechain: Smooth" : (m == fr::Mode::Retune ? "Silent sidechain: Retune (whole auto)" : "Silent sidechain: Tension");
        ctx.add ("Null tests", nm, worst <= 1e-5 && eng->probe().sidechain == fr::SidechainStatus::Silent,
                 fmt ("max diff %s, status %s", worst == 0.0 ? "0 (bit-exact)" : fmt ("%.1f dBFS", db (worst)).c_str(),
                      eng->probe().sidechain == fr::SidechainStatus::Silent ? "\"Sidechain silent\"" : "?"),
                 "<= -100 dBFS, status Silent");
    }
}

void latencyTests (Context& ctx)
{
    // 1) impulse through an idle path: output position must equal the reported latency exactly
    std::string meas;
    bool pass = true;
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        for (fr::LatencyMode lm : { fr::LatencyMode::Mix, fr::LatencyMode::Track })
        {
            fr::Params p;
            p.latencyMode = lm;
            p.amount = 1.0f;
            auto eng = makeEngine (fs, 256, 1, p);
            const int reported = eng->currentLatency();
            Buf x ((size_t) (fs * 1.0), 0.0f);
            const size_t click = (size_t) (0.6 * fs);
            x[click] = 0.9f;
            Buf r = sine (fs, 1.0, 440.0, 0.2);
            Chans out = runEngine (*eng, Chans { x }, Chans { r }, RunOptions());
            size_t best = 0;
            for (size_t i = 0; i < out[0].size(); ++i)
                if (std::abs (out[0][i]) > std::abs (out[0][best])) best = i;
            const long measured = (long) best - (long) click;
            if (measured != reported || std::abs (out[0][best] - 0.9f) > 1e-6f) pass = false;
            meas += fmt ("%.1fk %s %ld/%d; ", fs / 1000.0, lm == fr::LatencyMode::Mix ? "Mix" : "Track", measured, reported);
        }
    ctx.add ("Latency", "Impulse latency == reported (6 rates x 2 modes)", pass, meas, "exact, unit gain");

    // 2) alignment while processing is active (cross-correlation over broadband material)
    meas.clear();
    pass = true;
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (fr::LatencyMode lm : { fr::LatencyMode::Mix, fr::LatencyMode::Track })
        {
            fr::Params p;
            p.latencyMode = lm;
            p.amount = 1.0f;
            p.sensitivity = 1.0f;
            auto eng = makeEngine (fs, 256, 1, p);
            const int reported = eng->currentLatency();
            const double secs = 2.0;
            Buf x = saw (fs, secs, 330.0 * std::pow (2.0, 20.0 / 1200.0), 0.2);
            addInto (x, noise (fs, secs, 0.05, 9));
            Buf r = saw (fs, secs, 330.0, 0.2);
            Chans out = runEngine (*eng, Chans { x }, Chans { r }, RunOptions());
            const size_t a = (size_t) (1.0 * fs), b = (size_t) (1.2 * fs);
            const int lag = bestLag (x, out[0], reported + 8, a, b);
            if (std::abs (lag - reported) > 1 || eng->probe().maxDepthDb < 1.0f) pass = false;
            meas += fmt ("%.0fk %s lag %d (L %d, cut %.0f dB); ", fs / 1000.0, lm == fr::LatencyMode::Mix ? "Mix" : "Track", lag, reported, eng->probe().maxDepthDb);
        }
    ctx.add ("Latency", "Output aligned to reported latency while processing", pass, meas, "|lag - L| <= 1 sample (bells are minimum-phase)");
}

} // namespace

void ft::testsSmooth (Context& ctx)
{
    nullTests (ctx);
    latencyTests (ctx);

    const double fs = 48000.0;
    const double secs = ctx.quick ? 6.0 : 9.0;
    const size_t a = (size_t) (1.5 * fs), b = (size_t) ((secs - 0.5) * fs);

    // ---- two sawtooth tones a few cents apart
    const double f0 = 220.0, cents = 8.0;
    Buf tgt = saw (fs, secs, f0 * std::pow (2.0, cents / 1200.0), 0.25);
    Buf ref = saw (fs, secs, f0, 0.25, 400, 1.0);
    const Buf before = mixOf (tgt, ref);
    for (fr::LatencyMode lm : { fr::LatencyMode::Mix, fr::LatencyMode::Track })
    {
        fr::Params p;
        p.mode = fr::Mode::Smooth;
        p.latencyMode = lm;
        p.amount = 1.0f;
        p.sensitivity = 0.8f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, Chans { tgt }, Chans { ref }, RunOptions());
        const Buf after = mixOf (align (out[0], lat), ref);
        const BeatReport rep = beatingChange (before, after, fs, f0, 12, a, b);
        const char* nm = lm == fr::LatencyMode::Mix ? "Smooth (Mix) reduces saw-pair beating (8 cents)" : "Smooth (Track) reduces saw-pair beating (8 cents)";
        ctx.add ("Smooth", nm, rep.meanDb <= -6.0,
                 fmt ("modulation depth change: mean of 12 harmonics %.1f dB, fundamental %.1f dB, broadband %.1f dB [%s]",
                      rep.meanDb, rep.fundDb, rep.broadbandDb, rep.perHarm.c_str()),
                 "mean <= -6 dB (Amount 100 %, Sensitivity 80 %)");
        if (lm == fr::LatencyMode::Mix)
        {
            ctx.wav ("smooth_saws_before_mix.wav", { before }, fs, 3.0);
            ctx.wav ("smooth_saws_after_mix.wav", { after }, fs, 3.0);
            ctx.wav ("smooth_saws_target_before.wav", { tgt }, fs, 3.0);
            ctx.wav ("smooth_saws_target_after.wav", { align (out[0], lat) }, fs, 3.0);
        }
    }

    // ---- low band: bass at 55 Hz vs reference at 57.2 Hz (2.2 Hz beat), handled by the YIN path + bells
    {
        Buf bass = harmonic (fs, secs, 55.0, { 0.4, 0.15, 0.06 });
        Buf kickBody = harmonic (fs, secs, 57.2, { 0.4, 0.1 });
        fr::Params p;
        p.amount = 1.0f;
        p.sensitivity = 0.8f;
        p.focus = fr::Focus::Low;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, Chans { bass }, Chans { kickBody }, RunOptions());
        const Buf b0 = mixOf (bass, kickBody), b1 = mixOf (align (out[0], lat), kickBody);
        const double d0 = bandDepth (b0, fs, 35.0, 80.0, a, b), d1 = bandDepth (b1, fs, 35.0, 80.0, a, b);
        const double ch = db (std::max (d1, 1e-6) / d0);
        ctx.add ("Smooth", "Low band: 55 vs 57.2 Hz beating reduced (bells)", ch <= -6.0,
                 fmt ("modulation depth 35-80 Hz: %.3f -> %.3f (%.1f dB)", d0, d1, ch), "<= -6 dB");
        ctx.wav ("smooth_lowband_before_mix.wav", { b0 }, fs, 3.0);
        ctx.wav ("smooth_lowband_after_mix.wav", { b1 }, fs, 3.0);
    }

    // ---- transparency where friction is low: a partial far from any reference partial is untouched
    {
        Buf t = sine (fs, secs, 1000.0, 0.3);
        addInto (t, sine (fs, secs, 3000.0, 0.3)); // reference sits at 3012 Hz
        Buf r = sine (fs, secs, 3012.0, 0.3);
        fr::Params p;
        p.amount = 1.0f;
        p.sensitivity = 0.8f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, Chans { t }, Chans { r }, RunOptions());
        const Buf o = align (out[0], lat);
        const double in1k = rms (bandPass (t, fs, 900, 1100), a, b), out1k = rms (bandPass (o, fs, 900, 1100), a, b);
        const double in3k = rms (bandPass (t, fs, 2950, 3050), a, b), out3k = rms (bandPass (o, fs, 2950, 3050), a, b);
        const double change1k = db (out1k / in1k), change3k = db (out3k / in3k);
        ctx.add ("Smooth", "Only the clashing partial is cut (1 kHz untouched)", std::abs (change1k) < 0.1 && change3k < -6.0,
                 fmt ("1 kHz partial %.3f dB, 3 kHz partial (12 Hz from ref) %.1f dB", change1k, change3k), "|1 kHz| < 0.1 dB, 3 kHz < -6 dB");
    }

    // ---- consonant material stays transparent (unison, octave); processing follows consonance ranking
    {
        struct Iv { const char* name; double ratio; };
        const Iv ivs[] = { { "unison", 1.0 }, { "octave", 2.0 }, { "fifth 3:2", 1.5 }, { "fourth 4:3", 4.0 / 3.0 },
                           { "major third 5:4", 1.25 }, { "tritone 45:32", 45.0 / 32.0 }, { "minor second 16:15", 16.0 / 15.0 } };
        double resid[7] = {};
        std::string meas;
        for (int k = 0; k < 7; ++k)
        {
            Buf t = saw (fs, secs, 220.0, 0.25);
            Buf r = saw (fs, secs, 220.0 / ivs[k].ratio, 0.25, 400, 0.7); // reference below the target
            fr::Params p;
            p.amount = 1.0f;
            p.sensitivity = 0.65f;
            auto eng = makeEngine (fs, 512, 1, p);
            const int lat = eng->currentLatency();
            Chans out = runEngine (*eng, Chans { t }, Chans { r }, RunOptions());
            const Buf o = align (out[0], lat);
            Buf diff (t.size());
            for (size_t i = 0; i < t.size(); ++i) diff[i] = o[i] - t[i];
            resid[k] = db (rms (diff, a, b) / rms (t, a, b));
            meas += fmt ("%s %.1f; ", ivs[k].name, std::max (-150.0, resid[k]));
            if (k < 2)
                ctx.add ("Smooth", fmt ("Consonant reference (%s) is near-transparent", ivs[k].name), resid[k] < -30.0,
                         fmt ("residual (out - dry) %s re input", resid[k] < -150 ? "-inf dB (bit-exact)" : fmt ("%.1f dB", resid[k]).c_str()),
                         "< -30 dB at default settings");
        }
        const bool ranked = resid[0] <= resid[1] && resid[1] < resid[2] && resid[2] < resid[6] && resid[2] < resid[5];
        ctx.add ("Smooth", "Amount of processing follows consonance (saw vs saw, default settings)", ranked,
                 "residual dB: " + meas, "unison <= octave < fifth < tritone, minor second");
    }
}
