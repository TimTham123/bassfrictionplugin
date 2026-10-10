// Friction - CPU measurement (one core, 48 kHz stereo target + stereo sidechain)
#include "Harness.h"

#include <chrono>
#include <fstream>

using namespace ft;

namespace
{
std::string cpuName()
{
#if defined(__linux__)
    std::ifstream f ("/proc/cpuinfo");
    std::string line;
    while (std::getline (f, line))
        if (line.rfind ("model name", 0) == 0)
            return line.substr (line.find (':') + 2);
#endif
    return "unknown CPU";
}
}

void ft::testsPerformance (Context& ctx)
{
    const double fs = 48000.0;
    const double secs = ctx.quick ? 8.0 : 20.0;

    // dense material: detuned triad + noise + kicks vs a four-note pad + bass
    Buf t ((size_t) (secs * fs), 0.0f), r ((size_t) (secs * fs), 0.0f);
    for (double f : { 196.0, 246.94, 293.66 })
        addInto (t, saw (fs, secs, f * std::pow (2.0, 12.0 / 1200.0), 0.08, 120));
    addInto (t, noise (fs, secs, 0.01, 2));
    KickParams kp;
    for (double h = 0.2; h < secs - 0.6; h += 0.5)
        addKick (t, fs, (size_t) (h * fs), kp);
    for (double f : { 98.0, 196.0, 246.94, 293.66, 392.0 })
        addInto (r, saw (fs, secs, f, 0.06, 120, f * 0.01));
    Buf t2 = t, r2 = r;
    for (size_t i = 0; i < t2.size(); ++i) { t2[i] *= 0.9f; r2[i] *= 0.85f; }
    const Chans tgt { t, t2 }, ref { r, r2 };

    struct Case { const char* name; fr::Mode mode; fr::LatencyMode lat; fr::RetuneStyle rs; fr::TensionStyle ts; double limit; };
    const Case cases[] = {
        { "Mix / Smooth", fr::Mode::Smooth, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth, 10.0 },
        { "Mix / Retune per-partial", fr::Mode::Retune, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth, 10.0 },
        { "Mix / Retune whole auto", fr::Mode::Retune, fr::LatencyMode::Mix, fr::RetuneStyle::WholeAuto, fr::TensionStyle::Synth, 10.0 },
        { "Mix / Tension synth", fr::Mode::Tension, fr::LatencyMode::Mix, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth, 10.0 },
        { "Track / Smooth", fr::Mode::Smooth, fr::LatencyMode::Track, fr::RetuneStyle::PerPartial, fr::TensionStyle::Synth, 4.0 },
    };
    for (const auto& c : cases)
    {
        fr::Params p;
        p.mode = c.mode;
        p.latencyMode = c.lat;
        p.retuneStyle = c.rs;
        p.tensionStyle = c.ts;
        p.amount = 0.8f;
        p.sensitivity = 0.8f;
        double best = 1e9;
        for (int rep = 0; rep < 2; ++rep)
        {
            auto e = makeEngine (fs, 256, 2, p);
            RunOptions o;
            o.block = 256;
            const auto t0 = std::chrono::steady_clock::now();
            runEngine (*e, tgt, ref, o);
            best = std::min (best, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
        }
        const double pct = 100.0 * best / secs;
        ctx.add ("Performance", fmt ("CPU %s (48 kHz, stereo + stereo SC, block 256)", c.name), pct < c.limit,
                 fmt ("%.2f %% of one core", pct), fmt ("< %.0f %%", c.limit));
    }
    ctx.notes.push_back ("Performance measured on " + cpuName() + " (cloud VM, single thread, -O3). Desktop CPUs are typically faster.");
}
