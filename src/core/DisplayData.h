// Friction - data published from the audio thread to the GUI and to the Advisor thread
#pragma once

#include "Common.h"

namespace fr
{

/** Log-frequency display grid shared by engine and editor. */
struct DisplayGrid
{
    static constexpr int kPoints = 256;
    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 20000.0f;

    static float hzAt (int i) noexcept
    {
        return kMinHz * std::pow (kMaxHz / kMinHz, (float) i / (float) (kPoints - 1));
    }
    static float indexOf (float hz) noexcept
    {
        return (float) (kPoints - 1) * std::log (hz / kMinHz) / std::log (kMaxHz / kMinHz);
    }
};

struct DisplayFrame
{
    static constexpr int kN = DisplayGrid::kPoints;
    static constexpr int kMarks = 48;

    std::uint32_t sequence = 0;
    float sampleRate = 48000.0f;

    float targetDb[kN] {};
    float refDb[kN] {};
    float friction[kN] {};      // cross-friction curve
    float selfFriction[kN] {};  // target-internal curve
    float gainDb[kN] {};        // Smooth: applied attenuation (<= 0)

    struct Mark
    {
        float freq = 0, amp = 0, friction = 0, self = 0, gainDb = 0, shiftCents = 0;
        std::uint8_t src = 0;
    };
    int numTarget = 0;
    Mark target[kMarks];
    int numRef = 0;
    Mark ref[kMarks];

    Clash worst;
    Clash worstSelf;
    float totalFriction = 0.0f;
    float totalSelf = 0.0f;
    float threshold = 0.0f;
    float wholeShiftCents = 0.0f;   // currently applied global shift (Retune whole)
    float maxGainReductionDb = 0.0f;

    int sidechain = 0;              // SidechainStatus
    int mode = 0, latencyMode = 0, retuneStyle = 0, tensionStyle = 0;
    bool selfAnalysis = false;      // analysis currently uses target-internal friction
    bool lfTargetVoiced = false, lfRefVoiced = false;
    float lfTargetF0 = 0.0f, lfRefF0 = 0.0f;
    int synthVoices = 0;
    int latencySamples = 0;
    float cpuLoad = 0.0f;
};

/** One analysis frame of partial statistics for the Tuning Advisor (POD, fixed size). */
struct AdvisorFrame
{
    static constexpr int kMax = 32;
    double time = 0.0;              // seconds since Learn started
    float hopSeconds = 0.0f;
    std::uint8_t nT = 0, nR = 0;
    float tf[kMax] {}, ta[kMax] {};
    float rf[kMax] {}, ra[kMax] {};
    std::uint8_t tLow[kMax] {}, rLow[kMax] {};  // 1 if from the low-band tracker (below crossover)
    bool tVoiced = false, rVoiced = false;
    float tF0 = 0.0f, rF0 = 0.0f;
    float tLfLevel = 0.0f, rLfLevel = 0.0f;
    float sinceOnset = 1.0e9f;       // seconds since the last target onset
    bool startOfLearn = false;
};

} // namespace fr
