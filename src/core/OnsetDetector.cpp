#include "OnsetDetector.h"

namespace fr
{

void OnsetDetector::prepare (double sampleRate)
{
    fs = sampleRate;
    frameLen = std::max (8, (int) std::lround (fs * 0.001));
    // slow average time constant 40 ms, updated once per 1 ms frame
    slowCoeff = 1.0 - std::exp (-1.0 / 40.0);
    reset();
}

void OnsetDetector::reset() noexcept
{
    pos = 0;
    accFull = accDiff = accDiff2 = 0.0;
    slowFull = slowDiff = slowDiff2 = 0.0;
    prevX = prevX2 = 0.0f;
    count = 0;
    frameStart = 0;
    last = INT64_MIN / 2;
    framesSeen = 0;
    head = n = 0;
}

bool OnsetDetector::push (float x) noexcept
{
    const double d = (double) x - (double) prevX;
    const double d2 = (double) x - 2.0 * (double) prevX + (double) prevX2;
    prevX2 = prevX;
    prevX = x;
    accFull += (double) x * x;
    accDiff += d * d;
    accDiff2 += d2 * d2;
    ++count;

    if (++pos < frameLen)
        return false;

    const double eFull = accFull / frameLen + 1.0e-20;
    const double eDiff = accDiff / frameLen + 1.0e-20;
    const double eDiff2 = accDiff2 / frameLen + 1.0e-20;
    accFull = accDiff = accDiff2 = 0.0;
    pos = 0;
    const std::int64_t thisFrameStart = frameStart;
    frameStart = count;

    bool onset = false;
    if (framesSeen >= 3)
    {
        const double fullDb = 10.0 * std::log10 (eFull);
        const double jumpFull = fullDb - 10.0 * std::log10 (slowFull + 1.0e-20);
        const double jumpDiff = 10.0 * std::log10 (eDiff) - 10.0 * std::log10 (slowDiff + 1.0e-20);
        const double jumpDiff2 = 10.0 * std::log10 (eDiff2) - 10.0 * std::log10 (slowDiff2 + 1.0e-20);
        // full-scale sine has mean square 0.5 -> -3 dB
        const bool loudEnough = fullDb > (double) floorDb;
        if (loudEnough && (jumpFull > jumpDb || jumpDiff > jumpDb + 3.0 || jumpDiff2 > jumpDb + 6.0)
            && (double) (thisFrameStart - last) > refractorySec * fs)
        {
            onset = true;
            last = thisFrameStart;
            hist[head] = thisFrameStart;
            head = (head + 1) % kHist;
            n = std::min (n + 1, kHist);
        }
    }
    ++framesSeen;

    slowFull += slowCoeff * (eFull - slowFull);
    slowDiff += slowCoeff * (eDiff - slowDiff);
    slowDiff2 += slowCoeff * (eDiff2 - slowDiff2);
    if (slowFull < 1.0e-30) slowFull = 0.0;
    if (slowDiff < 1.0e-30) slowDiff = 0.0;
    if (slowDiff2 < 1.0e-30) slowDiff2 = 0.0;
    return onset;
}

std::int64_t OnsetDetector::lastOnset() const noexcept
{
    return last;
}

std::int64_t OnsetDetector::nextOnsetAfter (std::int64_t t) const noexcept
{
    std::int64_t best = INT64_MAX;
    for (int i = 0; i < n; ++i)
    {
        const std::int64_t o = hist[(head - 1 - i + kHist) % kHist];
        if (o > t)
            best = std::min (best, o);
        else
            break; // history is in increasing order
    }
    return best;
}

std::int64_t OnsetDetector::lastOnsetAtOrBefore (std::int64_t t) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const std::int64_t o = hist[(head - 1 - i + kHist) % kHist];
        if (o <= t)
            return o;
    }
    return INT64_MIN / 2;
}

bool OnsetDetector::anyOnsetIn (std::int64_t from, std::int64_t to) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const std::int64_t o = hist[(head - 1 - i + kHist) % kHist];
        if (o < from)
            break;
        if (o <= to)
            return true;
    }
    return false;
}

} // namespace fr
