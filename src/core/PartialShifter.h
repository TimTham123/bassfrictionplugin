// Friction - per-partial frequency shifting in the STFT domain (Laroche & Dolson peak regions)
#pragma once

#include "Common.h"
#include "RealFFT.h"

#include <vector>

namespace fr
{

struct ShiftRequest
{
    int peakBin = 0;      // integer bin of the peak to move
    float shiftHz = 0.0f; // frequency shift for this frame
    int slot = -1;        // tracker slot (owns the phase accumulator)
};

/**
    Moves the region of influence of each requested peak (valley to valley, max +-4 bins) by
    round(shift / binHz) bins and rotates it rigidly by an accumulated phase
    phi += 2 pi shift H / fs (identity phase locking). Applied identically to every channel, so the
    stereo image is preserved. Bins outside shifted regions are untouched.
    A slot keeps its accumulated phase while it lives so that a shift returning to 0 does not cause
    a phase jump. Transient frames pass unmodified and reset the accumulators.
*/
class PartialShifter
{
public:
    void prepare (int fftSize, int hop, double sampleRate, int maxSlots);
    void reset() noexcept;

    /** spectra: numCh arrays of N/2+1 bins (modified in place). mag: magnitude used for region search. */
    void process (cfloat* const* spectra, int numCh, const float* mag, const ShiftRequest* req, int nReq,
                  bool transient) noexcept;

    /** Must be called when a tracker slot is (re)born or dies. */
    void resetSlot (int slot) noexcept;
    bool slotHasPhase (int slot) const noexcept;
    float slotPhase (int slot) const noexcept;

    int maxRegionHalfWidth = 4;

private:
    int n = 0, hop = 0, bins = 0;
    double fs = 48000.0, binHz = 1.0;
    std::vector<double> phaseAcc;
    std::vector<std::uint8_t> active;
    std::vector<cfloat> scratch[kMaxChannels];
    struct Region { int a, b, shiftBins; cfloat rot; };
    std::vector<Region> regions;
};

} // namespace fr
