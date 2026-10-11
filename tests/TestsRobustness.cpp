// Friction - robustness tests: NaN/Inf, DC, denormals, block size, sample rate, stereo linking
#include "AllocCounter.h"
#include "Harness.h"

#include <chrono>

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

struct ModeCase { const char* name; fr::Mode mode; fr::LatencyMode lat; fr::RetuneStyle rs; fr::TensionStyle ts; };
const ModeCase kModes[] = {
    { "Smooth/Mix", fr::Mode::Smooth, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth },
    { "Smooth/Track", fr::Mode::Smooth, fr::LatencyMode::Track, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth },
    { "Retune per-partial", fr::Mode::Retune, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth },
    { "Retune whole auto", fr::Mode::Retune, fr::LatencyMode::Mix, fr::RetuneStyle::WholeAuto, fr::TensionStyle::Synth },
    { "Tension synth", fr::Mode::Tension, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth },
    { "Tension push", fr::Mode::Tension, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, fr::TensionStyle::Push },
};

fr::Params paramsFor (const ModeCase& m)
{
    fr::Params p;
    p.mode = m.mode;
    p.latencyMode = m.lat;
    p.retuneStyle = m.rs;
    p.tensionStyle = m.ts;
    p.amount = 0.9f;
    p.sensitivity = 0.8f;
    return p;
}

/** Busy test material: stereo target (detuned saw + noise + kick) and stereo reference. */
void material (double fs, double secs, Chans& tgt, Chans& ref)
{
    Buf t = saw (fs, secs, 196.0 * std::pow (2.0, 14.0 / 1200.0), 0.2);
    addInto (t, noise (fs, secs, 0.01, 4));
    KickParams kp;
    for (double h = 0.3; h < secs - 0.6; h += 0.7)
        addKick (t, fs, (size_t) (h * fs), kp);
    Buf t2 = t;
    for (size_t i = 0; i < t2.size(); ++i)
        t2[i] = 0.8f * t[i] + 0.05f * std::sin ((float) i * 0.013f);
    Buf r = saw (fs, secs, 196.0, 0.2, 400, 0.4);
    addInto (r, sine (fs, secs, 55.0, 0.3));
    Buf r2 = r;
    tgt = { t, t2 };
    ref = { r, r2 };
}

} // namespace

void ft::testsRobustness (Context& ctx)
{
    const double fs = 48000.0;

    // ---- NaN / Inf in target and sidechain
    {
        const double secs = 4.0;
        Chans tgt, ref;
        material (fs, secs, tgt, ref);
        Chans tgtBad = tgt, refBad = ref, tgtZero = tgt, refZero = ref;
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        for (size_t i = (size_t) (1.0 * fs); i < (size_t) (1.01 * fs); ++i)
        {
            tgtBad[0][i] = nan; tgtBad[1][i] = inf; refBad[0][i] = -inf; refBad[1][i] = nan;
            tgtZero[0][i] = 0.0f; tgtZero[1][i] = 0.0f; refZero[0][i] = 0.0f; refZero[1][i] = 0.0f;
        }
        tgtBad[0][(size_t) (2.0 * fs)] = inf;
        tgtZero[0][(size_t) (2.0 * fs)] = 0.0f;
        bool finite = true, same = true;
        for (const auto& m : kModes)
        {
            auto e1 = makeEngine (fs, 512, 2, paramsFor (m));
            auto e2 = makeEngine (fs, 512, 2, paramsFor (m));
            Chans o1 = runEngine (*e1, tgtBad, refBad, RunOptions());
            Chans o2 = runEngine (*e2, tgtZero, refZero, RunOptions());
            for (int c = 0; c < 2; ++c)
            {
                finite = finite && allFinite (o1[(size_t) c]);
                same = same && maxAbsDiff (o1[(size_t) c], o2[(size_t) c]) == 0.0;
            }
        }
        ctx.add ("Robustness", "NaN/Inf in target and sidechain (all modes)", finite && same,
                 fmt ("output finite: %s; identical to zero-substituted input: %s", finite ? "yes" : "NO", same ? "yes" : "NO"),
                 "finite, non-finite samples treated as 0, state recovers");
    }

    // ---- DC offset
    {
        const double secs = 4.0;
        Buf t = sine (fs, secs, 220.0, 0.3);
        Buf r = sine (fs, secs, 226.0, 0.3);
        for (auto& v : t) v += 0.4f;
        for (auto& v : r) v -= 0.3f;
        bool ok = true;
        std::string meas;
        for (const auto& m : kModes)
        {
            auto e = makeEngine (fs, 512, 1, paramsFor (m));
            Chans o = runEngine (*e, Chans { t }, Chans { r }, RunOptions());
            const Buf al = alignOut (o[0], e->currentLatency());
            double mean = 0.0;
            const size_t a = (size_t) (1.5 * fs), b = (size_t) (3.5 * fs);
            for (size_t i = a; i < b; ++i) mean += al[i];
            mean /= (double) (b - a);
            const bool fin = allFinite (o[0]);
            const bool dcKept = std::abs (mean - 0.4) < 0.01;
            if (! fin || ! dcKept) ok = false;
            meas += fmt ("%s mean %.4f; ", m.name, mean);
        }
        ctx.add ("Robustness", "DC offset (+0.4 target, -0.3 sidechain)", ok, meas, "finite, DC passed through unchanged (+-0.01), no false partials");
    }

    // ---- denormals: tiny signals must not slow processing down
    {
        const double secs = 6.0;
        Chans tgt, ref;
        material (fs, secs, tgt, ref);
        Chans tiny = tgt, tinyR = ref;
        for (auto& ch : tiny) for (auto& v : ch) v *= 1.0e-36f;
        for (auto& ch : tinyR) for (auto& v : ch) v *= 1.0e-36f;
        auto timeRun = [&] (const Chans& t, const Chans& r)
        {
            auto e = makeEngine (fs, 512, 2, paramsFor (kModes[0]));
            const auto t0 = std::chrono::steady_clock::now();
            Chans o = runEngine (*e, t, r, RunOptions());
            const double s = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            return std::make_pair (s, allFinite (o[0]) && allFinite (o[1]));
        };
        const auto normal = timeRun (tgt, ref);
        const auto small = timeRun (tiny, tinyR);
        const double ratio = small.first / normal.first;
        ctx.add ("Robustness", "Denormals: 1e-36-level input", small.second && ratio < 2.0,
                 fmt ("processing time ratio tiny/normal %.2f, output finite %s", ratio, small.second ? "yes" : "no"),
                 "ratio < 2, finite (FTZ/DAZ + flushing)");
    }

    // ---- block-size independence (bit exact) for every mode
    {
        const double secs = 3.0;
        Chans tgt, ref;
        material (fs, secs, tgt, ref);
        bool ok = true;
        std::string meas;
        for (const auto& m : kModes)
        {
            Chans refOut;
            double worst = 0.0;
            for (int block : { 512, 1, 7, 64, 333, 4096 })
            {
                auto e = makeEngine (fs, block, 2, paramsFor (m));
                RunOptions o;
                o.block = block;
                Chans out = runEngine (*e, tgt, ref, o);
                if (block == 512) { refOut = out; continue; }
                for (int c = 0; c < 2; ++c)
                    worst = std::max (worst, maxAbsDiff (out[(size_t) c], refOut[(size_t) c]));
            }
            if (worst != 0.0) ok = false;
            meas += fmt ("%s %s; ", m.name, worst == 0.0 ? "bit-exact" : fmt ("%.1e", worst).c_str());
        }
        ctx.add ("Robustness", "Block-size independence (1, 7, 64, 333, 512, 4096)", ok, meas, "bit-identical output in every mode");
    }

    // ---- sample-rate independence of processing results
    {
        bool ok = true;
        std::string meas;
        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const double secs = 6.0;
            const size_t a = (size_t) (2.0 * sr), b = (size_t) (5.5 * sr);
            Buf t = saw (sr, secs, 220.0 * std::pow (2.0, 8.0 / 1200.0), 0.25, 60);
            Buf r = saw (sr, secs, 220.0, 0.25, 60, 1.0);
            fr::Params p;
            p.amount = 1.0f;
            p.sensitivity = 0.8f;
            auto e = makeEngine (sr, 512, 1, p);
            Chans o = runEngine (*e, Chans { t }, Chans { r }, RunOptions());
            const Buf al = alignOut (o[0], e->currentLatency());
            // fundamental and 4th harmonic attenuation
            const double g1 = db (rms (bandPass (al, sr, 190, 250), a, b) / rms (bandPass (t, sr, 190, 250), a, b));
            const double g4 = db (rms (bandPass (al, sr, 850, 910), a, b) / rms (bandPass (t, sr, 850, 910), a, b));
            p.mode = fr::Mode::Retune;
            p.retuneStyle = fr::RetuneStyle::WholeAuto;
            auto e2 = makeEngine (sr, 512, 1, p);
            Chans o2 = runEngine (*e2, Chans { t }, Chans { r }, RunOptions());
            const Buf al2 = alignOut (o2[0], e2->currentLatency());
            const double beat = std::abs (bandFrequency (al2, sr, 190, 250, a, b) - 220.0);
            meas += fmt ("%.1fk: Smooth h1 %.1f dB h4 %.1f dB, Retune beat %.3f Hz; ", sr / 1000.0, g1, g4, beat);
            if (g1 > -3.0 || g4 > -3.0 || beat > 0.05) ok = false;
        }
        ctx.add ("Robustness", "Sample-rate independence of processing (44.1-192 kHz)", ok, meas,
                 "Smooth cuts the clashing partials (< -3 dB) and Retune beat < 0.05 Hz at every rate");
    }

    // ---- stereo linking: image (L/R ratio) preserved
    {
        const double secs = 4.0;
        Buf l = saw (fs, secs, 220.0 * std::pow (2.0, 10.0 / 1200.0), 0.3);
        Buf r = l;
        for (auto& v : r) v *= 0.5f;
        Buf ref = saw (fs, secs, 220.0, 0.3, 400, 0.3);
        bool ok = true;
        std::string meas;
        for (const auto& m : kModes)
        {
            if (m.mode == fr::Mode::Tension && m.ts == fr::TensionStyle::Synth)
                continue; // synth adds a centred (mono) layer by design
            auto e = makeEngine (fs, 512, 2, paramsFor (m));
            Chans o = runEngine (*e, Chans { l, r }, Chans { ref }, RunOptions());
            const size_t a = (size_t) (1.5 * fs), b = (size_t) (3.5 * fs);
            double worst = 0.0;
            for (size_t i = a; i < b; ++i)
                worst = std::max (worst, (double) std::abs (o[1][i] - 0.5f * o[0][i]));
            if (worst > 1e-5) ok = false;
            meas += fmt ("%s %.1e; ", m.name, worst);
        }
        ctx.add ("Robustness", "Stereo linked: R = 0.5 L stays 0.5 L after processing", ok, meas, "max |R - 0.5 L| <= 1e-5");
    }

    // ---- live switching of mode / style / latency / delta: no clicks
    {
        const double secs = 8.0;
        Buf t = sine (fs, secs, 220.0 * std::pow (2.0, 10.0 / 1200.0), 0.5);
        Buf r = sine (fs, secs, 220.0, 0.5);
        fr::Params p0;
        p0.amount = 1.0f;
        p0.sensitivity = 0.9f;
        auto e = makeEngine (fs, 256, 1, p0);
        RunOptions o;
        o.block = 256;
        o.beforeBlock = [&] (std::int64_t pos, fr::Params& q)
        {
            const int phase = (int) ((double) pos / fs / 0.5);
            q.mode = (fr::Mode) (phase % 3);
            q.retuneStyle = (phase / 3) % 2 ? fr::RetuneStyle::WholeAuto : fr::RetuneStyle::PerPartial;
            q.latencyMode = (phase % 7 == 6) ? fr::LatencyMode::Track : fr::LatencyMode::Mix;
            q.delta = phase % 5 == 4;
        };
        Chans out = runEngine (*e, Chans { t }, Chans { r }, o);
        const Buf hf = bandPass (out[0], fs, 6000.0, 23000.0);
        const double outRms = rms (out[0], (size_t) fs, out[0].size());
        double worst = 0.0, at = 0.0;
        for (size_t i = (size_t) (0.2 * fs); i + 240 < hf.size(); i += 120)
        {
            const double v = rms (hf, i, i + 240);
            if (v > worst) { worst = v; at = (double) i / fs; }
        }
        ctx.add ("Robustness", "Live switching (mode, style, latency, delta every 0.5 s): no clicks", db (worst / outRms) < -40.0 && allFinite (out[0]),
                 fmt ("worst 5 ms burst above 6 kHz %.1f dB re output RMS (at %.2f s) on a 226 Hz sine", db (worst / outRms), at),
                 "< -40 dB (fades are 10 ms raised-cosine)");
    }

    // ---- real-time safety: no heap allocation inside setParams() / process()
    {
        const double secs = 6.0;
        Chans tgt, ref;
        material (fs, secs, tgt, ref);
        const size_t n = tgt[0].size();
        struct Case { const char* name; int block; bool sidechain; int kind; };
        const Case cases[] = {
            { "Smooth/Mix", 256, true, 0 }, { "Smooth/Track", 256, true, 1 }, { "Retune per-partial", 256, true, 2 },
            { "Retune whole auto", 256, true, 3 }, { "Retune whole fixed +50 c", 256, true, 4 }, { "Tension synth", 256, true, 5 },
            { "Tension push", 256, true, 6 }, { "Self (forced)", 256, true, 7 }, { "no sidechain bus", 256, false, 0 },
            { "live switching (mode/style/latency/delta/bypass)", 333, true, 8 },
        };
        long long total = 0;
        long long calls = 0;
        std::string bad;
        for (const auto& c : cases)
        {
            fr::Params p;
            p.amount = 0.9f;
            p.sensitivity = 0.8f;
            p.learn = true; // Advisor frames are pushed to the FIFO as well
            switch (c.kind)
            {
                case 1: p.latencyMode = fr::LatencyMode::Track; break;
                case 2: p.mode = fr::Mode::Retune; p.retuneStyle = fr::RetuneStyle::PerPartial; break;
                case 3: p.mode = fr::Mode::Retune; p.retuneStyle = fr::RetuneStyle::WholeAuto; break;
                case 4: p.mode = fr::Mode::Retune; p.retuneStyle = fr::RetuneStyle::WholeFixed; p.wholeOffsetCents = 50.0f; break;
                case 5: p.mode = fr::Mode::Tension; p.tensionStyle = fr::TensionStyle::Synth; break;
                case 6: p.mode = fr::Mode::Tension; p.tensionStyle = fr::TensionStyle::Push; break;
                case 7: p.reference = fr::ReferenceSource::Self; break;
                default: break;
            }
            auto e = makeEngine (fs, c.block, 2, p);
            Chans work = tgt;
            long long count = 0;
            for (size_t pos = 0; pos < n; pos += (size_t) c.block)
            {
                const int len = (int) std::min ((size_t) c.block, n - pos);
                if (c.kind == 8)
                {
                    const int phase = (int) ((double) pos / fs / 0.25);
                    p.mode = (fr::Mode) (phase % 3);
                    p.retuneStyle = (fr::RetuneStyle) ((phase / 3) % 3);
                    p.tensionStyle = (fr::TensionStyle) ((phase / 2) % 2);
                    p.latencyMode = (phase % 7 == 6) ? fr::LatencyMode::Track : fr::LatencyMode::Mix;
                    p.delta = phase % 5 == 4;
                    p.bypass = phase % 11 == 10;
                }
                float* io[2] = { work[0].data() + pos, work[1].data() + pos };
                const float* sc[2] = { ref[0].data() + pos, ref[1].data() + pos };
                AllocCounter::begin();
                e->setParams (p);
                e->process (io, 2, c.sidechain ? sc : nullptr, c.sidechain ? 2 : 0, len, c.sidechain);
                count += AllocCounter::end();
                ++calls;
            }
            total += count;
            if (count != 0)
                bad += fmt ("%s: %lld; ", c.name, count);
        }
        // self-check: the counter must see a deliberate allocation (volatile sink: cannot be elided)
        static std::vector<float>* volatile sink = nullptr;
        AllocCounter::begin();
        sink = new std::vector<float> (16);
        const long long probe = AllocCounter::end();
        delete sink;
        sink = nullptr;
        ctx.add ("Robustness", "Real-time safety: no heap allocation in process()", total == 0 && probe >= 2,
                 (total == 0 ? fmt ("0 allocations in %lld setParams()+process() calls over 10 configurations (all modes, Self, no sidechain, live switching, Advisor learning)", calls)
                             : fmt ("%lld allocations: %s", total, bad.c_str()))
                     + fmt ("; counter self-check saw %lld allocations for one new std::vector", probe),
                 "0 calls to operator new on the audio thread after prepare(); counter self-check >= 2");
    }

    // ---- prepare() must not reset the Advisor FIFO: the Advisor thread may be popping at that moment,
    //      and resetting head/tail under a concurrent pop can leave the FIFO permanently "full"
    {
        Chans tgt, ref;
        material (fs, 2.0, tgt, ref);
        fr::Params p;
        p.learn = true;
        auto e = makeEngine (fs, 512, 2, p);
        RunOptions o;
        runEngine (*e, tgt, ref, o);
        const auto queued = e->advisorFifo().size();
        e->prepare (96000.0, 256, 2);
        const auto after = e->advisorFifo().size();
        fr::AdvisorFrame f;
        size_t popped = 0;
        while (e->advisorFifo().pop (f))
            ++popped;
        runEngine (*e, tgt, ref, o);
        const auto refilled = e->advisorFifo().size();
        const bool ok = queued > 0 && after == queued && popped == queued && refilled > 0;
        ctx.add ("Robustness", "Advisor FIFO untouched by prepare() (Advisor thread may be popping)", ok,
                 fmt ("%zu frames queued, %zu after prepare(), %zu popped, %zu queued after more audio", (size_t) queued, (size_t) after, popped, (size_t) refilled),
                 "prepare() leaves the FIFO's contents and indices unchanged; frames keep arriving");
    }

    // ---- hot input
    {
        const double secs = 3.0;
        Chans tgt, ref;
        material (fs, secs, tgt, ref);
        for (auto& ch : tgt) for (auto& v : ch) v *= 8.0f;
        bool ok = true;
        double peak = 0.0;
        for (const auto& m : kModes)
        {
            auto e = makeEngine (fs, 512, 2, paramsFor (m));
            Chans o = runEngine (*e, tgt, ref, RunOptions());
            for (auto& ch : o) { ok = ok && allFinite (ch); for (float v : ch) peak = std::max (peak, (double) std::abs (v)); }
        }
        ctx.add ("Robustness", "Hot input (+18 dBFS peaks)", ok && peak < 20.0, fmt ("finite, output peak %.2f", peak), "finite, bounded");
    }
}
