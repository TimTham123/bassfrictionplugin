// Friction - top-level DSP engine (JUCE-independent)
#pragma once

#include "Advisor.h"
#include "BellBank.h"
#include "Common.h"
#include "DisplayData.h"
#include "LockFree.h"
#include "LowBandTracker.h"
#include "OnsetDetector.h"
#include "PartialShifter.h"
#include "RealFFT.h"
#include "Roughness.h"
#include "SpectralAnalyzer.h"
#include "SynthBank.h"
#include "WholeShifter.h"

#include <vector>

namespace fr
{

/**
    The whole plugin DSP: analysis of target + reference, roughness, Smooth / Retune / Tension
    processing, latency modes, display and Advisor publishing.

    Threading: prepare()/reset() on a non-realtime thread; setParams()/process() on the audio
    thread only. process() never allocates, locks or throws. Display frames go out through a
    triple buffer, Advisor frames through an SPSC FIFO.

    All control events happen on absolute sample indices that are multiples of kControlTick, so
    the output does not depend on the host block size.
*/
class FrictionEngine
{
public:
    FrictionEngine();

    void prepare (double sampleRate, int maxBlockSize, int numTargetChannels);
    void reset();

    void setParams (const Params& p) noexcept { params = p; }
    const Params& getParams() const noexcept { return params; }

    /** io: target channels (processed in place). sc: reference channels (may be null / 0 channels). */
    void process (float* const* io, int numCh, const float* const* sc, int numScCh, int numSamples,
                  bool sidechainPresent) noexcept;

    int latencyFor (LatencyMode m) const noexcept { return m == LatencyMode::Mix ? mixN : 0; }
    int currentLatency() const noexcept { return latencyFor (activeLatency); }
    LatencyMode activeLatencyMode() const noexcept { return activeLatency; }
    double getSampleRate() const noexcept { return fs; }
    int mixFftSize() const noexcept { return mixN; }
    int trackFftSize() const noexcept { return trackN; }

    TripleBuffer<DisplayFrame>& display() noexcept { return displayBuf; }
    SpscFifo<AdvisorFrame>& advisorFifo() noexcept { return advisorQ; }

    // ------------------------------------------------------------ introspection (tests / UI)
    struct Probe
    {
        int nT = 0, nR = 0;
        Partial T[kMaxPartials];
        Partial R[kMaxPartials];
        float totalFriction = 0.0f, totalSelf = 0.0f;
        Clash worst;
        bool lfTVoiced = false, lfRVoiced = false;
        float lfTF0 = 0.0f, lfRF0 = 0.0f;
        float wholeCents = 0.0f, wholeTargetCents = 0.0f;
        float maxDepthDb = 0.0f;
        int pvRequests = 0;
        int synthVoices = 0;
        bool selfMode = false;
        SidechainStatus sidechain = SidechainStatus::Absent;
        std::int64_t frameTime = 0;
        int frames = 0;
        int splices = 0;
        bool lastFrameIdentity = true;
    };
    const Probe& probe() const noexcept { return lastProbe; }

private:
    // ------------------------------------------------------------ per-mode analysis state
    struct SlotState
    {
        float fSmooth = 0.0f;
        bool active = false;
        float depthDb = 0.0f;      // Smooth: current attenuation (positive dB)
        float shiftCents = 0.0f;   // Retune/Push: current smoothed shift
        float targetCents = 0.0f;  // decided target
        float lastFreq = 0.0f;
        float loBin = 0.0f, hiBin = 0.0f; // region of the partial in the analysis spectrum
        int bell = -1;             // Track mode bell assignment
        bool seen = false;
    };

    struct Analysis
    {
        int N = 0, H = 0;
        SpectralAnalyzer specT, specR;
        PeakTracker trT, trR;
        std::vector<SlotState> slots;
        std::vector<float> frameT, frameR;
        // display mapping: per grid point bin range
        std::vector<int> gridLo, gridHi;
    };

    struct LfRecord { std::int64_t time; float f[kLfHarmonics]; float g[kLfHarmonics]; float bw[kLfHarmonics]; };
    struct SynthRecord { std::int64_t time; int n; VoiceTarget v[16]; };
    struct CentsRecord { std::int64_t time; float cents; };

    template <typename T, int Cap>
    struct Sched
    {
        T items[Cap];
        int head = 0, count = 0;
        void clear() noexcept { head = count = 0; }
        void push (const T& x) noexcept
        {
            if (count == Cap) { head = (head + 1) % Cap; --count; } // drop oldest
            items[(head + count) % Cap] = x;
            ++count;
        }
        const T* peek() const noexcept { return count > 0 ? &items[head] : nullptr; }
        void pop() noexcept { if (count > 0) { head = (head + 1) % Cap; --count; } }
    };

    void configureAnalysis (Analysis& a, int N);
    void controlTick() noexcept;
    void onLfHop() noexcept;
    void onFrame() noexcept;
    void processMixSpectral (Analysis& a, bool smoothActive, bool pvActive) noexcept;
    void decideSmooth (Analysis& a, float threshold) noexcept;
    void decidePerPartial (Analysis& a, float threshold, bool tension) noexcept;
    void decideWholeAuto (float threshold) noexcept;
    void decideTensionSynth (Analysis& a) noexcept;
    void updateTrackBells (Analysis& a) noexcept;
    void publishDisplay (Analysis& a) noexcept;
    void pushAdvisorFrame (Analysis& a) noexcept;
    void applyTransitionIfIdle() noexcept;
    void resetModeState() noexcept;
    bool eligible (float freq, bool lowBand) const noexcept;
    float frictionOf (const Partial& p) const noexcept { return selfMode ? p.self : p.friction; }
    void gatherFrame (const std::vector<float>& ring, std::vector<float>& out, int N) const noexcept;

    // ------------------------------------------------------------ configuration
    Params params;
    double fs = 48000.0;
    int maxBlock = 512;
    int nCh = 2;
    int mixN = 4096, mixH = 1024, trackN = 2048, trackH = 512;
    int ringSize = 0, ringMask = 0;

    LatencyMode activeLatency = LatencyMode::Mix;
    Mode activeMode = Mode::Smooth;
    RetuneStyle activeRetuneStyle = RetuneStyle::PerPartial;
    TensionStyle activeTensionStyle = TensionStyle::Synth;

    // ------------------------------------------------------------ time and rings
    std::int64_t t = 0;
    std::vector<float> inRing[kMaxChannels];
    std::vector<float> midT, midR;
    double hpT_x1 = 0, hpT_y1 = 0, hpR_x1 = 0, hpR_y1 = 0, hpCoeff = 0.999;

    // ------------------------------------------------------------ analysis
    Analysis mixAna, trackAna;
    LowBandTracker lfT, lfR;
    OnsetDetector onsets;
    bool lfPending = false;

    Partial curT[kMaxPartials], curR[kMaxPartials], pvList[kMaxPartials];
    int nCurT = 0, nCurR = 0, nPv = 0;
    int pvPeakIndex[kMaxPartials] {};
    float totalCross = 0.0f, totalSelfF = 0.0f;
    Clash worstCross, worstSelf;
    LfEstimate lastLfT, lastLfR;
    SlotState lfSlots[kLfHarmonics];

    bool selfMode = false;
    bool sidechainPresentNow = false;
    SidechainStatus scStatus = SidechainStatus::Absent;
    double refEnergySmooth = 0.0;
    std::int64_t refLastActive = INT64_MIN / 2;

    // ------------------------------------------------------------ processing
    RealFFT procFft;
    std::vector<float> anaWin, synWin, procFrame;
    std::vector<cfloat> spec[kMaxChannels];
    std::vector<float> ola[kMaxChannels];
    int olaMask = 0;
    std::vector<float> gainDb;
    std::int64_t lastNonIdentityTrigger = INT64_MIN / 2;

    BellBank lfBells;      // Mix-mode low-band bells (one per harmonic)
    BellBank trackBells;   // Track-mode bells: 24 high-band + 8 low-band
    static constexpr int kTrackHfBells = 24;
    std::vector<int> bellOwner;   // Track mode: slot owning each HF bell (-1 free)

    WholeShifter whole;
    PartialShifter pv;
    SynthBank synth;
    ShiftRequest pvReq[kMaxPartials];
    int nPvReq = 0;
    bool frameIsTransient = false;

    struct WholePair { float fi, ai, li, fj, aj, lj; };
    static constexpr int kMaxWholePairs = 1024;
    WholePair wholePairs[kMaxWholePairs] {};

    float wholeTargetCents = 0.0f, wholeCents = 0.0f, wholeDecided = 0.0f;
    float wholeDecidedCost = 0.0f;

    Sched<LfRecord, 128> lfSched;
    Sched<SynthRecord, 16> synthSched;
    Sched<CentsRecord, 64> centsSched;

    // ------------------------------------------------------------ output smoothing / transitions
    float mixSm = 1.0f, trimSm = 1.0f, deltaSm = 0.0f, bypassSm = 0.0f;
    float paramAlpha = 0.01f;
    float wetFade = 1.0f;
    float outFade = 1.0f;
    bool modeTransition = false;
    bool latencyTransition = false;
    float fadeStep = 0.01f;

    // chunk buffers
    float wetBuf[kMaxChannels][kControlTick] {};
    float dryBuf[kMaxChannels][kControlTick] {};
    float synthBuf[kControlTick] {};

    // ------------------------------------------------------------ outputs
    TripleBuffer<DisplayFrame> displayBuf;
    SpscFifo<AdvisorFrame> advisorQ;
    bool wasLearning = false;
    double learnStartTime = 0.0;
    Probe lastProbe;
    std::uint32_t displaySeq = 0;
    int frameCounter = 0;
};

} // namespace fr
