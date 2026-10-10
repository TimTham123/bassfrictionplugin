// Friction - Tuning Advisor accumulator (runs on a background thread; no audio-thread use)
#pragma once

#include "Common.h"
#include "DisplayData.h"

#include <vector>

namespace fr
{

struct AdvisorResult
{
    static constexpr int kRange = 600;          // cents searched either side
    static constexpr int kN = 2 * kRange + 1;

    bool valid = false;
    int frames = 0;
    double seconds = 0.0;
    double frictionSeconds = 0.0;               // integral of total friction at 0 cents

    float bestCents = 0.0f;
    float reductionPct = 0.0f;
    float r0 = 0.0f, rBest = 0.0f;
    float curve[kN] {};                          // R(c) relative to R(0) (1 = unchanged)

    // low band / kick vs bass
    bool lowValid = false;
    float lowBestCents = 0.0f;
    float lowReductionPct = 0.0f;
    float lowCurve[kN] {};

    bool kickDetected = false;
    int kickHits = 0;
    float kickF0 = 0.0f;                         // settled kick fundamental (Hz)
    float kickNewF0 = 0.0f;                      // after the recommended tuning
    struct Note { int midi = 0; float hz = 0.0f; float centsDev = 0.0f; float share = 0.0f; };
    int numNotes = 0;
    Note notes[4];
};

class Advisor
{
public:
    Advisor();
    void reset();
    void consume (const AdvisorFrame& f);
    AdvisorResult finish() const;
    int framesConsumed() const noexcept { return frames; }
    double secondsConsumed() const noexcept { return seconds; }

    /** Formats "+37 cents" / "-1 st -23 ct (-123 cents)". */
    static void formatOffset (float cents, char* buf, int bufSize);

private:
    static constexpr int kN = AdvisorResult::kN;
    static constexpr int kRange = AdvisorResult::kRange;

    std::vector<double> R, RLF;
    std::vector<float> pow2;
    int frames = 0;
    double seconds = 0.0;
    double r0Integral = 0.0;

    // kick statistics
    int onsets = 0;
    float prevSinceOnset = 1.0e9f;
    static constexpr int kF0Bins = 4000;        // 1-cent bins from 20 Hz
    std::vector<double> settledHist;
    double earlySum = 0.0, earlyW = 0.0, settledSum = 0.0, settledW = 0.0;
    std::vector<double> noteW, noteDev, noteHz;
};

} // namespace fr
