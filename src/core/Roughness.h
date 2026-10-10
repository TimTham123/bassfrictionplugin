// Friction - roughness model (Sethares frequency term + Vassilakis amplitude terms)
// See DESIGN.md section 1.4 for the exact formula.
#pragma once

#include "Common.h"

namespace fr
{

class Roughness
{
public:
    // Sethares (1993) / Vassilakis (2001) constants
    static constexpr float kB1 = 3.5f;
    static constexpr float kB2 = 5.75f;
    static constexpr float kDStar = 0.24f;
    static constexpr float kS1 = 0.0207f;
    static constexpr float kS2 = 18.96f;
    static constexpr float kLevelExp = 0.1f;   // X^0.1
    static constexpr float kFluctExp = 3.11f;  // Y^3.11
    static constexpr float kXMax = 2.0f;       // pairs beyond this are ignored (zHat < 0.5 %)

    /** Shared, immutable lookup tables. Call once outside hot loops and keep the reference. */
    static const Roughness& get();

    /** e^{-b1 x} - e^{-b2 x} */
    static double zExact (double x) noexcept;
    /** Peak value of zExact (at x* = ln(b2/b1)/(b2-b1)). */
    static double zMax() noexcept;
    static double xStar() noexcept;
    /** zExact(x) / zMax(): 1 at the roughness peak. */
    static double zHatExact (double x) noexcept { return zExact (x) / zMax(); }

    static float sFactor (float fMin) noexcept { return kDStar / (kS1 * fMin + kS2); }
    /** Frequency difference of maximum roughness above a partial at f (Hz). */
    static float peakDeltaHz (float f) noexcept { return (float) xStar() / sFactor (f); }
    /** Largest frequency difference (Hz) that can still be rough for a lower partial at fMin. */
    static float windowHz (float fMin) noexcept { return kXMax / sFactor (fMin); }

    /** Normalised frequency term from the LUT; x >= 0. */
    float zHat (float x) const noexcept
    {
        if (x >= kXMax) return 0.0f;
        const float p = x * zScale;
        const int i = (int) p;
        const float fr = p - (float) i;
        return zTab[i] + fr * (zTab[i + 1] - zTab[i]);
    }

    /** Y^3.11 with Y = 2 aMin / (aMin + aMax), via LUT on the ratio aMin/aMax. */
    float fluct (float a1, float a2) const noexcept
    {
        const float lo = std::min (a1, a2), hi = std::max (a1, a2);
        if (hi <= 0.0f) return 0.0f;
        const float p = (lo / hi) * (float) kYN;
        const int i = std::min ((int) p, kYN - 1);
        const float fr = p - (float) i;
        return yTab[i] + fr * (yTab[i + 1] - yTab[i]);
    }

    static float level (float a) noexcept { return a > 0.0f ? std::pow (a, kLevelExp) : 0.0f; }

    /** Full pair roughness r(f1,a1,f2,a2), peak-normalised (two equal 0 dBFS sines at worst spacing = 1). */
    float pair (float f1, float a1, float f2, float a2) const noexcept
    {
        return pairL (f1, a1, level (a1), f2, a2, level (a2));
    }

    /** Same with precomputed level terms l = a^0.1. */
    float pairL (float f1, float a1, float l1, float f2, float a2, float l2) const noexcept
    {
        const float df = std::abs (f2 - f1);
        const float x = df * sFactor (std::min (f1, f2));
        if (x >= kXMax) return 0.0f;
        return l1 * l2 * fluct (a1, a2) * zHat (x);
    }

    /** Exact (no LUT) reference implementation, used by tests. */
    static double pairExact (double f1, double a1, double f2, double a2) noexcept;

private:
    Roughness();

    static constexpr int kZN = 4096;
    static constexpr int kYN = 1024;
    float zScale = 0.0f;
    float zTab[kZN + 2] {};
    float yTab[kYN + 2] {};
};

/** Friction of every target partial against the reference set (fills Partial::friction).
    Returns total friction; optionally reports the worst pair. */
float computeCrossFriction (Partial* target, int nT, const Partial* ref, int nR, Clash* worst) noexcept;

/** Target-internal friction (fills Partial::self); each pair is attributed to its weaker partial. */
float computeSelfFriction (Partial* target, int nT, Clash* worst) noexcept;

/** Friction of a single partial at frequency f, amplitude a against a reference set. */
float frictionAgainst (float f, float a, const Partial* ref, int nR) noexcept;

} // namespace fr
