#include "SpectralAnalyzer.h"

namespace fr
{

namespace
{
    constexpr int kCandCapacity = kMaxPeaks * 2;
}

float SpectralAnalyzer::hannKernel (float d) noexcept
{
    const float ad = std::abs (d);
    if (ad < 1.0e-5f)
        return 1.0f;
    const float den = 1.0f - ad * ad;
    if (std::abs (den) < 1.0e-4f)
        return 0.5f;
    const float pd = (float) kPi * ad;
    return std::abs ((std::sin (pd) / pd) / den);
}

void SpectralAnalyzer::prepare (int fftSize, double sampleRate)
{
    n = fftSize;
    fs = sampleRate;
    fft.prepare (n);

    win.resize ((size_t) n);
    dwin.resize ((size_t) n);
    tmp.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double a = kTwoPi * i / n;
        win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (a));
        dwin[(size_t) i] = (float) (kPi / n * std::sin (a));
    }

    const int bins = n / 2 + 1;
    X.assign ((size_t) bins, cfloat());
    Xd.assign ((size_t) bins, cfloat());
    mag.assign ((size_t) bins, 0.0f);
    magDb.assign ((size_t) bins, -200.0f);
    localMean.assign ((size_t) bins, -200.0f);
    prefix.assign ((size_t) bins + 1, 0.0);
    halfWidth.resize ((size_t) bins);
    erbOfBin.resize ((size_t) bins);
    const double bhz = fs / n;
    for (int k = 0; k < bins; ++k)
    {
        const double f = k * bhz;
        halfWidth[(size_t) k] = std::clamp ((int) std::lround (0.5 * erbHz (f) / bhz), 2, 64);
        erbOfBin[(size_t) k] = (float) erbRate (f);
    }
    nCand = nOut = 0;
}

void SpectralAnalyzer::transformOnly (const float* frame) noexcept
{
    for (int i = 0; i < n; ++i)
        tmp[(size_t) i] = frame[i] * win[(size_t) i];
    fft.forward (tmp.data(), X.data());
}

float SpectralAnalyzer::reassignedOffset (int k) const noexcept
{
    const cfloat x = X[(size_t) k];
    const float p = std::norm (x);
    if (p <= 1.0e-30f)
        return 1.0e6f;
    const cfloat q = Xd[(size_t) k] * std::conj (x);
    return -(q.imag() / p) * (float) (n / kTwoPi);
}

int SpectralAnalyzer::analyze (const float* frame, float minHz, float maxHz) noexcept
{
    transformOnly (frame);
    for (int i = 0; i < n; ++i)
        tmp[(size_t) i] = frame[i] * dwin[(size_t) i];
    fft.forward (tmp.data(), Xd.data());

    const int bins = n / 2 + 1;
    const float ampScale = 4.0f / (float) n;
    prefix[0] = 0.0;
    for (int k = 0; k < bins; ++k)
    {
        const float a = std::abs (X[(size_t) k]) * ampScale;
        mag[(size_t) k] = a;
        const float db = a > 1.0e-10f ? 20.0f * std::log10 (a) : -200.0f;
        magDb[(size_t) k] = db;
        prefix[(size_t) k + 1] = prefix[(size_t) k] + db;
    }

    const double bhz = fs / n;
    const int kLo = std::max (3, (int) std::floor (minHz / bhz));
    const int kHi = std::min (bins - 4, (int) std::ceil (maxHz / bhz));

    float frameMax = -200.0f;
    for (int k = kLo; k <= kHi; ++k)
        frameMax = std::max (frameMax, magDb[(size_t) k]);

    nCand = 0;
    nOut = 0;
    if (kHi <= kLo || frameMax < absFloorDb)
        return 0;

    const float relFloor = frameMax - relFloorDb;

    for (int k = kLo; k <= kHi; ++k)
    {
        const float d0 = magDb[(size_t) k];
        if (d0 < absFloorDb || d0 < relFloor)
            continue;
        if (! (d0 > magDb[(size_t) k - 1] && d0 >= magDb[(size_t) k + 1]
               && d0 > magDb[(size_t) k - 2] && d0 >= magDb[(size_t) k + 2]))
            continue;

        // region of influence: walk down to the valleys
        int lo = k, hi = k;
        while (lo > 1 && k - lo < maxRegionHalfWidth && mag[(size_t) lo - 1] < mag[(size_t) lo])
            --lo;
        while (hi < bins - 2 && hi - k < maxRegionHalfWidth && mag[(size_t) hi + 1] < mag[(size_t) hi])
            ++hi;

        // prominence over the surrounding floor (log mean of +-ERB/2, at least 4 bins, around the region,
        // excluding the region itself so that smeared peaks are not compared with themselves)
        const int w = std::max (4, halfWidth[(size_t) k]);
        const int a = std::max (0, lo - w), b = std::min (bins - 1, hi + w);
        const double outer = prefix[(size_t) b + 1] - prefix[(size_t) a];
        const double inner = prefix[(size_t) hi + 1] - prefix[(size_t) lo];
        const int count = (b - a + 1) - (hi - lo + 1);
        const float lm = count > 0 ? (float) ((outer - inner) / (double) count) : -200.0f;
        localMean[(size_t) k] = lm;
        if (d0 < lm + localMeanMarginDb)
            continue;

        // stationarity: reassigned frequencies of bins k-1, k, k+1 must agree (sinusoid, not noise).
        // Frequency-modulated partials fail this but are kept when they are strongly prominent.
        const float prominence = d0 - lm;
        const float off = reassignedOffset (k);
        const float cL = (float) (k - 1) + reassignedOffset (k - 1);
        const float cR = (float) (k + 1) + reassignedOffset (k + 1);
        const float c = (float) k + off;
        const bool stationary = std::abs (off) <= 0.75f
                                && std::abs (cL - c) <= consistencyBins && std::abs (cR - c) <= consistencyBins;
        if (! (stationary || prominence >= strongMarginDb))
            continue;

        SpectralPeak p;
        p.k = k;
        p.lo = lo;
        p.hi = hi;
        p.stationary = stationary;
        if (stationary)
        {
            p.bin = c;
            p.amp = mag[(size_t) k] / hannKernel (off);
        }
        else
        {
            // smeared (frequency-modulated) partial: energy-weighted reassigned frequency and
            // amplitude from the energy of its region (Parseval; Hann sum of squares = 3N/8)
            double eSum = 0.0, fSum = 0.0;
            for (int j = lo; j <= hi; ++j)
            {
                const double e = (double) std::norm (X[(size_t) j]);
                const float oj = std::clamp (reassignedOffset (j), -1.5f, 1.5f);
                eSum += e;
                fSum += e * ((double) j + oj);
            }
            p.bin = eSum > 0.0 ? (float) (fSum / eSum) : (float) k;
            p.amp = (float) std::sqrt (eSum * 32.0 / (3.0 * (double) n * (double) n));
        }
        p.freq = (float) (p.bin * bhz);
        p.db = gainToDb (p.amp);
        if (p.freq < minHz || p.freq > maxHz)
            continue;

        if (nCand < kCandCapacity)
        {
            cand[nCand++] = p;
        }
        else
        {
            int weakest = 0;
            for (int i = 1; i < nCand; ++i)
                if (cand[i].amp < cand[weakest].amp)
                    weakest = i;
            if (cand[weakest].amp < p.amp)
                cand[weakest] = p;
        }
    }

    // strongest first for the masking test
    std::sort (cand, cand + nCand, [] (const SpectralPeak& x, const SpectralPeak& y) { return x.amp > y.amp; });

    for (int i = 0; i < nCand && nOut < kMaxPeaks; ++i)
    {
        const SpectralPeak& p = cand[i];
        const float ei = (float) erbRate (p.freq);
        bool masked = false;
        for (int j = 0; j < nOut; ++j)
        {
            const SpectralPeak& q = peakOut[j];
            const float ej = (float) erbRate (q.freq);
            const float dist = ej - ei;
            const float thr = dist > 0.0f ? q.db - maskOffsetDb - maskSlopeLowDbPerErb * dist
                                          : q.db - maskOffsetDb + maskSlopeHighDbPerErb * dist;
            if (p.db < thr)
            {
                masked = true;
                break;
            }
        }
        if (! masked)
            peakOut[nOut++] = p;
    }

    std::sort (peakOut, peakOut + nOut, [] (const SpectralPeak& x, const SpectralPeak& y) { return x.freq < y.freq; });
    return nOut;
}

// ------------------------------------------------------------------ PeakTracker

void PeakTracker::prepare (int maxSlots)
{
    slots.assign ((size_t) maxSlots, Slot());
    cands.resize (4096);
    nextId = 1;
}

void PeakTracker::reset() noexcept
{
    for (auto& s : slots)
        s = Slot();
    nextId = 1;
}

void PeakTracker::update (const float* freq, const float* amp, int n, int* outSlot, double binHz) noexcept
{
    const int ns = (int) slots.size();
    for (auto& s : slots)
    {
        s.born = false;
        s.matched = false;
    }
    for (int p = 0; p < n; ++p)
        outSlot[p] = -1;

    int nc = 0;
    const int capacity = (int) cands.size();
    for (int s = 0; s < ns && nc < capacity; ++s)
    {
        const Slot& sl = slots[(size_t) s];
        if (! sl.used)
            continue;
        const float tol = std::max (relTolerance, (float) (binTolerance * binHz) / std::max (1.0f, sl.freq))
                          * (sl.missed > 0 ? 1.5f : 1.0f);
        // peaks arrive sorted by frequency: only scan the tolerance window
        const float fLo = sl.freq * (1.0f - tol);
        int p = (int) (std::lower_bound (freq, freq + n, fLo) - freq);
        for (; p < n && nc < capacity; ++p)
        {
            const float rel = std::abs (freq[p] - sl.freq) / std::max (1.0f, sl.freq);
            if (freq[p] > sl.freq && rel >= tol)
                break;
            if (rel < tol)
                cands[(size_t) nc++] = { rel / tol, s, p };
        }
    }

    std::sort (cands.begin(), cands.begin() + nc, [] (const Cand& a, const Cand& b) { return a.cost < b.cost; });

    for (int c = 0; c < nc; ++c)
    {
        const Cand& cd = cands[(size_t) c];
        Slot& sl = slots[(size_t) cd.slot];
        if (sl.matched || outSlot[cd.peak] >= 0)
            continue;
        sl.matched = true;
        sl.freq = freq[cd.peak];
        sl.amp = amp[cd.peak];
        sl.missed = 0;
        ++sl.age;
        outSlot[cd.peak] = cd.slot;
    }

    for (int p = 0; p < n; ++p)
    {
        if (outSlot[p] >= 0)
            continue;
        for (int s = 0; s < ns; ++s)
        {
            Slot& sl = slots[(size_t) s];
            if (sl.used)
                continue;
            sl = Slot();
            sl.used = true;
            sl.born = true;
            sl.matched = true;
            sl.id = nextId++;
            sl.freq = freq[p];
            sl.amp = amp[p];
            outSlot[p] = s;
            break;
        }
    }

    for (auto& sl : slots)
    {
        if (sl.used && ! sl.matched)
        {
            if (++sl.missed > maxMissed)
                sl.used = false;
        }
    }
}

} // namespace fr
