#include "WholeShifter.h"

namespace fr
{

namespace
{
    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        const double hx = 0.5 * x;
        for (int k = 1; k < 64; ++k)
        {
            term *= (hx / k) * (hx / k);
            sum += term;
            if (term < 1.0e-14 * sum)
                break;
        }
        return sum;
    }
}

void WholeShifter::prepare (double sampleRate, int maxLatency, int numChannels)
{
    fs = sampleRate;
    nCh = std::clamp (numChannels, 1, kMaxChannels);
    const int size = nextPow2 (maxLatency + (int) (0.25 * fs) + 4096);
    for (auto& r : ring)
        r.assign ((size_t) size, 0.0f);
    mono.assign ((size_t) size, 0.0f);
    mask = size - 1;

    const float cut[kTables] = { 1.0f, 0.85f, 0.7f, 0.5f };
    tables.assign ((size_t) (kTables * (kPhases + 1) * kTaps), 0.0f);
    const double base = 0.92, beta = 8.6;
    const double i0b = besselI0 (beta);
    for (int tb = 0; tb < kTables; ++tb)
    {
        tableCutoff[tb] = cut[tb];
        const double c = base * cut[tb];
        for (int ph = 0; ph <= kPhases; ++ph)
        {
            const double phi = (double) ph / kPhases;
            float* row = tables.data() + ((size_t) tb * (kPhases + 1) + (size_t) ph) * kTaps;
            double sum = 0.0;
            for (int j = 0; j < kTaps; ++j)
            {
                const double x = (double) (j - (kHalf - 1)) - phi;
                const double u = x / (double) kHalf;
                const double w = std::abs (u) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - u * u)) / i0b;
                const double arg = kPi * c * x;
                const double sinc = std::abs (arg) < 1.0e-12 ? 1.0 : std::sin (arg) / arg;
                const double h = c * sinc * w;
                row[j] = (float) h;
                sum += h;
            }
            for (int j = 0; j < kTaps; ++j)
                row[j] = (float) (row[j] / sum);
        }
    }
    reset();
}

void WholeShifter::reset() noexcept
{
    for (auto& r : ring)
        std::fill (r.begin(), r.end(), 0.0f);
    std::fill (mono.begin(), mono.end(), 0.0f);
    count = 0;
    ratio = 1.0;
    posA = -(double) latency;
    posB = 0.0;
    xfActive = false;
    xfPos = 0;
    splices = 0;
    lastSpliceT = INT64_MIN / 2;
}

void WholeShifter::setLatency (int samples) noexcept
{
    latency = samples;
    realign();
}

void WholeShifter::realign() noexcept
{
    ratio = 1.0;
    xfActive = false;
    xfPos = 0;
    posA = (double) (count - latency);
}

const float* WholeShifter::kernelTableFor (double r) const noexcept
{
    // pick the widest table whose pass band (0.92 * cutoff * Nyquist) still lands below
    // Nyquist after scaling by r, so upward shifts cannot alias
    int tb = kTables - 1;
    for (int i = 0; i < kTables; ++i)
    {
        if (0.92 * (double) tableCutoff[i] * std::max (1.0, r) <= 0.98)
        {
            tb = i;
            break;
        }
    }
    return tables.data() + (size_t) tb * (kPhases + 1) * kTaps;
}

float WholeShifter::ncc (double posARead, std::int64_t candidateStart, int len, int stride) const noexcept
{
    const std::int64_t a0 = (std::int64_t) std::floor (posARead);
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int i = 0; i < len; i += stride)
    {
        const double a = mono[(size_t) ((a0 + i) & mask)];
        const double b = mono[(size_t) ((candidateStart + i) & mask)];
        ab += a * b;
        aa += a * a;
        bb += b * b;
    }
    if (aa <= 1.0e-18 || bb <= 1.0e-18)
        return (aa <= 1.0e-18 && bb <= 1.0e-18) ? 1.0f : 0.0f;
    return (float) (ab / std::sqrt (aa * bb));
}

void WholeShifter::startSplice (double newPos, int len, bool equalGain) noexcept
{
    posB = newPos;
    xfLen = std::max (1, len);
    xfPos = 0;
    xfEqualGain = equalGain;
    xfActive = true;
    ++splices;
}

void WholeShifter::controlTick (std::int64_t t, const OnsetDetector& onsets) noexcept
{
    if (xfActive)
        return;

    const double nominal = (double) (t - latency);
    const double drift = posA - nominal;
    const bool unity = ratio == 1.0;
    if (unity && drift == 0.0)
        return;

    const int xfDefault = std::max (16, (int) std::lround (crossfadeSec * fs));
    const double driftSoft = driftSoftSec * fs;
    const double driftHard = std::min (driftHardSec * fs, 0.45 * latency);
    const std::int64_t newestAvail = t - 1;

    // 1) onset alignment. The splice is started as late as possible before the onset and its target is
    //    pre-compensated for the drift that accumulates until the onset, so the hit is output exactly at
    //    its nominal (latency-compensated) time. No routine splice may start when an onset is near.
    const std::int64_t u = onsets.nextOnsetAfter ((std::int64_t) std::floor (std::min (posA, nominal)));
    if (u != INT64_MAX)
    {
        const double untilA = ((double) u - posA) / std::max (0.25, ratio);   // head A reaches the onset
        const double untilNom = (double) u - nominal;                           // onset's nominal output time
        const double until = std::min (untilA, untilNom);
        const double margin = 0.001 * fs;
        if (until < 2.0 * xfDefault + 4.0 * margin)
        {
            const double targetDrift = -(ratio - 1.0) * untilNom;   // drift at splice time -> 0 at the onset
            if (std::abs (drift - targetDrift) > 0.0003 * fs && until <= (double) xfDefault + 2.0 * margin)
            {
                const int len = (int) (until - margin);
                if (len >= (int) (0.002 * fs))
                {
                    startSplice (nominal + targetDrift, len, false);
                    lastSpliceT = t;
                }
            }
            return; // near an onset: wait for (or skip) the alignment splice
        }
    }

    // 2) hold after an onset
    const std::int64_t lastOn = onsets.lastOnsetAtOrBefore ((std::int64_t) std::floor (posA));
    const bool inHold = (posA - (double) lastOn) < holdSec * fs;

    const bool forced = std::abs (drift) > driftHard;
    bool need = std::abs (drift) > driftSoft;
    if (unity && drift != 0.0)
        need = need || (double) (t - lastSpliceT) > 0.5 * fs;
    if (! need && ! forced)
        return;
    if (inHold && ! forced)
        return;

    if (unity)
    {
        startSplice (nominal, xfDefault, false);
        lastSpliceT = t;
        return;
    }

    // 3) correlation-optimised splice for pitch shifting
    const int len = std::max (32, (int) std::lround (0.010 * fs));
    const int stride = std::max (1, (int) std::lround (fs / 24000.0));
    double lo, hi;
    if (ratio > 1.0) { lo = -driftSoft; hi = 0.0; }
    else             { lo = 0.0; hi = driftSoft; }
    // keep candidates inside the available input
    hi = std::min (hi, (double) (newestAvail - len) - nominal);

    const std::int64_t nom = (std::int64_t) nominal;
    float bestScore = -2.0f;
    std::int64_t bestD = (std::int64_t) (ratio > 1.0 ? lo * 0.5 : hi * 0.5);
    for (std::int64_t d = (std::int64_t) std::ceil (lo); d <= (std::int64_t) std::floor (hi); ++d)
    {
        // small preference for using the full drift range (fewer splices)
        const float pref = 0.02f * (float) (ratio > 1.0 ? (-d / std::max (1.0, driftSoft)) : (d / std::max (1.0, driftSoft)));
        const float score = ncc (posA, nom + d, len, stride) + pref;
        if (score > bestScore)
        {
            bestScore = score;
            bestD = d;
        }
    }
    startSplice ((double) (nom + bestD), xfDefault, bestScore > 0.6f);
    lastSpliceT = t;
}

void WholeShifter::push (const float* x, int numCh, float m) noexcept
{
    const size_t idx = (size_t) (count & mask);
    for (int c = 0; c < nCh; ++c)
        ring[c][idx] = x[std::min (c, numCh - 1)];
    mono[idx] = m;
    ++count;
}

float WholeShifter::readInterp (int ch, double pos, const float* kernel, int) const noexcept
{
    const std::int64_t i0 = (std::int64_t) std::floor (pos);
    const float* r = ring[ch].data();
    float acc = 0.0f;
    for (int j = 0; j < kTaps; ++j)
        acc += kernel[j] * r[(size_t) ((i0 + j - (kHalf - 1)) & mask)];
    return acc;
}

void WholeShifter::render (float* y, int numCh) noexcept
{
    const float* table = kernelTableFor (ratio);
    auto readAt = [&] (double pos, float* out)
    {
        const double fl = std::floor (pos);
        const std::int64_t i0 = (std::int64_t) fl;
        const double phi = pos - fl;
        if (phi == 0.0)
        {
            for (int c = 0; c < nCh; ++c)
                out[c] = ring[c][(size_t) (i0 & mask)];
            return;
        }
        const double idx = phi * kPhases;
        const int p0 = std::min (kPhases - 1, (int) idx);
        const float f = (float) (idx - p0);
        const float* r0 = table + (size_t) p0 * kTaps;
        const float* r1 = r0 + kTaps;
        for (int j = 0; j < kTaps; ++j)
            kernelScratch[j] = r0[j] + f * (r1[j] - r0[j]);
        for (int c = 0; c < nCh; ++c)
            out[c] = readInterp (c, pos, kernelScratch, 0);
    };

    float a[kMaxChannels] = {};
    readAt (posA, a);
    if (xfActive)
    {
        float b[kMaxChannels] = {};
        readAt (posB, b);
        const double tx = ((double) xfPos + 0.5) / (double) xfLen;
        double wA, wB;
        if (xfEqualGain)
        {
            wA = 0.5 * (1.0 + std::cos (kPi * tx));
            wB = 1.0 - wA;
        }
        else
        {
            wA = std::cos (0.5 * kPi * tx);
            wB = std::sin (0.5 * kPi * tx);
        }
        for (int c = 0; c < nCh; ++c)
            a[c] = (float) (wA * a[c] + wB * b[c]);
        posB += ratio;
        posA += ratio;
        if (++xfPos >= xfLen)
        {
            xfActive = false;
            posA = posB;
        }
    }
    else
    {
        posA += ratio;
    }

    for (int c = 0; c < numCh; ++c)
        y[c] = a[std::min (c, nCh - 1)];
}

bool WholeShifter::isIdentity (std::int64_t t) const noexcept
{
    return ratio == 1.0 && ! xfActive && posA == (double) (t - latency);
}

} // namespace fr
