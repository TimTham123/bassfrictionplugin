#include "Advisor.h"
#include "Roughness.h"

#include <cstdio>

namespace fr
{

Advisor::Advisor()
{
    R.assign ((size_t) kN, 0.0);
    RLF.assign ((size_t) kN, 0.0);
    pow2.resize ((size_t) kN);
    for (int i = 0; i < kN; ++i)
        pow2[(size_t) i] = (float) std::exp2 ((double) (i - kRange) / 1200.0);
    settledHist.assign ((size_t) kF0Bins, 0.0);
    noteW.assign (128, 0.0);
    noteDev.assign (128, 0.0);
    noteHz.assign (128, 0.0);
    reset();
}

void Advisor::reset()
{
    std::fill (R.begin(), R.end(), 0.0);
    std::fill (RLF.begin(), RLF.end(), 0.0);
    std::fill (settledHist.begin(), settledHist.end(), 0.0);
    std::fill (noteW.begin(), noteW.end(), 0.0);
    std::fill (noteDev.begin(), noteDev.end(), 0.0);
    std::fill (noteHz.begin(), noteHz.end(), 0.0);
    frames = 0;
    seconds = 0.0;
    r0Integral = 0.0;
    onsets = 0;
    prevSinceOnset = 1.0e9f;
    earlySum = earlyW = settledSum = settledW = 0.0;
}

void Advisor::consume (const AdvisorFrame& f)
{
    const Roughness& rough = Roughness::get();
    const double dt = std::max (1.0e-4f, f.hopSeconds);
    ++frames;
    seconds += dt;

    float lt[AdvisorFrame::kMax], lr[AdvisorFrame::kMax];
    for (int i = 0; i < f.nT; ++i) lt[i] = Roughness::level (f.ta[i]);
    for (int j = 0; j < f.nR; ++j) lr[j] = Roughness::level (f.ra[j]);

    double lfClash = 0.0;
    for (int i = 0; i < f.nT; ++i)
    {
        const float fi = f.tf[i];
        if (fi <= 0.0f)
            continue;
        for (int j = 0; j < f.nR; ++j)
        {
            const float fj = f.rf[j];
            if (fj <= 0.0f)
                continue;
            const float w = lt[i] * lr[j] * rough.fluct (f.ta[i], f.ra[j]);
            if (w < 1.0e-7f)
                continue;
            const bool lowPair = f.tLow[i] != 0 && f.rLow[j] != 0;

            const float fLowest = std::min (fi * pow2[0], fj);
            const float win = Roughness::windowHz (fLowest);
            const double cLo = 1200.0 * std::log2 (std::max (fj - win, 1.0e-3f) / fi);
            const double cHi = 1200.0 * std::log2 ((fj + win) / fi);
            const int iLo = std::max (-kRange, (int) std::ceil (cLo));
            const int iHi = std::min (kRange, (int) std::floor (cHi));
            for (int c = iLo; c <= iHi; ++c)
            {
                const float fs = fi * pow2[(size_t) (c + kRange)];
                const float x = std::abs (fs - fj) * Roughness::sFactor (std::min (fs, fj));
                if (x >= Roughness::kXMax)
                    continue;
                const double r = (double) (w * rough.zHat (x)) * dt;
                R[(size_t) (c + kRange)] += r;
                if (lowPair)
                    RLF[(size_t) (c + kRange)] += r;
                if (c == 0 && lowPair)
                    lfClash += r;
            }
        }
    }
    r0Integral = R[(size_t) kRange];

    // kick statistics
    if (f.sinceOnset < prevSinceOnset && f.sinceOnset < 0.05f)
        ++onsets;
    prevSinceOnset = f.sinceOnset;

    if (f.tVoiced && f.tF0 > 20.0f && f.tF0 < 400.0f)
    {
        const double w = f.tLfLevel;
        if (f.sinceOnset < 0.03f)
        {
            earlySum += w * std::log2 (f.tF0);
            earlyW += w;
        }
        else if (f.sinceOnset >= 0.04f && f.sinceOnset < 0.8f)
        {
            settledSum += w * std::log2 (f.tF0);
            settledW += w;
            const int b = (int) std::lround (1200.0 * std::log2 (f.tF0 / 20.0));
            if (b >= 0 && b < kF0Bins)
                settledHist[(size_t) b] += w;
        }
    }

    if (lfClash > 0.0 && f.rVoiced && f.rF0 > 20.0f)
    {
        const double m = hzToMidi (f.rF0);
        const int note = (int) std::lround (m);
        if (note >= 0 && note < 128)
        {
            noteW[(size_t) note] += lfClash;
            noteDev[(size_t) note] += lfClash * (m - note) * 100.0;
            noteHz[(size_t) note] += lfClash * f.rF0;
        }
    }
}

namespace
{
    int bestIndex (const std::vector<double>& v)
    {
        const int n = (int) v.size();
        const int mid = n / 2;
        double minV = v[0];
        for (int i = 1; i < n; ++i)
            minV = std::min (minV, v[(size_t) i]);
        // among near-minimal candidates (within 0.5 %) prefer the smallest |offset|
        const double tol = std::max (1.0e-12, std::abs (minV) * 0.005);
        int best = -1;
        for (int i = 0; i < n; ++i)
            if (v[(size_t) i] <= minV + tol)
                if (best < 0 || std::abs (i - mid) < std::abs (best - mid))
                    best = i;
        // refine to the exact local minimum near that candidate
        int b = best;
        while (b > 0 && v[(size_t) b - 1] < v[(size_t) b]) --b;
        while (b < n - 1 && v[(size_t) b + 1] < v[(size_t) b]) ++b;
        return b;
    }
}

AdvisorResult Advisor::finish() const
{
    AdvisorResult res;
    res.frames = frames;
    res.seconds = seconds;
    const double r0 = R[(size_t) kRange];
    res.frictionSeconds = r0;
    if (frames > 0 && r0 > 1.0e-9)
    {
        const int b = bestIndex (R);
        res.valid = true;
        res.bestCents = (float) (b - kRange);
        res.r0 = (float) r0;
        res.rBest = (float) R[(size_t) b];
        res.reductionPct = (float) (100.0 * (r0 - R[(size_t) b]) / r0);
        for (int i = 0; i < kN; ++i)
            res.curve[i] = (float) (R[(size_t) i] / r0);
    }

    const double rl0 = RLF[(size_t) kRange];
    if (frames > 0 && rl0 > 1.0e-9)
    {
        const int b = bestIndex (RLF);
        res.lowValid = true;
        res.lowBestCents = (float) (b - kRange);
        res.lowReductionPct = (float) (100.0 * (rl0 - RLF[(size_t) b]) / rl0);
        for (int i = 0; i < kN; ++i)
            res.lowCurve[i] = (float) (RLF[(size_t) i] / rl0);
    }

    // kick fundamental = weighted median of settled f0
    double total = 0.0;
    for (double w : settledHist)
        total += w;
    if (total > 0.0)
    {
        double acc = 0.0;
        for (int i = 0; i < kF0Bins; ++i)
        {
            acc += settledHist[(size_t) i];
            if (acc >= 0.5 * total)
            {
                res.kickF0 = (float) (20.0 * std::exp2 (i / 1200.0));
                break;
            }
        }
    }
    const bool dropping = earlyW > 0.0 && settledW > 0.0
                          && (earlySum / earlyW) - (settledSum / settledW) > std::log2 (1.1);
    res.kickHits = onsets;
    res.kickDetected = onsets >= 3 && settledW > 0.0 && dropping;
    if (res.kickF0 > 0.0f)
        res.kickNewF0 = res.kickF0 * (float) std::exp2 ((res.lowValid ? res.lowBestCents : 0.0f) / 1200.0);

    // clashing reference notes, ranked by friction caused
    double noteTotal = 0.0;
    for (double w : noteW)
        noteTotal += w;
    if (noteTotal > 0.0)
    {
        bool used[128] = {};
        for (int k = 0; k < 4; ++k)
        {
            int best = -1;
            for (int m = 0; m < 128; ++m)
                if (! used[m] && noteW[(size_t) m] > 0.0 && (best < 0 || noteW[(size_t) m] > noteW[(size_t) best]))
                    best = m;
            if (best < 0)
                break;
            used[best] = true;
            AdvisorResult::Note& nt = res.notes[res.numNotes++];
            nt.midi = best;
            nt.share = (float) (noteW[(size_t) best] / noteTotal);
            nt.centsDev = (float) (noteDev[(size_t) best] / noteW[(size_t) best]);
            nt.hz = (float) (noteHz[(size_t) best] / noteW[(size_t) best]);
        }
    }
    return res;
}

void Advisor::formatOffset (float cents, char* buf, int bufSize)
{
    const int c = (int) std::lround (cents);
    if (std::abs (c) < 100)
    {
        std::snprintf (buf, (size_t) bufSize, "%+d cents", c);
        return;
    }
    const int st = c / 100;
    const int rem = c - st * 100;
    std::snprintf (buf, (size_t) bufSize, "%+d st %+d ct (%+d cents)", st, rem, c);
}

} // namespace fr
