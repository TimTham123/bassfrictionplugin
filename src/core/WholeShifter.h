// Friction - whole-signal micro pitch shifter: resampling delay line with smart splices
#pragma once

#include "Common.h"
#include "OnsetDetector.h"

#include <vector>

namespace fr
{

/**
    Reads the latency-compensated input through a 32-tap windowed-sinc interpolator at a ratio
    rho = 2^(cents/1200). The read head drifts from the nominal position (t - L) by (rho - 1) per
    sample; drift is removed by splices:
      * normal splice when |drift| > driftSoft: new position chosen by normalised cross-correlation,
        12 ms crossfade (equal-gain if well correlated, equal-power otherwise),
      * onset alignment: just before an upcoming onset the head is spliced back to exactly the
        nominal position, so every hit is output on time,
      * no voluntary splice within holdSec after an onset,
      * forced splice when |drift| > driftHard.
    At rho == 1 with zero drift the output is the exactly delayed input (bit-exact).
*/
class WholeShifter
{
public:
    void prepare (double sampleRate, int maxLatency, int numChannels);
    void reset() noexcept;
    /** Sets the nominal delay and realigns the read head (keeps the input history). */
    void setLatency (int samples) noexcept;
    /** Puts the read head exactly on the nominal position for the next output sample. */
    void realign() noexcept;

    /** Current (already glided) ratio. */
    void setRatio (double r) noexcept { ratio = r; }
    double getRatio() const noexcept { return ratio; }

    /** Splice decisions; call at the start of every control tick, before pushing sample t. */
    void controlTick (std::int64_t t, const OnsetDetector& onsets) noexcept;

    /** Push the input sample for time t (count must equal t). */
    void push (const float* x, int numCh, float mono) noexcept;
    /** Render the output sample for time t (after push). */
    void render (float* y, int numCh) noexcept;

    bool isIdentity (std::int64_t t) const noexcept;
    double driftSamples (std::int64_t t) const noexcept { return posA - (double) (t - latency); }
    int spliceCount() const noexcept { return splices; }

    double driftSoftSec = 0.010;
    double driftHardSec = 0.030;
    double crossfadeSec = 0.012;
    double holdSec = 0.060;
    double onsetLookSec = 0.030;

private:
    float readInterp (int ch, double pos, const float* kernel, int i0) const noexcept;
    void startSplice (double newPos, int xfLen, bool equalGain) noexcept;
    float ncc (double posARead, std::int64_t candidateStart, int len, int stride) const noexcept;
    const float* kernelTableFor (double r) const noexcept;

    double fs = 48000.0;
    int nCh = 2;
    int latency = 0;
    std::vector<float> ring[kMaxChannels];
    std::vector<float> mono;
    int mask = 0;
    std::int64_t count = 0;

    double ratio = 1.0;
    double posA = 0.0, posB = 0.0;
    bool xfActive = false;
    int xfPos = 0, xfLen = 1;
    bool xfEqualGain = true;
    int splices = 0;
    std::int64_t lastSpliceT = INT64_MIN / 2;

    static constexpr int kTaps = 32;
    static constexpr int kHalf = 16;
    static constexpr int kPhases = 256;
    static constexpr int kTables = 4;
    std::vector<float> tables;   // kTables x (kPhases + 1) x kTaps
    float tableCutoff[kTables] {};
    float kernelScratch[kTaps] {};
};

} // namespace fr
