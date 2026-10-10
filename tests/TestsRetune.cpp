// Friction - Retune tests (whole-signal and per-partial) including artifact checks
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

double crestDb (const Buf& x, size_t a, size_t b)
{
    double pk = 0.0;
    for (size_t i = a; i < b; ++i) pk = std::max (pk, (double) std::abs (x[i]));
    return db (pk / std::max (1e-12, rms (x, a, b)));
}

/** Drum-like hit: click + noise burst + decaying tonal body. */
void addDrum (Buf& dst, double fs, size_t at, double bodyHz)
{
    std::mt19937 rng ((unsigned) at);
    std::uniform_real_distribution<double> u (-1.0, 1.0);
    const size_t n = (size_t) (0.25 * fs);
    for (size_t i = 0; i < n && at + i < dst.size(); ++i)
    {
        const double t = (double) i / fs;
        double s = 0.5 * std::exp (-t / 0.004) * u (rng);           // noise burst
        s += 0.45 * std::exp (-t / 0.09) * std::sin (2.0 * kPi * bodyHz * t); // body
        if (i < 8) s += 0.6 * (1.0 - (double) i / 8.0);               // click
        dst[at + i] += (float) s;
    }
}

struct TransientReport { double preEchoDb = 0.0, attackDb = 0.0, worstPre = -300.0; };

/** Pre-echo (energy 25..2 ms before each onset re onset peak) and attack energy change vs the input. */
TransientReport transientCheck (const Buf& in, const Buf& out, double fs, const std::vector<size_t>& onsets)
{
    TransientReport r;
    double accPre = 0.0, accAtt = 0.0;
    for (size_t on : onsets)
    {
        const size_t preA = on - (size_t) (0.025 * fs), preB = on - (size_t) (0.002 * fs);
        const size_t atA = on, atB = on + (size_t) (0.006 * fs);
        const double peakIn = rms (in, atA, atB);
        const double preOut = rms (out, preA, preB), preIn = rms (in, preA, preB);
        const double attOut = rms (out, atA, atB);
        const double preRel = db (std::max (preOut, preIn * 0.0 + 1e-9) / peakIn);
        accPre += preRel;
        r.worstPre = std::max (r.worstPre, preRel);
        accAtt += db (attOut / peakIn);
    }
    r.preEchoDb = accPre / (double) onsets.size();
    r.attackDb = accAtt / (double) onsets.size();
    return r;
}

} // namespace

void ft::testsRetune (Context& ctx)
{
    const double fs = 48000.0;
    const double secs = ctx.quick ? 6.0 : 8.0;
    const size_t a = (size_t) (2.5 * fs), b = (size_t) ((secs - 0.5) * fs);

    // ---------------------------------------------------------------- beat rate -> 0 (saw pair, 8 cents)
    const double f0 = 220.0;
    Buf tgt = saw (fs, secs, f0 * std::pow (2.0, 8.0 / 1200.0), 0.25);
    Buf ref = saw (fs, secs, f0, 0.25, 400, 1.0);
    for (int style = 0; style < 2; ++style)
    {
        fr::Params p;
        p.mode = fr::Mode::Retune;
        p.retuneStyle = style == 0 ? fr::RetuneStyle::WholeAuto : fr::RetuneStyle::PerPartial;
        p.amount = 1.0f;
        p.maxCents = 50.0f;
        p.sensitivity = 0.8f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, Chans { tgt }, Chans { ref }, RunOptions());
        const Buf o = alignOut (out[0], lat);
        std::string per;
        double worstBeat = 0.0, beatBefore = 0.0;
        for (int h = 1; h <= 6; ++h)
        {
            const double fr0 = f0 * h;
            const double fIn = bandFrequency (tgt, fs, fr0 - 30, fr0 + 30, a, b);
            const double fOut = bandFrequency (o, fs, fr0 - 30, fr0 + 30, a, b);
            const double beatOut = std::abs (fOut - fr0);
            if (h == 1) beatBefore = std::abs (fIn - fr0);
            worstBeat = std::max (worstBeat, beatOut / h);
            per += fmt ("h%d %.3f Hz; ", h, beatOut);
        }
        const char* nm = style == 0 ? "Retune whole (auto): saw-pair beat rate -> 0" : "Retune per-partial: saw-pair beat rate -> 0";
        ctx.add ("Retune", nm, worstBeat < 0.05,
                 fmt ("fundamental beat %.3f Hz -> %.3f Hz; per harmonic: %s(%d splices)", beatBefore, std::abs (bandFrequency (o, fs, 190, 250, a, b) - f0),
                      per.c_str(), eng->probe().splices),
                 "beat rate < 0.05 Hz per harmonic order (from 1.02 Hz)");
        if (style == 0)
        {
            ctx.wav ("retune_whole_saws_before_mix.wav", { mixOf (tgt, ref) }, fs);
            ctx.wav ("retune_whole_saws_after_mix.wav", { mixOf (o, ref) }, fs);
        }
        else
        {
            ctx.wav ("retune_partial_saws_after_mix.wav", { mixOf (o, ref) }, fs);
        }
    }

    // ---------------------------------------------------------------- whole fixed: accuracy and splice ripple
    {
        fr::Params p;
        p.mode = fr::Mode::Retune;
        p.retuneStyle = fr::RetuneStyle::WholeFixed;
        p.wholeOffsetCents = 50.0f;
        p.amount = 1.0f;
        p.attackMs = 5.0f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Buf x = sine (fs, secs, 300.0, 0.5);
        Chans out = runEngine (*eng, Chans { x }, Chans { Buf (x.size(), 0.0f) }, RunOptions());
        const Buf o = alignOut (out[0], lat);
        const double fOut = bandFrequency (o, fs, 250, 350, a, b);
        const double err = 1200.0 * std::log2 (fOut / (300.0 * std::pow (2.0, 50.0 / 1200.0)));
        Buf env = bandEnvelope (o, fs, 250, 350);
        double mn = 1e9, mx = 0;
        for (size_t i = a; i < b; ++i) { mn = std::min (mn, (double) env[i]); mx = std::max (mx, (double) env[i]); }
        const double ripple = db (mx / mn);
        ctx.add ("Retune", "Whole fixed +50 c on a sine: pitch accuracy and splice ripple", std::abs (err) < 0.1 && ripple < 1.0,
                 fmt ("pitch error %.4f cents, level ripple %.2f dB over %d splices", err, ripple, eng->probe().splices),
                 "< 0.1 cent, < 1 dB");
    }

    // ---------------------------------------------------------------- transient smearing on drum hits
    {
        Buf drums ((size_t) (fs * secs), 0.0f);
        std::vector<size_t> onsets;
        for (double t = 0.6; t < secs - 0.4; t += 0.4)
        {
            onsets.push_back ((size_t) (t * fs));
            addDrum (drums, fs, (size_t) (t * fs), 180.0);
        }
        // reference tone near the drum body so Retune engages
        Buf r = harmonic (fs, secs, 186.0, { 0.3, 0.1 });
        for (int style = 0; style < 2; ++style)
        {
            fr::Params p;
            p.mode = fr::Mode::Retune;
            p.retuneStyle = style == 0 ? fr::RetuneStyle::PerPartial : fr::RetuneStyle::WholeFixed;
            p.wholeOffsetCents = 40.0f;
            p.amount = 1.0f;
            p.maxCents = 60.0f;
            p.sensitivity = 0.9f;
            p.attackMs = 5.0f;
            auto eng = makeEngine (fs, 512, 1, p);
            const int lat = eng->currentLatency();
            Chans out = runEngine (*eng, Chans { drums }, Chans { r }, RunOptions());
            const Buf o = alignOut (out[0], lat);
            std::vector<size_t> on2 (onsets.begin() + 2, onsets.end());
            const auto tIn = transientCheck (drums, drums, fs, on2);
            const auto tOut = transientCheck (drums, o, fs, on2);
            const bool ok = tOut.worstPre < -40.0 && std::abs (tOut.attackDb - tIn.attackDb) < 1.5;
            ctx.add ("Retune artifacts", style == 0 ? "Per-partial: no transient smearing on drum hits" : "Whole (fixed +40 c): no transient smearing on drum hits", ok,
                     fmt ("pre-echo (25-2 ms before hit) mean %.1f dB / worst %.1f dB re hit (input %.1f dB); attack energy change %.2f dB",
                          tOut.preEchoDb, tOut.worstPre, tIn.preEchoDb, tOut.attackDb - tIn.attackDb),
                     "worst pre-echo < -40 dB re hit, attack within 1.5 dB");
            if (style == 0)
            {
                ctx.wav ("retune_drums_before.wav", { drums }, fs);
                ctx.wav ("retune_drums_after_partial.wav", { o }, fs);
            }
            else
            {
                ctx.wav ("retune_drums_after_whole_fixed40.wav", { o }, fs);
                // onset timing: hits must stay on the grid (envelope crossing within 1 ms)
                double worstShift = 0.0;
                const Buf envIn = rmsEnvelope (drums, 48), envOut = rmsEnvelope (o, 48);
                for (size_t on : on2)
                {
                    auto firstCross = [&] (const Buf& e)
                    {
                        const double thr = 0.5 * e[on + (size_t) (0.003 * fs)];
                        for (size_t i = on - (size_t) (0.01 * fs); i < on + (size_t) (0.01 * fs); ++i)
                            if (e[i] > thr) return (double) i;
                        return (double) on;
                    };
                    worstShift = std::max (worstShift, std::abs (firstCross (envOut) - firstCross (envIn)) / fs * 1000.0);
                }
                ctx.add ("Retune artifacts", "Whole shift keeps hits on time (onset-aligned splices)", worstShift <= 1.0,
                         fmt ("worst onset displacement %.2f ms over %zu hits", worstShift, on2.size()), "<= 1 ms");
            }
        }
    }

    // ---------------------------------------------------------------- phasiness: peaky harmonic tone, uniform shift
    {
        std::vector<double> amps (24, 0.04);
        const double fT = 165.0;
        Buf t ((size_t) (fs * secs), 0.0f);
        for (int k = 1; k <= 24; ++k)
        {
            const double fk = fT * std::pow (2.0, 12.0 / 1200.0) * k;
            for (size_t i = 0; i < t.size(); ++i)
                t[i] += (float) (0.04 * std::cos (2.0 * kPi * fk * (double) i / fs));
        }
        Buf r = saw (fs, secs, fT, 0.2, 400, 0.3);
        fr::Params p;
        p.mode = fr::Mode::Retune;
        p.retuneStyle = fr::RetuneStyle::PerPartial;
        p.amount = 1.0f;
        p.maxCents = 50.0f;
        p.sensitivity = 0.9f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, Chans { t }, Chans { r }, RunOptions());
        const Buf o = alignOut (out[0], lat);
        const double cIn = crestDb (t, a, b), cOut = crestDb (o, a, b);
        // AM ripple of a shifted partial (harmonic 3)
        Buf env = bandEnvelope (o, fs, 3 * fT - 40, 3 * fT + 40);
        std::vector<float> v (env.begin() + (long) a, env.begin() + (long) b);
        std::sort (v.begin(), v.end());
        const double ripple = db (v[(size_t) (0.99 * (double) (v.size() - 1))] / v[(size_t) (0.01 * (double) (v.size() - 1))]);
        const double shiftC = 1200.0 * std::log2 (bandFrequency (o, fs, 3 * fT - 40, 3 * fT + 40, a, b) / (3 * fT * std::pow (2.0, 12.0 / 1200.0)));
        ctx.add ("Retune artifacts", "Per-partial: phasiness (vertical coherence of a pulse-like tone)", cIn - cOut < 2.0 && ripple < 1.0,
                 fmt ("crest factor %.2f dB -> %.2f dB; shifted partial AM ripple %.2f dB (99/1 pct); applied shift %.2f cents",
                      cIn, cOut, ripple, shiftC),
                 "crest drop < 2 dB, ripple < 1 dB");
        ctx.wav ("retune_pulse_before.wav", { t }, fs);
        ctx.wav ("retune_pulse_after_partial.wav", { o }, fs);
    }

    // ---------------------------------------------------------------- partials outside the processed range unchanged
    for (int m = 0; m < 2; ++m)
    {
        Buf t = sine (fs, secs, 100.0, 0.2);
        addInto (t, sine (fs, secs, 1000.0, 0.2));
        addInto (t, sine (fs, secs, 5000.0, 0.2));
        Buf r = sine (fs, secs, 104.0, 0.2);
        addInto (r, sine (fs, secs, 1012.0, 0.2));
        addInto (r, sine (fs, secs, 5025.0, 0.2));
        fr::Params p;
        p.mode = m == 0 ? fr::Mode::Retune : fr::Mode::Smooth;
        p.retuneStyle = fr::RetuneStyle::PerPartial;
        p.amount = 1.0f;
        p.sensitivity = 0.9f;
        p.rangeLowHz = 300.0f;
        p.rangeHighHz = 2000.0f;
        auto eng = makeEngine (fs, 512, 1, p);
        const int lat = eng->currentLatency();
        Chans out = runEngine (*eng, Chans { t }, Chans { r }, RunOptions());
        const Buf o = alignOut (out[0], lat);
        auto resid = [&] (double lo, double hi)
        {
            const Buf bi = bandPass (t, fs, lo, hi), bo = bandPass (o, fs, lo, hi);
            Buf d (bi.size());
            for (size_t i = 0; i < d.size(); ++i) d[i] = bo[i] - bi[i];
            return db (rms (d, a, b) / rms (bi, a, b));
        };
        const double r100 = resid (60, 140), r5k = resid (4900, 5100), r1k = resid (900, 1100);
        ctx.add ("Retune artifacts", m == 0 ? "Per-partial: partials outside Range untouched" : "Smooth: partials outside Range untouched",
                 r100 < -40.0 && r5k < -40.0 && r1k > -20.0,
                 fmt ("residual 100 Hz %.1f dB, 5 kHz %.1f dB (outside); 1 kHz (inside, processed) %.1f dB", r100, r5k, r1k),
                 "outside < -40 dB, inside processed");
    }
}
