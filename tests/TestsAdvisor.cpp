// Friction - Tuning Advisor tests
#include "Harness.h"

using namespace ft;

namespace
{

struct BassNote { double f; double start, end; };

/** Runs the engine with Learn on and feeds the published Advisor frames into an Advisor. */
fr::AdvisorResult learnFromAudio (double fs, const Buf& target, const Buf& ref, fr::Params p, int* framesOut = nullptr)
{
    p.learn = true;
    p.amount = 0.0f;
    auto eng = makeEngine (fs, 512, 1, p);
    fr::Advisor adv;
    int frames = 0;
    RunOptions o;
    o.afterBlock = [&] (std::int64_t, fr::FrictionEngine& e)
    {
        fr::AdvisorFrame f;
        while (e.advisorFifo().pop (f))
        {
            adv.consume (f);
            ++frames;
        }
    };
    runEngine (*eng, Chans { target }, Chans { ref }, o);
    if (framesOut) *framesOut = frames;
    return adv.finish();
}

} // namespace

void ft::testsAdvisor (Context& ctx)
{
    const double fs = 48000.0;

    // ---- steady detuned saws: the advisor must find the detune
    {
        const double secs = 6.0;
        Buf t = saw (fs, secs, 196.0 * std::pow (2.0, 17.0 / 1200.0), 0.25);
        Buf r = saw (fs, secs, 196.0, 0.25, 400, 0.4);
        fr::Params p;
        int frames = 0;
        const auto res = learnFromAudio (fs, t, r, p, &frames);
        ctx.add ("Advisor", "Detuned saw pair (+17 cents): best offset", res.valid && std::abs (res.bestCents + 17.0f) <= 1.0f,
                 fmt ("best %+.0f cents, expected friction reduction %.0f %% (%d frames)", res.bestCents, res.reductionPct, frames),
                 "-17 +-1 cents");
    }

    // ---- kick vs bass line
    const double secs = ctx.quick ? 12.0 : 16.0;
    KickParams kp;
    kp.fStart = 160.0;
    kp.fEnd = 52.0;
    kp.dropTau = 0.025;
    kp.ampTau = 0.30;
    kp.amp = 0.8;
    std::vector<double> hits;
    Buf kick ((size_t) (secs * fs), 0.0f);
    for (double h = 0.05; h < secs - 0.6; h += 0.5)
    {
        hits.push_back (h);
        addKick (kick, fs, (size_t) (h * fs), kp);
    }
    // bass: A1 A1 C2 G1 (one note per second), 3 harmonics
    std::vector<BassNote> notes;
    const double pattern[4] = { 55.0, 55.0, 65.406, 48.999 };
    for (int i = 0; i < (int) secs; ++i)
        notes.push_back ({ pattern[i % 4], (double) i, (double) i + 1.0 });
    const double bassAmps[3] = { 0.35, 0.12, 0.05 };
    Buf bass ((size_t) (secs * fs), 0.0f);
    {
        double ph[3] = {};
        for (size_t n = 0; n < bass.size(); ++n)
        {
            const double tt = (double) n / fs;
            const int ni = std::min ((int) notes.size() - 1, (int) tt);
            const BassNote& bn = notes[(size_t) ni];
            const double fadeIn = std::min (1.0, (tt - bn.start) / 0.005), fadeOut = std::min (1.0, (bn.end - tt) / 0.005);
            const double env = std::max (0.0, std::min (fadeIn, fadeOut));
            double s = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                ph[k] += 2.0 * kPi * bn.f * (k + 1) / fs;
                s += bassAmps[k] * std::sin (ph[k]);
            }
            bass[n] = (float) (env * s);
        }
    }

    // oracle: the same Advisor fed with the exact synthetic partials at the engine's frame times
    fr::Advisor oracle;
    {
        fr::FrictionEngine probeEng;
        probeEng.prepare (fs, 512, 1);
        const int N = probeEng.mixFftSize(), H = N / 4;
        for (std::int64_t end = H; end <= (std::int64_t) kick.size(); end += H)
        {
            if (end < N) continue;
            const double tc = (double) (end - N / 2) / fs;
            fr::AdvisorFrame f;
            f.hopSeconds = (float) ((double) H / fs);
            // kick partials: the most recent hit and the decaying previous one
            for (int hi = (int) hits.size() - 1; hi >= 0 && f.nT < 2; --hi)
            {
                const double dt = tc - hits[(size_t) hi];
                if (dt < 0.0 || dt > 0.6) continue;
                f.tf[f.nT] = (float) kickFreqAt (kp, dt);
                f.ta[f.nT] = (float) kickAmpAt (kp, dt);
                f.tLow[f.nT] = 1;
                ++f.nT;
            }
            const int ni = std::min ((int) notes.size() - 1, (int) tc);
            for (int k = 0; k < 3; ++k)
            {
                f.rf[f.nR] = (float) (notes[(size_t) ni].f * (k + 1));
                f.ra[f.nR] = (float) bassAmps[k];
                f.rLow[f.nR] = 1;
                ++f.nR;
            }
            oracle.consume (f);
        }
    }
    const auto truth = oracle.finish();

    fr::Params p;
    int frames = 0;
    const auto res = learnFromAudio (fs, kick, bass, p, &frames);
    const bool okLow = res.lowValid && std::abs (res.lowBestCents - truth.lowBestCents) <= 5.0f;
    ctx.add ("Advisor", "Kick vs bass: recovers the optimal kick offset", okLow,
             fmt ("advisor %+.0f cents (low-band curve), oracle %+.0f cents; overall curve %+.0f vs oracle %+.0f; expected reduction %.0f %% (oracle %.0f %%)",
                  res.lowBestCents, truth.lowBestCents, res.bestCents, truth.bestCents, res.lowReductionPct, truth.lowReductionPct),
             "within +-5 cents of the oracle (same model, exact partials)");

    std::string noteList;
    for (int i = 0; i < res.numNotes; ++i)
    {
        char nm[8];
        fr::midiNoteName (res.notes[i].midi, nm);
        noteList += fmt ("%s (%.1f Hz, %+.0f c, %.0f %%) ", nm, res.notes[i].hz, res.notes[i].centsDev, res.notes[i].share * 100.0f);
    }
    char newName[8];
    fr::midiNoteName ((int) std::lround (fr::hzToMidi (std::max (1.0f, res.kickNewF0))), newName);
    const bool kickOk = res.kickDetected && std::abs (1200.0 * std::log2 (res.kickF0 / kp.fEnd)) < 10.0 && res.numNotes >= 3;
    ctx.add ("Advisor", "Kick vs bass: report (kick f0, clashing notes, new tuning)", kickOk,
             fmt ("kick detected=%s, %d hits, fundamental %.1f Hz (true %.1f); clashes with %s; best-compromise kick %.1f Hz (%s)",
                  res.kickDetected ? "yes" : "no", res.kickHits, res.kickF0, kp.fEnd, noteList.c_str(), res.kickNewF0, newName),
             "kick detected, f0 within 10 cents of 52 Hz, >= 3 bass notes listed");

    ctx.wav ("advisor_kick_bass_mix.wav", { mixOf (kick, bass) }, fs);
    ctx.notes.push_back (fmt ("Advisor oracle (kick vs bass): best low-band offset %+.0f cents, %.0f %% reduction; musical unison with A1 would be %+.1f cents.",
                              truth.lowBestCents, truth.lowReductionPct, 1200.0 * std::log2 (55.0 / 52.0)));
}
