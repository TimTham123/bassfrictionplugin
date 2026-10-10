// Friction - analysis engine and roughness model tests
#include "Harness.h"

using namespace ft;

namespace
{

void testFFT (Context& ctx)
{
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    double worstRound = 0.0, worstRef = 0.0;
    for (int n : { 16, 256, 4096, 16384 })
    {
        fr::RealFFT fft;
        fft.prepare (n);
        std::vector<float> x ((size_t) n), y ((size_t) n);
        for (auto& v : x) v = d (rng);
        std::vector<fr::cfloat> X ((size_t) n / 2 + 1);
        fft.forward (x.data(), X.data());
        fft.inverse (X.data(), y.data());
        for (int i = 0; i < n; ++i)
            worstRound = std::max (worstRound, (double) std::abs (x[(size_t) i] - y[(size_t) i]));

        std::vector<std::complex<double>> ref ((size_t) n);
        for (int i = 0; i < n; ++i) ref[(size_t) i] = x[(size_t) i];
        fftc (ref, false);
        double scale = 0.0;
        for (int k = 0; k <= n / 2; ++k) scale = std::max (scale, std::abs (ref[(size_t) k]));
        for (int k = 0; k <= n / 2; ++k)
            worstRef = std::max (worstRef, std::abs (std::complex<double> (X[(size_t) k].real(), X[(size_t) k].imag()) - ref[(size_t) k]) / scale);
    }
    ctx.add ("Analysis", "Real FFT round trip and accuracy", worstRound < 1e-5 && worstRef < 1e-5,
             fmt ("round-trip max err %.2e, vs double DFT %.2e (rel)", worstRound, worstRef), "both < 1e-5");
}

void testRoughnessLut (Context& ctx)
{
    const auto& R = fr::Roughness::get();
    double worst = 0.0, worstCut = 0.0;
    for (double f1 : { 40.0, 110.0, 440.0, 1000.0, 4000.0, 12000.0 })
        for (double df = 0.0; df < 600.0; df += 0.37)
            for (double ratio : { 1.0, 0.5, 0.1 })
            {
                const double a1 = 0.5, a2 = 0.5 * ratio;
                const double ex = fr::Roughness::pairExact (f1, a1, f1 + df, a2);
                const double lu = R.pair ((float) f1, (float) a1, (float) (f1 + df), (float) a2);
                const double x = fr::Roughness::sFactor ((float) f1) * df;
                if (x < fr::Roughness::kXMax)
                    worst = std::max (worst, std::abs (ex - lu));
                else
                    worstCut = std::max (worstCut, std::abs (ex - lu));
            }
    ctx.add ("Analysis", "Roughness LUT vs exact formula", worst < 2e-4 && worstCut < 0.0055,
             fmt ("max abs err %.2e inside the window; %.2e from the x > 2 cut-off (by design)", worst, worstCut),
             "< 2e-4 inside; cut-off loss < 0.0055 (scale: 1 = worst pair)");

    // sanity of the curve shape itself
    const double peak = fr::Roughness::peakDeltaHz (440.0f);
    ctx.add ("Analysis", "Roughness peak position at 440 Hz", std::abs (peak - 25.80) < 0.05,
             fmt ("peak at %.2f Hz = %.0f%% of Zwicker CB (%.1f Hz)", peak, 100.0 * peak / fr::zwickerCbHz (440.0), fr::zwickerCbHz (440.0)),
             "25.80 Hz (Sethares x* = 0.2206)");
}

void testReassignment (Context& ctx)
{
    const double fs = 48000.0;
    const int N = 4096;
    fr::SpectralAnalyzer an;
    an.prepare (N, fs);
    std::mt19937 rng (11);
    std::uniform_real_distribution<double> u (0.0, 1.0);
    double worstCents = 0.0, worstHz = 0.0, worstDb = 0.0;
    int missed = 0, count = 0;
    for (int i = 0; i < 60; ++i)
    {
        const double f = 60.0 * std::pow (15000.0 / 60.0, u (rng));
        const double amp = 0.05 + 0.6 * u (rng);
        Buf x = sine (fs, (double) N / fs, f, amp, u (rng) * 6.28);
        const int np = an.analyze (x.data(), 20.0f, 20000.0f);
        int best = -1;
        for (int p = 0; p < np; ++p)
            if (best < 0 || std::abs (an.peaks()[p].freq - f) < std::abs (an.peaks()[best].freq - f))
                best = p;
        ++count;
        if (best < 0 || std::abs (an.peaks()[best].freq - f) > 2.0 * fs / N) { ++missed; continue; }
        const auto& pk = an.peaks()[best];
        worstHz = std::max (worstHz, std::abs (pk.freq - f));
        worstCents = std::max (worstCents, std::abs (1200.0 * std::log2 (pk.freq / f)));
        worstDb = std::max (worstDb, std::abs (db (pk.amp / amp)));
    }
    ctx.add ("Analysis", "STFT reassignment: frequency of single sines", missed == 0 && worstHz < 0.05,
             fmt ("%d sines 60 Hz-15 kHz: max err %.4f Hz (%.3f cents), %d missed", count, worstHz, worstCents, missed),
             "max err < 0.05 Hz, none missed");
    ctx.add ("Analysis", "STFT reassignment: amplitude (scalloping corrected)", missed == 0 && worstDb < 0.05,
             fmt ("max err %.4f dB", worstDb), "< 0.05 dB");

    // noise rejection: 3 sines in white noise at -50 dBFS
    Buf x = noise (fs, (double) N / fs, 0.0032, 3);
    const double fsines[3] = { 523.3, 1975.0, 6300.0 };
    for (double f : fsines) addInto (x, sine (fs, (double) N / fs, f, 0.2));
    int falsePeaks = 0, found = 0;
    const int frames = 20;
    for (int fr_ = 0; fr_ < frames; ++fr_)
    {
        Buf y = noise (fs, (double) N / fs, 0.0032, 100 + fr_);
        for (double f : fsines) addInto (y, sine (fs, (double) N / fs, f, 0.2, fr_));
        const int np = an.analyze (y.data(), 20.0f, 20000.0f);
        for (int p = 0; p < np; ++p)
        {
            bool real = false;
            for (double f : fsines)
                if (std::abs (an.peaks()[p].freq - f) < 2.0 * fs / N) real = true;
            if (real) ++found; else ++falsePeaks;
        }
    }
    // vibrato / glides must still be detected (prominence path)
    {
        int det = 0, tot = 0;
        std::string worstCase;
        int worstDet = 1 << 30;
        for (double depthCents : { 25.0, 50.0, 100.0 })
            for (double fc : { 300.0, 1000.0, 3000.0 })
            {
                const size_t len = (size_t) (fs * 2);
                Buf v (len);
                double ph = 0.0;
                for (size_t i = 0; i < len; ++i)
                {
                    const double f = fc * std::pow (2.0, depthCents / 1200.0 * std::sin (2.0 * kPi * 5.5 * (double) i / fs));
                    ph += 2.0 * kPi * f / fs;
                    v[i] = (float) (0.3 * std::sin (ph));
                }
                int d = 0, tt = 0;
                for (size_t s0 = 0; s0 + N < len; s0 += 1024)
                {
                    const int np = an.analyze (v.data() + s0, 20.0f, 20000.0f);
                    bool ok = false;
                    for (int p = 0; p < np; ++p)
                        if (std::abs (1200.0 * std::log2 (an.peaks()[p].freq / fc)) < depthCents + 20.0) ok = true;
                    d += ok ? 1 : 0;
                    ++tt;
                }
                det += d;
                tot += tt;
                if (d < worstDet) { worstDet = d; worstCase = fmt ("+-%.0f c @ %.0f Hz: %d/%d", depthCents, fc, d, tt); }
            }
        ctx.add ("Analysis", "Peak picking keeps vibrato partials (5.5 Hz, +-25..100 c)", det == tot,
                 fmt ("%d/%d frames detected; worst %s", det, tot, worstCase.c_str()), "detected in every frame");
    }

    ctx.add ("Analysis", "Peak picking rejects noise (3 sines in -50 dBFS noise)", found == 3 * frames && falsePeaks <= frames / 4,
             fmt ("%d/%d true peaks found, %d false peaks in %d frames", found, 3 * frames, falsePeaks, frames),
             "all true peaks, <= 0.25 false peaks per frame");
}

void testLowBand (Context& ctx)
{
    const double fs = 48000.0;
    double worst = 0.0, worstH = 0.0, worstLock = 0.0, lock31 = 0.0;
    std::string lockList;
    for (double f : { 30.87, 41.2, 55.0, 73.42, 98.0, 130.8, 196.0, 293.7 })
    {
        for (int withHarm = 0; withHarm < 2; ++withHarm)
        {
            fr::LowBandTracker tr;
            tr.prepare (fs);
            tr.setHarmonicLimitHz (400.0f);
            Buf x (size_t (fs * 1.2), 0.0f);
            const size_t start = (size_t) (0.2 * fs);
            Buf tone = withHarm ? harmonic (fs, 1.0, f, { 0.5, 0.35, 0.25, 0.15, 0.1, 0.05 }) : sine (fs, 1.0, f, 0.5);
            addInto (x, tone, 1.0, start);
            std::vector<double> errs;
            double lockTime = -1.0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                if (tr.push (x[i]))
                {
                    const auto& e = tr.latest();
                    const double tt = (double) e.time / fs - 0.2;
                    if (e.voiced && lockTime < 0.0 && std::abs (1200.0 * std::log2 (e.f0 / f)) < 5.0)
                        lockTime = (double) i / fs - 0.2;
                    if (tt > 0.1 && tt < 0.95)
                        errs.push_back (e.voiced ? std::abs (1200.0 * std::log2 (e.f0 / f)) : 1200.0);
                }
            }
            std::sort (errs.begin(), errs.end());
            const double e95 = errs.empty() ? 1e9 : errs[(size_t) (0.95 * (double) (errs.size() - 1))];
            (withHarm ? worstH : worst) = std::max (withHarm ? worstH : worst, e95);
            const double lt = lockTime < 0 ? 9.0 : lockTime;
            if (f < 40.0) lock31 = std::max (lock31, lt);
            else worstLock = std::max (worstLock, lt);
            if (withHarm) lockList += fmt ("%.0f Hz %.0f ms; ", f, lt * 1000.0);
        }
    }
    ctx.add ("Analysis", "Low-band tracker: pure sines 31-294 Hz", worst < 1.0,
             fmt ("95th-percentile error %.3f cents", worst), "< 1 cent");
    ctx.add ("Analysis", "Low-band tracker: harmonic bass tones 31-294 Hz", worstH < 1.0,
             fmt ("95th-percentile error %.3f cents", worstH), "< 1 cent");
    ctx.add ("Analysis", "Low-band tracker: time to lock within 5 cents", worstLock <= 0.05,
             fmt ("worst %.0f ms for 41-294 Hz, %.0f ms at 31 Hz (B0); harmonic tones: %s", worstLock * 1000.0, lock31 * 1000.0, lockList.c_str()),
             "<= 50 ms after note start for f0 >= 40 Hz (31 Hz reported)");

    // kick pitch drop tracking
    KickParams kp;
    Buf x (size_t (fs * 2.0), 0.0f);
    std::vector<size_t> hits = { (size_t) (0.2 * fs), (size_t) (0.9 * fs), (size_t) (1.5 * fs) };
    for (size_t h : hits) addKick (x, fs, h, kp);
    fr::LowBandTracker tr;
    tr.prepare (fs);
    std::vector<double> tailErr, dropErr;
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (! tr.push (x[i])) continue;
        const auto& e = tr.latest();
        if (! e.voiced) continue;
        // which hit
        double tt = -1.0;
        for (size_t h : hits)
            if ((double) e.time >= (double) h)
                tt = ((double) e.time - (double) h) / fs;
        if (tt < 0.0 || tt > 0.55) continue;
        const double truth = kickFreqAt (kp, tt);
        const double err = std::abs (1200.0 * std::log2 (e.f0 / truth));
        (tt >= 0.08 ? tailErr : dropErr).push_back (err);
    }
    auto med = [] (std::vector<double> v) { if (v.empty()) return 1e9; std::sort (v.begin(), v.end()); return v[v.size() / 2]; };
    const double mt = med (tailErr), md = med (dropErr);
    ctx.add ("Analysis", "Low-band tracker: kick pitch drop 160->52 Hz", mt < 3.0 && ! dropErr.empty(),
             fmt ("tail (>=80 ms) median err %.2f cents (%zu est.); drop phase median err %.0f cents (%zu est.)", mt, tailErr.size(), md, dropErr.size()),
             "tail < 3 cents; drop phase reported");
}

/** Average friction of the target partial nearest fT over frames after `settle` seconds. */
double measureFriction (double fs, double fT, double fR, double ampT, double ampR, double seconds, int numCh = 1)
{
    fr::Params p;
    p.amount = 0.0f; // analysis only
    auto eng = makeEngine (fs, 512, numCh, p);
    Chans tgt (numCh, sine (fs, seconds, fT, ampT));
    Chans ref (1, sine (fs, seconds, fR, ampR));
    double acc = 0.0;
    int n = 0;
    int lastFrame = -1;
    RunOptions o;
    o.afterBlock = [&] (std::int64_t pos, fr::FrictionEngine& e)
    {
        const auto& pr = e.probe();
        if (pr.frames == lastFrame || (double) pos < 0.8 * fs) return;
        lastFrame = pr.frames;
        int best = -1;
        for (int i = 0; i < pr.nT; ++i)
            if (best < 0 || std::abs (pr.T[i].freq - fT) < std::abs (pr.T[best].freq - fT)) best = i;
        if (best >= 0 && std::abs (pr.T[best].freq - fT) < 2.0)
        {
            acc += pr.T[best].friction;
            ++n;
        }
    };
    runEngine (*eng, tgt, ref, o);
    return n > 0 ? acc / n : -1.0;
}

} // namespace

void ft::testsAnalysis (Context& ctx)
{
    testFFT (ctx);
    testRoughnessLut (ctx);
    testReassignment (ctx);
    testLowBand (ctx);

    // ---- sine pairs: friction must follow the chosen roughness curve
    const double fs = 48000.0;
    const double amp = 0.5;
    const double level = std::pow (amp * amp, 0.1); // X^0.1 term for equal amplitudes, Y = 1
    for (double base : { 440.0, 60.0 })
    {
        std::string meas;
        bool pass = true;
        double prev = -1.0, at30 = 0.0, at100 = 0.0;
        for (double d : { 0.0, 1.0, 3.0, 10.0, 30.0, 100.0 })
        {
            const double m = measureFriction (fs, base, base + d, amp, amp, 1.6);
            const double theory = level * fr::Roughness::zHatExact (fr::Roughness::sFactor ((float) base) * d);
            const double tol = 0.02 + 0.05 * theory;
            if (m < 0.0 || std::abs (m - theory) > tol) pass = false;
            if (d <= 30.0 && m < prev - 1e-6) pass = false;
            prev = m;
            if (d == 30.0) at30 = m;
            if (d == 100.0) at100 = m;
            meas += fmt ("d=%g: %.3f (th %.3f); ", d, m, theory);
        }
        if (! (at100 < at30)) pass = false;
        ctx.add ("Analysis", fmt ("Sine pairs at %.0f Hz follow roughness curve (%s path)", base, base < 200 ? "low-band YIN" : "STFT"),
                 pass, meas, "|meas - theory| <= 0.02 + 5%, rising to ~25 Hz, falling at 100 Hz");
    }

    // low-band cross-check: a 52 Hz sine under a 222 Hz harmonic tone (YIN alone locks to 55.6 Hz)
    {
        const double fs2 = 48000.0;
        Buf t = harmonic (fs2, 3.0, 222.3, { 0.25, 0.18, 0.12, 0.08, 0.05, 0.04, 0.03 });
        addInto (t, sine (fs2, 3.0, 52.0, 0.3));
        Buf r = sine (fs2, 3.0, 55.0, 0.3);
        fr::Params p;
        p.amount = 0.0f;
        auto eng = makeEngine (fs2, 512, 1, p);
        double worst = 0.0;
        int frames = 0, last = -1;
        bool wrong = false;
        RunOptions o;
        o.afterBlock = [&] (std::int64_t pos, fr::FrictionEngine& e)
        {
            const auto& pr = e.probe();
            if (pr.frames == last || (double) pos < 1.0 * fs2) return;
            last = pr.frames;
            ++frames;
            double bestD = 1e9;
            for (int i = 0; i < pr.nT; ++i)
            {
                bestD = std::min (bestD, std::abs ((double) pr.T[i].freq - 52.0));
                if (std::abs (pr.T[i].freq - 55.58f) < 1.0f) wrong = true;
            }
            worst = std::max (worst, bestD);
        };
        runEngine (*eng, Chans { t }, Chans { r }, o);
        ctx.add ("Analysis", "Low-band cross-check: 52 Hz sine under a 222 Hz tone", worst < 0.1 && ! wrong,
                 fmt ("52 Hz partial found within %.3f Hz in %d frames; subharmonic 55.6 Hz reported: %s", worst, frames, wrong ? "yes" : "no"),
                 "52 Hz within 0.1 Hz, no false 55.6 Hz partial");
    }

    // sample-rate independence of the friction measurement
    {
        std::string meas;
        bool pass = true;
        const double ref = measureFriction (48000.0, 440.0, 450.0, amp, amp, 1.6);
        for (double sr : { 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        {
            const double m = measureFriction (sr, 440.0, 450.0, amp, amp, 1.6);
            if (std::abs (m - ref) > 0.01) pass = false;
            meas += fmt ("%.1fk: %.4f; ", sr / 1000.0, m);
        }
        ctx.add ("Analysis", "Friction (440 vs 450 Hz) independent of sample rate", pass,
                 fmt ("48k: %.4f; ", ref) + meas, "all within 0.01 of the 48 kHz value");
    }
}
