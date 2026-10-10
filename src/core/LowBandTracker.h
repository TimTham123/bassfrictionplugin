// Friction - low-band fundamental tracker (YIN on a decimated band + full-rate refinement)
#pragma once

#include "Common.h"

#include <vector>

namespace fr
{

struct LfEstimate
{
    std::int64_t time = 0;          // full-rate input sample index of the analysis centre
    bool voiced = false;
    float f0 = 0.0f;                // Hz
    float aperiodicity = 1.0f;      // YIN d'(tau*)
    float level = 0.0f;             // RMS of the band-limited signal (linear)
    float harm[kLfHarmonics] {};    // peak amplitude of harmonic k+1 (linear)
};

/** Cascade of biquads in double precision (TDF-II). */
struct BiquadD
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
    void setLowpass (double fc, double q, double fs) noexcept;
};

/**
    Time-domain fundamental tracker for tonal low-frequency content (kick body, bass, 808).
      1. DC blocker + 8th-order Butterworth low-pass (900 Hz), full rate, double precision
      2. decimation by D = round(fs / 4000)
      3. YIN (W = 20 ms, 27.5 .. 420 Hz, threshold 0.15) every 16 decimated samples (~4 ms)
      4. full-rate refinement of the period (5-lag parabola on the full-rate difference function)
      5. harmonic amplitudes by pitch-synchronous projection (Hann over m >= 2 periods)
    Allocation-free after prepare().
*/
class LowBandTracker
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    /** Highest harmonic frequency to report (Hz). */
    void setHarmonicLimitHz (float hz) noexcept { harmLimitHz = hz; }

    /** Push one full-rate sample. Returns true when a new estimate was produced. */
    bool push (float x) noexcept;

    const LfEstimate& latest() const noexcept { return hist[(histHead + kHist - 1) % kHist]; }
    bool hasEstimate() const noexcept { return histCount > 0; }

    /** Nearest estimate to `time`; false if none within maxDist samples. */
    bool lookup (std::int64_t time, LfEstimate& out, std::int64_t maxDist) const noexcept;

    int decimation() const noexcept { return D; }
    int hopSamples() const noexcept { return hopDec * D; }
    double decimatedRate() const noexcept { return fsd; }

    // tunables
    float yinThreshold = 0.15f;
    float voicingThreshold = 0.2f;
    float levelThreshold = 2.0e-4f;   // RMS, about -74 dBFS

private:
    void analyse() noexcept;
    double fullDiff (std::int64_t start, int len, int lag, int stride) const noexcept;

    double fs = 48000.0, fsd = 4000.0;
    int D = 12;
    float harmLimitHz = 250.0f;

    BiquadD lpf[4];
    double hpX1 = 0.0, hpY1 = 0.0, hpR = 0.999;

    std::vector<float> full;  // full-rate band-limited ring
    int fullMask = 0;
    std::int64_t fullCount = 0;

    std::vector<float> dec;   // decimated ring
    int decMask = 0;
    std::int64_t decCount = 0;
    int phase = 0;
    int hopCounter = 0;

    int W = 120, tauMin = 9, tauMax = 146, hopDec = 16;
    std::vector<float> frame, dfn, cmnd, hwin;

    float rawF0[3] {};
    int rawCount = 0;

    static constexpr int kHist = 128;
    LfEstimate hist[kHist];
    int histHead = 0, histCount = 0;
};

} // namespace fr
