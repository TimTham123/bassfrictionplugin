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
            if (std::abs (ic1) < 1.0e-25) ic1 = 0.0;
            if (std::abs (ic2) < 1.0e-25) ic2 = 0.0;
            s.ic1[c] = ic1;
            s.ic2[c] = ic2;
        }
    }
}

} // namespace fr
