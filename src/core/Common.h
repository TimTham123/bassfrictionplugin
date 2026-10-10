// Friction - psychoacoustic roughness control
// Common types, constants and helpers for the JUCE-independent DSP core.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
 #include <xmmintrin.h>
 #define FR_HAS_SSE 1
#else
 #define FR_HAS_SSE 0
#endif

namespace fr
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

constexpr int kMaxChannels = 2;      // target and reference are each mono or stereo
constexpr int kMaxPeaks = 160;       // STFT peaks kept per signal per frame
constexpr int kMaxPartials = kMaxPeaks + 16; // merged partials per signal per frame (all peaks + low-band harmonics)
constexpr int kMaxSlots = 224;       // tracker slots per signal (peaks plus coasting tracks)
constexpr int kLfHarmonics = 8;      // low-band harmonics per tracker
constexpr int kControlTick = 16;     // all control events happen on multiples of this

// ---------------------------------------------------------------- enums / params
enum class Mode : int { Smooth = 0, Retune = 1, Tension = 2 };
enum class LatencyMode : int { Mix = 0, Track = 1 };
enum class ReferenceSource : int { Sidechain = 0, Self = 1 };
enum class Focus : int { Low = 0, High = 1, Both = 2 };
enum class RetuneStyle : int { PerPartial = 0, WholeAuto = 1, WholeFixed = 2 };
enum class TensionStyle : int { Synth = 0, Push = 1 };
enum class SidechainStatus : int { Absent = 0, Silent = 1, Active = 2 };

/** Engine-side parameter snapshot (plain values, already de-normalised). */
struct Params
{
    Mode mode = Mode::Smooth;
    LatencyMode latencyMode = LatencyMode::Mix;
    ReferenceSource reference = ReferenceSource::Sidechain;
    float amount = 0.5f;         // 0..1
    float sensitivity = 0.65f;   // 0..1
    float attackMs = 20.0f;
    float releaseMs = 150.0f;
    float mix = 1.0f;            // 0..1
    float trimDb = 0.0f;
    float rangeLowHz = 20.0f;
    float rangeHighHz = 20000.0f;
    Focus focus = Focus::Both;
    float crossoverHz = 200.0f;
    bool delta = false;
    bool bypass = false;
    float maxCents = 50.0f;
    RetuneStyle retuneStyle = RetuneStyle::PerPartial;
    float wholeOffsetCents = 0.0f;
    float beatRate = 0.4f;       // 0..1
    TensionStyle tensionStyle = TensionStyle::Synth;
    bool learn = false;          // Advisor learning (UI button, not automatable)
};

// ---------------------------------------------------------------- helpers
inline float clampf (float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd (double v, double lo, double hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g, float floorDb = -200.0f) noexcept
{
    return g > 1.0e-10f ? 20.0f * std::log10 (g) : floorDb;
}

inline double centsBetween (double fFrom, double fTo) noexcept { return 1200.0 * std::log2 (fTo / fFrom); }
inline double centsToRatio (double cents) noexcept { return std::exp2 (cents / 1200.0); }

/** Glasberg & Moore (1990) equivalent rectangular bandwidth, Hz. */
inline double erbHz (double f) noexcept { return 24.7 * (4.37 * f / 1000.0 + 1.0); }
/** ERB-rate (ERB number) scale. */
inline double erbRate (double f) noexcept { return 21.4 * std::log10 (4.37 * f / 1000.0 + 1.0); }
inline double erbRateToHz (double e) noexcept { return (std::pow (10.0, e / 21.4) - 1.0) * 1000.0 / 4.37; }
/** Zwicker & Terhardt (1980) critical bandwidth, Hz. */
inline double zwickerCbHz (double f) noexcept
{
    const double k = f / 1000.0;
    return 25.0 + 75.0 * std::pow (1.0 + 1.4 * k * k, 0.69);
}

/** One-pole smoothing coefficient for a time constant (seconds) at an update rate (Hz). */
inline float onePoleCoeff (double tauSeconds, double updateRateHz) noexcept
{
    if (tauSeconds <= 0.0 || updateRateHz <= 0.0)
        return 1.0f;
    return (float) (1.0 - std::exp (-1.0 / (tauSeconds * updateRateHz)));
}

inline bool isFiniteF (float x) noexcept { return std::isfinite (x); }

inline int nextPow2 (int v) noexcept
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/** Sensitivity (0..1) -> friction threshold. See DESIGN.md section 6.1. */
inline float sensitivityToThreshold (float s) noexcept
{
    return 0.6f * std::pow (1.0f / 30.0f, clampf (s, 0.0f, 1.0f));
}

/** MIDI note number (fractional) for a frequency. */
inline double hzToMidi (double f) noexcept { return 69.0 + 12.0 * std::log2 (f / 440.0); }
inline double midiToHz (double m) noexcept { return 440.0 * std::exp2 ((m - 69.0) / 12.0); }

/** Writes e.g. "A1" into buf (>= 6 chars). */
inline void midiNoteName (int midi, char* buf) noexcept
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = ((midi % 12) + 12) % 12;
    const int oct = (int) std::floor (midi / 12.0) - 1;
    const char* nm = names[n];
    int i = 0;
    while (*nm)
        buf[i++] = *nm++;
    if (oct < 0) { buf[i++] = '-'; buf[i++] = (char) ('0' - oct); }
    else if (oct < 10) buf[i++] = (char) ('0' + oct);
    else { buf[i++] = '1'; buf[i++] = (char) ('0' + oct - 10); }
    buf[i] = 0;
}

/** Flush denormals to zero for the lifetime of the object (x86 SSE; no-op elsewhere). */
class ScopedFlushDenormals
{
public:
    ScopedFlushDenormals() noexcept
    {
       #if FR_HAS_SSE
        saved = _mm_getcsr();
        _mm_setcsr (saved | 0x8040); // FTZ | DAZ
       #endif
    }
    ~ScopedFlushDenormals() noexcept
    {
       #if FR_HAS_SSE
        _mm_setcsr (saved);
       #endif
    }
    ScopedFlushDenormals (const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator= (const ScopedFlushDenormals&) = delete;

private:
   #if FR_HAS_SSE
    unsigned int saved = 0;
   #endif
};

/** A partial as used by the roughness engine. */
struct Partial
{
    float freq = 0.0f;   // Hz
    float amp = 0.0f;    // linear peak amplitude (1 = full-scale sine)
    float bin = -1.0f;   // fractional STFT bin, -1 for low-band tracker partials
    float lo = 0.0f, hi = 0.0f; // STFT region of influence (bins, valley to valley)
    int slot = -1;       // tracker slot (STFT) or kLfSlotBase + harmonic index
    std::uint8_t src = 0;   // 0 = STFT, 1 = low-band tracker
    float friction = 0.0f;  // vs reference
    float self = 0.0f;      // target-internal
};

constexpr int kLfSlotBase = 1000;

/** Worst current clash, for the readout. */
struct Clash
{
    bool valid = false;
    float targetHz = 0.0f;
    float refHz = 0.0f;
    float cents = 0.0f;     // target relative to reference
    float beatHz = 0.0f;
    float roughness = 0.0f;
};

} // namespace fr
