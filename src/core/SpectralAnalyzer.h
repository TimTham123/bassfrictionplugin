// Friction - STFT peak analysis with reassignment frequency estimation
#pragma once

#include "Common.h"
#include "RealFFT.h"

#include <vector>

namespace fr
{

struct SpectralPeak
{
    float freq = 0.0f;  // Hz (reassigned)
    float amp = 0.0f;   // linear peak amplitude, scalloping-corrected
    float bin = 0.0f;   // fractional bin of the reassigned frequency
    float db = -200.0f; // amplitude in dBFS
    int k = 0;          // integer bin of the magnitude maximum
    int lo = 0, hi = 0; // region of influence (valley to valley, inclusive)
    bool stationary = true; // passed the reassignment-consistency test
};

/**
    Analyses one signal (mono) frame by frame:
      * Hann window (periodic) and its analytic derivative -> two real FFTs
      * local maxima, absolute/relative floors, local-mean noise test
      * reassignment-consistency test (bins k-1, k, k+1 must agree on the frequency)
      * simple spreading-function masking between peaks of the same signal
    Allocation-free after prepare().
*/
class SpectralAnalyzer
{
public:
    void prepare (int fftSize, double sampleRate);

    /** frame: the N most recent samples, oldest first. Returns number of peaks (sorted by frequency). */
    int analyze (const float* frame, float minHz, float maxHz) noexcept;

    const SpectralPeak* peaks() const noexcept { return peakOut; }
    int numPeaks() const noexcept { return nOut; }

    /** Per-bin amplitude in dBFS (bins 0..N/2) of the last frame. */
    const float* magnitudeDb() const noexcept { return magDb.data(); }
    /** Per-bin linear amplitude (bins 0..N/2) of the last frame. */
    const float* magnitude() const noexcept { return mag.data(); }
    const cfloat* spectrum() const noexcept { return X.data(); }

    int fftSize() const noexcept { return n; }
    int numBins() const noexcept { return n / 2 + 1; }
    double binHz() const noexcept { return fs / n; }

    /** Hann kernel magnitude relative to its peak, at fractional offset d (bins). */
    static float hannKernel (float d) noexcept;

    /** Applies the analysis (Hann) window and FFT only; result in spectrum(). */
    void transformOnly (const float* frame) noexcept;

    // tunables (public for tests)
    float absFloorDb = -96.0f;
    float relFloorDb = 70.0f;
    float localMeanMarginDb = 10.0f;      // stationary peaks: prominence over the local log mean
    float strongMarginDb = 15.0f;         // non-stationary (vibrato, glides) peaks need this much
    float consistencyBins = 0.2f;
    int maxRegionHalfWidth = 6;
    float maskOffsetDb = 18.0f;
    float maskSlopeLowDbPerErb = 25.0f;   // masker above the maskee
    float maskSlopeHighDbPerErb = 15.0f;  // masker below the maskee (upward spread)

private:
    float reassignedOffset (int k) const noexcept;

    int n = 0;
    double fs = 48000.0;
    RealFFT fft;
    std::vector<float> win, dwin, tmp;
    std::vector<cfloat> X, Xd;
    std::vector<float> mag, magDb, localMean;
    std::vector<double> prefix;
    std::vector<int> halfWidth;
    std::vector<float> erbOfBin;

    SpectralPeak cand[kMaxPeaks * 2];
    int nCand = 0;
    SpectralPeak peakOut[kMaxPeaks];
    int nOut = 0;
};

/**
    Frame-to-frame continuation of spectral peaks (McAulay & Quatieri style greedy matching).
    Slots are stable while a track lives; a track may coast for up to `maxMissed` frames.
*/
class PeakTracker
{
public:
    void prepare (int maxSlots);
    void reset() noexcept;

    /** freq/amp: n peaks. outSlot receives the slot index per peak. */
    void update (const float* freq, const float* amp, int n, int* outSlot, double binHz) noexcept;

    bool isNew (int slot) const noexcept { return slots[(size_t) slot].born; }
    std::uint32_t idOf (int slot) const noexcept { return slots[(size_t) slot].id; }
    int ageOf (int slot) const noexcept { return slots[(size_t) slot].age; }
    bool isUsed (int slot) const noexcept { return slots[(size_t) slot].used; }
    int numSlots() const noexcept { return (int) slots.size(); }

    int maxMissed = 2;
    float relTolerance = 0.05f;  // 5 % (~85 cents): follows wide vibrato at the hop rate
    float binTolerance = 1.0f;

private:
    struct Slot
    {
        bool used = false;
        bool born = false;
        bool matched = false;
        std::uint32_t id = 0;
        float freq = 0.0f, amp = 0.0f;
        int missed = 0, age = 0;
    };
    struct Cand { float cost; int slot; int peak; };

    std::vector<Slot> slots;
    std::vector<Cand> cands;
    std::uint32_t nextId = 1;
};

} // namespace fr
