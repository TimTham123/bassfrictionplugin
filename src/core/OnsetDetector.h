// Friction - time-domain onset detector (1 ms energy frames)
#pragma once

#include "Common.h"

namespace fr
{

/**
    Detects energy onsets on a mono signal with ~1 ms precision.
    An onset is declared when the frame energy (broadband or first-difference / HF-weighted)
    jumps more than `jumpDb` above a slow running average and exceeds `floorDb`.
    Onset times are full-rate input sample indices (start of the triggering frame).
*/
class OnsetDetector
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    /** Push one sample; returns true if an onset was declared on this sample. */
    bool push (float x) noexcept;

    std::int64_t lastOnset() const noexcept;
    /** Earliest onset strictly after t, or INT64_MAX. */
    std::int64_t nextOnsetAfter (std::int64_t t) const noexcept;
    /** Latest onset at or before t, or INT64_MIN / 2. */
    std::int64_t lastOnsetAtOrBefore (std::int64_t t) const noexcept;
    bool anyOnsetIn (std::int64_t from, std::int64_t to) const noexcept;

    float jumpDb = 9.0f;
    float floorDb = -55.0f;
    double refractorySec = 0.05;

private:
    double fs = 48000.0;
    int frameLen = 48;
    int pos = 0;
    double accFull = 0.0, accDiff = 0.0;
    double slowFull = 0.0, slowDiff = 0.0;
    double slowCoeff = 0.03;
    float prevX = 0.0f;
    std::int64_t count = 0;
    std::int64_t frameStart = 0;
    std::int64_t last = INT64_MIN / 2;
    int framesSeen = 0;

    static constexpr int kHist = 64;
    std::int64_t hist[kHist] {};
    int head = 0, n = 0;
};

} // namespace fr
