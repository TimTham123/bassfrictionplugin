#include "BellBank.h"

namespace fr
{

void BellBank::prepare (double sampleRate, int numSlots, int numChannels)
{
    fs = sampleRate;
    nCh = std::clamp (numChannels, 1, kMaxChannels);
    slots.assign ((size_t) numSlots, Slot());
    alpha = onePoleCoeff (0.003, fs / kControlTick);
    reset();
}

void BellBank::reset() noexcept
{
    for (auto& s : slots)
    {
        s = Slot();
    }
    numActive = 0;
}

void BellBank::setTarget (int slot, float freqHz, float gainDb, float bwHz) noexcept
{
    if (slot < 0 || slot >= (int) slots.size())
        return;
    Slot& s = slots[(size_t) slot];
    const float nyq = (float) (0.45 * fs);
    s.tgtF = clampf (freqHz, 10.0f, nyq);
    s.tgtG = clampf (gainDb, -48.0f, 24.0f);
    s.tgtBw = clampf (bwHz, 1.0f, nyq * 0.5f);
    if (! s.running && s.tgtG != 0.0f)
    {
        // start from the target frequency with 0 dB so the cut fades in at the right place
        s.curF = s.tgtF;
        s.curBw = s.tgtBw;
        s.curG = 0.0f;
        for (int c = 0; c < kMaxChannels; ++c)
            s.ic1[c] = s.ic2[c] = 0.0;
    }
}

void BellBank::snap (int slot) noexcept
{
    if (slot < 0 || slot >= (int) slots.size())
        return;
    Slot& s = slots[(size_t) slot];
    s.curF = s.tgtF;
    s.curG = s.tgtG;
    s.curBw = s.tgtBw;
}

void BellBank::computeCoeffs (Slot& s) noexcept
{
    const double A = std::pow (10.0, (double) s.curG / 40.0);
    const double q = std::max (0.05, (double) s.curF / std::max (0.5, (double) s.curBw));
    const double g = std::tan (kPi * std::min ((double) s.curF, 0.49 * fs) / fs);
    const double k = 1.0 / (q * A);
    s.a1 = 1.0 / (1.0 + g * (g + k));
    s.a2 = g * s.a1;
    s.a3 = g * s.a2;
    s.m1 = k * (A * A - 1.0);
}

void BellBank::tick() noexcept
{
    numActive = 0;
    for (auto& s : slots)
    {
        const bool settledIdle = s.tgtG == 0.0f && std::abs (s.curG) < 1.0e-4f;
        if (settledIdle)
        {
            s.curG = 0.0f;
            s.running = false;
            continue;
        }
        s.running = true;
        s.curG += alpha * (s.tgtG - s.curG);
        s.curBw += alpha * (s.tgtBw - s.curBw);
        // glide frequency in the log domain
        s.curF *= std::pow (s.tgtF / std::max (1.0f, s.curF), alpha);
        computeCoeffs (s);
        ++numActive;
    }
}

void BellBank::process (float* const* io, int numCh, int start, int numSamples) noexcept
{
    if (numActive == 0)
        return;
    const int ch = std::min (numCh, nCh);
    for (auto& s : slots)
    {
        if (! s.running)
            continue;
        // TPT SVF is well conditioned in single precision except for very low f / fs; use double there
        const bool useDouble = s.curF < 150.0f || s.curF < (float) (fs * 0.002);
        if (useDouble)
        {
            for (int c = 0; c < ch; ++c)
            {
                float* x = io[c] + start;
                double ic1 = s.ic1[c], ic2 = s.ic2[c];
                for (int i = 0; i < numSamples; ++i)
                {
                    const double v0 = x[i];
                    const double v3 = v0 - ic2;
                    const double v1 = s.a1 * ic1 + s.a2 * v3;
                    const double v2 = ic2 + s.a2 * ic1 + s.a3 * v3;
                    ic1 = 2.0 * v1 - ic1;
                    ic2 = 2.0 * v2 - ic2;
                    x[i] = (float) (v0 + s.m1 * v1);
                }
                s.ic1[c] = std::abs (ic1) < 1.0e-25 ? 0.0 : ic1;
                s.ic2[c] = std::abs (ic2) < 1.0e-25 ? 0.0 : ic2;
            }
        }
        else
        {
            const float a1 = (float) s.a1, a2 = (float) s.a2, a3 = (float) s.a3, m1 = (float) s.m1;
            if (ch == 2)
            {
                // both channels in one loop (independent recurrences -> instruction-level parallelism)
                float* xl = io[0] + start;
                float* xr = io[1] + start;
                float l1 = (float) s.ic1[0], l2 = (float) s.ic2[0], r1 = (float) s.ic1[1], r2 = (float) s.ic2[1];
                for (int i = 0; i < numSamples; ++i)
                {
                    const float vl0 = xl[i], vr0 = xr[i];
                    const float vl3 = vl0 - l2, vr3 = vr0 - r2;
                    const float vl1 = a1 * l1 + a2 * vl3, vr1 = a1 * r1 + a2 * vr3;
                    const float vl2 = l2 + a2 * l1 + a3 * vl3, vr2 = r2 + a2 * r1 + a3 * vr3;
                    l1 = 2.0f * vl1 - l1;
                    r1 = 2.0f * vr1 - r1;
                    l2 = 2.0f * vl2 - l2;
                    r2 = 2.0f * vr2 - r2;
                    xl[i] = vl0 + m1 * vl1;
                    xr[i] = vr0 + m1 * vr1;
                }
                s.ic1[0] = std::abs (l1) < 1.0e-25f ? 0.0 : (double) l1;
                s.ic2[0] = std::abs (l2) < 1.0e-25f ? 0.0 : (double) l2;
                s.ic1[1] = std::abs (r1) < 1.0e-25f ? 0.0 : (double) r1;
                s.ic2[1] = std::abs (r2) < 1.0e-25f ? 0.0 : (double) r2;
            }
            else
            {
                float* x = io[0] + start;
                float ic1 = (float) s.ic1[0], ic2 = (float) s.ic2[0];
                for (int i = 0; i < numSamples; ++i)
                {
                    const float v0 = x[i];
                    const float v3 = v0 - ic2;
                    const float v1 = a1 * ic1 + a2 * v3;
                    const float v2 = ic2 + a2 * ic1 + a3 * v3;
                    ic1 = 2.0f * v1 - ic1;
                    ic2 = 2.0f * v2 - ic2;
                    x[i] = v0 + m1 * v1;
                }
                s.ic1[0] = std::abs (ic1) < 1.0e-25f ? 0.0 : (double) ic1;
                s.ic2[0] = std::abs (ic2) < 1.0e-25f ? 0.0 : (double) ic2;
            }
        }
    }
}

} // namespace fr
