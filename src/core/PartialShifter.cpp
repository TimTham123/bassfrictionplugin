#include "PartialShifter.h"

namespace fr
{

void PartialShifter::prepare (int fftSize, int hopSize, double sampleRate, int maxSlots)
{
    n = fftSize;
    hop = hopSize;
    bins = n / 2 + 1;
    fs = sampleRate;
    binHz = fs / n;
    phaseAcc.assign ((size_t) maxSlots, 0.0);
    active.assign ((size_t) maxSlots, 0);
    for (auto& s : scratch)
        s.assign ((size_t) bins, cfloat());
    regions.resize ((size_t) maxSlots);
}

void PartialShifter::reset() noexcept
{
    std::fill (phaseAcc.begin(), phaseAcc.end(), 0.0);
    std::fill (active.begin(), active.end(), (std::uint8_t) 0);
}

void PartialShifter::resetSlot (int slot) noexcept
{
    if (slot >= 0 && slot < (int) phaseAcc.size())
    {
        phaseAcc[(size_t) slot] = 0.0;
        active[(size_t) slot] = 0;
    }
}

bool PartialShifter::slotHasPhase (int slot) const noexcept
{
    return slot >= 0 && slot < (int) phaseAcc.size() && active[(size_t) slot] != 0;
}

float PartialShifter::slotPhase (int slot) const noexcept
{
    return slot >= 0 && slot < (int) phaseAcc.size() ? (float) phaseAcc[(size_t) slot] : 0.0f;
}

void PartialShifter::process (cfloat* const* spectra, int numCh, const float* mag, const ShiftRequest* req, int nReq,
                              bool transient) noexcept
{
    if (transient)
    {
        // pass the transient frame untouched and restart all phase accumulators from 0
        for (int i = 0; i < nReq; ++i)
            resetSlot (req[i].slot);
        return;
    }

    int nRegions = 0;
    const int maxR = (int) regions.size();
    for (int i = 0; i < nReq && nRegions < maxR; ++i)
    {
        const ShiftRequest& r = req[i];
        if (r.slot < 0 || r.slot >= (int) phaseAcc.size())
            continue;
        const int kp = r.peakBin;
        if (kp < 3 || kp > bins - 4)
            continue;

        double& ph = phaseAcc[(size_t) r.slot];
        ph += kTwoPi * (double) r.shiftHz * (double) hop / fs;
        ph = std::remainder (ph, kTwoPi);
        active[(size_t) r.slot] = 1;

        // region of influence: walk down to the valleys on both sides
        int a = kp, b = kp;
        while (a > 2 && kp - a < maxRegionHalfWidth && mag[a - 1] < mag[a])
            --a;
        while (b < bins - 3 && b - kp < maxRegionHalfWidth && mag[b + 1] < mag[b])
            ++b;

        Region& g = regions[(size_t) nRegions++];
        g.a = a;
        g.b = b;
        g.shiftBins = (int) std::lround ((double) r.shiftHz / binHz);
        g.rot = cfloat ((float) std::cos (ph), (float) std::sin (ph));
    }

    if (nRegions == 0)
        return;

    for (int c = 0; c < std::min (numCh, kMaxChannels); ++c)
    {
        cfloat* X = spectra[c];
        cfloat* S = scratch[c].data();
        std::copy (X, X + bins, S);

        for (int i = 0; i < nRegions; ++i)
            for (int k = regions[(size_t) i].a; k <= regions[(size_t) i].b; ++k)
                X[k] = cfloat();

        for (int i = 0; i < nRegions; ++i)
        {
            const Region& g = regions[(size_t) i];
            for (int k = g.a; k <= g.b; ++k)
            {
                const int d = k + g.shiftBins;
                if (d >= 1 && d < bins - 1)
                    X[d] += S[k] * g.rot;
            }
        }
    }
}

} // namespace fr
