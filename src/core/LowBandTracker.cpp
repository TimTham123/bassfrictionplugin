#include "LowBandTracker.h"

#include <complex>
#include <cstdlib>

namespace fr
{

void BiquadD::setLowpass (double fc, double q, double fs) noexcept
{
    const double w0 = kTwoPi * fc / fs;
    const double c = std::cos (w0), s = std::sin (w0);
    const double alpha = s / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = (1.0 - c) * 0.5 / a0;
    b1 = (1.0 - c) / a0;
    b2 = b0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
}

void LowBandTracker::prepare (double sampleRate)
{
    fs = sampleRate;
    D = std::max (1, (int) std::lround (fs / 4000.0));
    fsd = fs / D;
    W = (int) std::lround (0.020 * fsd);
    tauMax = (int) std::ceil (fsd / 27.5);
    tauMin = std::max (2, (int) std::floor (fsd / 420.0));
    hopDec = 16;

    static const double qs[4] = { 2.5629154477415, 0.9000338715825, 0.6013448869351, 0.5097955791041 };
    const double fc = std::min (900.0, 0.4 * fsd);
    for (int i = 0; i < 4; ++i)
        lpf[i].setLowpass (fc, qs[i], fs);

    const int fullSize = nextPow2 ((W + tauMax + 8) * D + 256);
    full.assign ((size_t) fullSize, 0.0f);
    fullMask = fullSize - 1;

    const int decSize = nextPow2 (3 * tauMax + W + 64);
    dec.assign ((size_t) decSize, 0.0f);
    decMask = decSize - 1;

    frame.assign ((size_t) (W + tauMax + 2), 0.0f);
    hwin.assign ((size_t) (3 * tauMax + 16), 0.0f);
    dfn.assign ((size_t) (tauMax + 2), 0.0f);
    cmnd.assign ((size_t) (tauMax + 2), 1.0f);
    reset();
}

void LowBandTracker::reset() noexcept
{
    for (auto& b : lpf)
        b.reset();
    std::fill (full.begin(), full.end(), 0.0f);
    std::fill (dec.begin(), dec.end(), 0.0f);
    fullCount = decCount = 0;
    phase = hopCounter = 0;
    rawCount = 0;
    histHead = histCount = 0;
    for (auto& h : hist)
        h = LfEstimate();
}

bool LowBandTracker::push (float xIn) noexcept
{
    // no DC blocker: YIN's difference function is invariant to DC, and a high-pass would add an
    // onset transient that delays lock-in
    double y = (double) xIn;
    for (auto& b : lpf)
        y = b.process (y);
    if (std::abs (y) < 1.0e-20)
        y = 0.0;

    full[(size_t) (fullCount & fullMask)] = (float) y;
    ++fullCount;

    if (++phase >= D)
    {
        phase = 0;
        dec[(size_t) (decCount & decMask)] = (float) y;
        ++decCount;
        if (++hopCounter >= hopDec)
        {
            hopCounter = 0;
            if (decCount >= (std::int64_t) (W + tauMax + 2))
            {
                analyse();
                return true;
            }
        }
    }
    return false;
}

double LowBandTracker::fullDiff (std::int64_t start, int len, int lag, int stride) const noexcept
{
    double acc = 0.0;
    for (int n = 0; n < len; n += stride)
    {
        const double d = (double) full[(size_t) ((start + n) & fullMask)] - (double) full[(size_t) ((start + n - lag) & fullMask)];
        acc += d * d;
    }
    return acc;
}

void LowBandTracker::analyse() noexcept
{
    const int span = W + tauMax;
    const std::int64_t s = decCount - span;
    double sum = 0.0, energy = 0.0;
    for (int i = 0; i < span; ++i)
    {
        const float v = dec[(size_t) ((s + i) & decMask)];
        frame[(size_t) i] = v;
        sum += v;
        energy += (double) v * v;
    }
    const double mean = sum / span;
    const float level = (float) std::sqrt (std::max (0.0, energy / span - mean * mean));

    LfEstimate e;
    e.level = level;

    if (level < 1.0e-7f)
    {
        e.time = (std::int64_t) ((s + span / 2 + 1) * D - 1);
        e.voiced = false;
        rawCount = 0;
        hist[histHead] = e;
        histHead = (histHead + 1) % kHist;
        histCount = std::min (histCount + 1, kHist);
        return;
    }

    // YIN difference function and CMNDF. The comparison windows are anchored at the newest samples
    // (latest W samples against the W samples tau earlier), so short periods lock in after W + tau.
    dfn[0] = 0.0f;
    const float* latestW = frame.data() + (span - W);
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        // single precision with 4 partial sums (vectorisable); precision-critical refinement runs in double below
        float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
        const float* b = latestW - tau;
        int n = 0;
        for (; n + 4 <= W; n += 4)
        {
            const float d0 = latestW[n] - b[n], d1 = latestW[n + 1] - b[n + 1];
            const float d2 = latestW[n + 2] - b[n + 2], d3 = latestW[n + 3] - b[n + 3];
            acc0 += d0 * d0; acc1 += d1 * d1; acc2 += d2 * d2; acc3 += d3 * d3;
        }
        for (; n < W; ++n)
        {
            const float d = latestW[n] - b[n];
            acc0 += d * d;
        }
        dfn[(size_t) tau] = (acc0 + acc1) + (acc2 + acc3);
    }
    cmnd[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        running += dfn[(size_t) tau];
        cmnd[(size_t) tau] = running > 0.0 ? (float) (dfn[(size_t) tau] * tau / running) : 1.0f;
    }

    int tauStar = -1;
    for (int tau = tauMin; tau < tauMax; ++tau)
    {
        if (cmnd[(size_t) tau] < yinThreshold)
        {
            while (tau + 1 < tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;
            tauStar = tau;
            break;
        }
    }
    if (tauStar < 0)
    {
        tauStar = tauMin;
        for (int tau = tauMin + 1; tau < tauMax; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) tauStar])
                tauStar = tau;
    }
    e.aperiodicity = cmnd[(size_t) tauStar];

    double tf = tauStar;
    {
        const double a = dfn[(size_t) tauStar - 1], b = dfn[(size_t) tauStar], c = dfn[(size_t) tauStar + 1];
        const double den = a - 2.0 * b + c;
        if (den > 1.0e-20)
            tf += 0.5 * (a - c) / den;
    }

    // full-rate refinement on the same (newest) data
    double tFine = tf * D;
    if (D > 1)
    {
        const int Wf = W * D;
        const int stride = std::max (1, D / 4);
        int centre = (int) std::lround (tFine);
        const std::int64_t sf = fullCount - Wf; // start of the latest Wf full-rate samples

        double v[5] = {};
        int best = 2;
        for (int iter = 0; iter < 4; ++iter)
        {
            for (int i = 0; i < 5; ++i)
                v[i] = fullDiff (sf, Wf, std::max (1, centre - 2 + i), stride);
            best = 0;
            for (int i = 1; i < 5; ++i)
                if (v[i] < v[best])
                    best = i;
            if (best == 0 && centre - 2 > 2) { centre -= 2; continue; }
            if (best == 4) { centre += 2; continue; }
            break;
        }
        best = std::clamp (best, 1, 3);
        const double a = v[best - 1], b = v[best], c = v[best + 1];
        const double den = a - 2.0 * b + c;
        double off = 0.0;
        if (den > 1.0e-30)
            off = std::clamp (0.5 * (a - c) / den, -1.0, 1.0);
        tFine = (double) (centre - 2 + best) + off;
    }

    float f0 = (float) (fs / std::max (1.0, tFine));

    // outlier rejection (keeps fast monotonic glides such as kick drops)
    const bool voicedNow = e.aperiodicity < voicingThreshold && level > levelThreshold
                           && f0 >= 25.0f && f0 <= 440.0f;
    if (voicedNow)
    {
        // rawF0[0] = older, rawF0[1] = newer accepted raw estimate
        float out = f0;
        if (rawCount >= 2)
        {
            const float p1 = rawF0[0], p2 = rawF0[1];
            const bool prevAgree = std::abs (p1 - p2) < 0.03f * p2;
            const bool latestOff = std::abs (f0 - p2) > 0.06f * p2 && std::abs (f0 - p1) > 0.06f * p1;
            if (prevAgree && latestOff)
                out = p2;
        }
        rawF0[0] = rawF0[1];
        rawF0[1] = f0;
        rawCount = std::min (rawCount + 1, 2);
        f0 = out;
    }
    else
    {
        rawCount = 0;
    }

    e.f0 = f0;
    e.voiced = voicedNow;
    const double centreDec = (double) decCount - 0.5 * ((double) W + tf) - 0.5;
    e.time = (std::int64_t) std::llround ((centreDec + 1.0) * D) - 1;

    // harmonic amplitudes, pitch-synchronous Hann over m periods
    if (voicedNow)
    {
        const double Td = tFine / D;
        const int decSize = decMask + 1;
        int m = 3;
        if (m * Td + 8 > decSize - 64)
            m = 2;
        int Lh = std::max (8, (int) std::lround (m * Td));
        std::int64_t start = (std::int64_t) std::llround (centreDec - 0.5 * Lh);
        if (start + Lh > decCount)
            start = decCount - Lh;
        if (start < decCount - decSize + 1)
            start = decCount - decSize + 1;

        const double maxH = std::min ((double) harmLimitHz, 0.45 * fsd);
        // windowed segment computed once (periodic Hann over exactly Lh samples)
        Lh = std::min (Lh, (int) hwin.size());
        double sumW = 0.0;
        const std::complex<double> wrot (std::cos (kTwoPi / Lh), std::sin (kTwoPi / Lh));
        std::complex<double> wz (1.0, 0.0);
        for (int i = 0; i < Lh; ++i)
        {
            const double win = 0.5 - 0.5 * wz.real();
            wz *= wrot;
            hwin[(size_t) i] = (float) ((double) dec[(size_t) ((start + i) & decMask)] * win);
            sumW += win;
        }
        for (int k = 1; k <= kLfHarmonics; ++k)
        {
            const double fk = k * (double) f0;
            if (fk > maxH)
            {
                e.harm[k - 1] = 0.0f;
                continue;
            }
            const double w = kTwoPi * fk / fsd;
            const std::complex<double> rot (std::cos (w), -std::sin (w));
            std::complex<double> z (1.0, 0.0), acc (0.0, 0.0);
            for (int i = 0; i < Lh; ++i)
            {
                acc += (double) hwin[(size_t) i] * z;
                z *= rot;
            }
            e.harm[k - 1] = (float) (2.0 * std::abs (acc) / std::max (1.0e-9, sumW));
        }
    }

    hist[histHead] = e;
    histHead = (histHead + 1) % kHist;
    histCount = std::min (histCount + 1, kHist);
}

bool LowBandTracker::lookup (std::int64_t time, LfEstimate& out, std::int64_t maxDist) const noexcept
{
    if (histCount == 0)
        return false;
    std::int64_t bestDist = INT64_MAX;
    int best = -1;
    for (int i = 0; i < histCount; ++i)
    {
        const int idx = (histHead - 1 - i + kHist) % kHist;
        const std::int64_t d = std::abs (hist[idx].time - time);
        if (d < bestDist)
        {
            bestDist = d;
            best = idx;
        }
        else if (hist[idx].time < time - maxDist)
        {
            break;
        }
    }
    if (best < 0 || bestDist > maxDist)
        return false;
    out = hist[best];
    return true;
}

} // namespace fr
