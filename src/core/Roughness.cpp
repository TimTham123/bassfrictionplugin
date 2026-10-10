#include "Roughness.h"

namespace fr
{

double Roughness::zExact (double x) noexcept
{
    if (x <= 0.0) return 0.0;
    return std::exp (-(double) kB1 * x) - std::exp (-(double) kB2 * x);
}

double Roughness::xStar() noexcept
{
    return std::log ((double) kB2 / (double) kB1) / ((double) kB2 - (double) kB1);
}

double Roughness::zMax() noexcept
{
    static const double zm = zExact (xStar());
    return zm;
}

double Roughness::pairExact (double f1, double a1, double f2, double a2) noexcept
{
    const double fmin = std::min (f1, f2);
    const double s = (double) kDStar / ((double) kS1 * fmin + (double) kS2);
    const double x = s * std::abs (f2 - f1);
    const double amin = std::min (a1, a2), amax = std::max (a1, a2);
    if (amax <= 0.0 || amin <= 0.0) return 0.0;
    const double X = a1 * a2;
    const double Y = 2.0 * amin / (amin + amax);
    return std::pow (X, (double) kLevelExp) * std::pow (Y, (double) kFluctExp) * zHatExact (x);
}

Roughness::Roughness()
{
    zScale = (float) kZN / kXMax;
    for (int i = 0; i <= kZN + 1; ++i)
    {
        const double x = (double) i / (double) zScale;
        zTab[i] = (float) zHatExact (x);
    }
    for (int i = 0; i <= kYN + 1; ++i)
    {
        const double r = std::min (1.0, (double) i / (double) kYN);
        const double y = 2.0 * r / (1.0 + r);
        yTab[i] = (float) std::pow (y, (double) kFluctExp);
    }
}

const Roughness& Roughness::get()
{
    static const Roughness instance;
    return instance;
}

float coincidenceWeight (float x) noexcept
{
    const float xs = (float) Roughness::xStar();
    const float x0 = 0.25f * xs;
    if (x <= x0) return 1.0f;
    if (x >= xs) return 0.0f;
    return 0.5f * (1.0f + std::cos ((float) kPi * (x - x0) / (xs - x0)));
}

float selfPairWeight (float x) noexcept
{
    const float xs = (float) Roughness::xStar();
    const float x0 = 0.5f * xs;
    if (x <= x0) return 1.0f;
    if (x >= xs) return 0.0f;
    return 0.5f * (1.0f + std::cos ((float) kPi * (x - x0) / (xs - x0)));
}

float partialFriction (float f, float a, float l, const Partial* ref, const float* refLevel, int nR,
                       int* dominant, float* dominantR, const float* refS) noexcept
{
    const Roughness& R = Roughness::get();
    int cand[kMaxPartials];
    float candR[kMaxPartials];
    int nc = 0;
    int jStar = -1;
    float xBest = 1.0e9f;
    const int n = std::min (nR, kMaxPartials);
    const float sf = Roughness::sFactor (f);
    for (int j = 0; j < n; ++j)
    {
        const float fj = ref[j].freq;
        // s is evaluated at the lower partial: precomputed per reference partial when available
        const float s = fj < f ? (refS != nullptr ? refS[j] : Roughness::sFactor (fj)) : sf;
        const float x = std::abs (f - fj) * s;
        if (x < xBest)
        {
            xBest = x;
            jStar = j;
        }
        if (x >= Roughness::kXMax)
            continue;
        const float lj = refLevel != nullptr ? refLevel[j] : Roughness::level (ref[j].amp);
        const float r = l * lj * R.fluct (a, ref[j].amp) * R.zHat (x);
        if (r > 0.0f)
        {
            cand[nc] = j;
            candR[nc] = r;
            ++nc;
        }
    }

    const float w = jStar >= 0 ? coincidenceWeight (xBest) : 0.0f;
    float acc = 0.0f, best = 0.0f;
    int bestJ = -1;
    for (int c = 0; c < nc; ++c)
    {
        const int j = cand[c];
        float r = candR[c];
        if (w > 0.0f && j != jStar)
        {
            const Partial& ps = ref[jStar];
            const float ls = refLevel != nullptr ? refLevel[jStar] : Roughness::level (ps.amp);
            const float lj = refLevel != nullptr ? refLevel[j] : Roughness::level (ref[j].amp);
            const float rInternal = R.pairL (ps.freq, ps.amp, ls, ref[j].freq, ref[j].amp, lj);
            r = std::max (0.0f, r - w * rInternal);
        }
        acc += r;
        if (r > best)
        {
            best = r;
            bestJ = j;
        }
    }
    if (dominant != nullptr) *dominant = bestJ;
    if (dominantR != nullptr) *dominantR = best;
    return acc;
}

float computeCrossFriction (Partial* target, int nT, const Partial* ref, int nR, Clash* worst) noexcept
{
    float levR[kMaxPartials], sR[kMaxPartials], reachLo[kMaxPartials];
    const int nr = std::min (nR, kMaxPartials);
    bool sorted = true;
    for (int j = 0; j < nr; ++j)
    {
        levR[j] = Roughness::level (ref[j].amp);
        sR[j] = Roughness::sFactor (ref[j].freq);
        reachLo[j] = ref[j].freq + Roughness::kXMax / sR[j]; // highest frequency this partial can be rough with
        if (j > 0 && ref[j].freq < ref[j - 1].freq)
            sorted = false;
    }

    float total = 0.0f;
    float worstR = 0.0f;
    for (int i = 0; i < nT; ++i)
    {
        Partial& p = target[i];
        int dom = -1;
        float domR = 0.0f;
        int j0 = 0, j1 = nr;
        if (sorted)
        {
            // window of reference partials that can interact with p (both bounds are monotone in frequency);
            // keep one partial either side so the coincidence search still sees the nearest neighbour
            const float hi = p.freq + Roughness::windowHz (p.freq);
            j0 = (int) (std::upper_bound (reachLo, reachLo + nr, p.freq) - reachLo);
            j1 = (int) (std::lower_bound (ref, ref + nr, hi, [] (const Partial& q, float v) { return q.freq < v; }) - ref);
            j0 = std::max (0, j0 - 1);
            j1 = std::min (nr, j1 + 1);
        }
        p.friction = j1 > j0 ? partialFriction (p.freq, p.amp, Roughness::level (p.amp), ref + j0, levR + j0, j1 - j0, &dom, &domR, sR + j0)
                             : 0.0f;
        if (dom >= 0)
            dom += j0;
        total += p.friction;
        if (worst != nullptr && dom >= 0 && domR > worstR)
        {
            worstR = domR;
            worst->valid = true;
            worst->targetHz = p.freq;
            worst->refHz = ref[dom].freq;
            worst->cents = (float) centsBetween (ref[dom].freq, p.freq);
            worst->beatHz = std::abs (p.freq - ref[dom].freq);
            worst->roughness = domR;
        }
    }
    return total;
}

float computeSelfFriction (Partial* target, int nT, Clash* worst) noexcept
{
    const Roughness& R = Roughness::get();
    float lev[kMaxPartials];
    const int n = std::min (nT, kMaxPartials);
    for (int i = 0; i < n; ++i)
    {
        lev[i] = Roughness::level (target[i].amp);
        target[i].self = 0.0f;
    }

    float total = 0.0f;
    float worstR = 0.0f;
    bool sorted = true;
    for (int i = 1; i < n; ++i)
        if (target[i].freq < target[i - 1].freq)
            sorted = false;
    const float xCut = (float) Roughness::xStar(); // selfPairWeight is zero beyond x*
    for (int i = 0; i < n; ++i)
    {
        const float si = Roughness::sFactor (target[i].freq);
        for (int j = i + 1; j < n; ++j)
        {
            const float fi = target[i].freq, fj = target[j].freq;
            const float x = sorted ? (fj - fi) * si : std::abs (fi - fj) * Roughness::sFactor (std::min (fi, fj));
            if (sorted && x >= xCut)
                break;
            const float w = selfPairWeight (x);
            if (w <= 0.0f)
                continue;
            const float r = w * R.pairL (fi, target[i].amp, lev[i], fj, target[j].amp, lev[j]);
            if (r <= 0.0f)
                continue;
            total += r;
            if (target[i].amp < target[j].amp)       target[i].self += r;
            else if (target[j].amp < target[i].amp)  target[j].self += r;
            else { target[i].self += 0.5f * r; target[j].self += 0.5f * r; }

            if (worst != nullptr && r > worstR)
            {
                worstR = r;
                const bool iWeaker = target[i].amp <= target[j].amp;
                const Partial& weak = iWeaker ? target[i] : target[j];
                const Partial& strong = iWeaker ? target[j] : target[i];
                worst->valid = true;
                worst->targetHz = weak.freq;
                worst->refHz = strong.freq;
                worst->cents = (float) centsBetween (strong.freq, weak.freq);
                worst->beatHz = std::abs (weak.freq - strong.freq);
                worst->roughness = r;
            }
        }
    }
    return total;
}

float frictionAgainst (float f, float a, const Partial* ref, int nR) noexcept
{
    return partialFriction (f, a, Roughness::level (a), ref, nullptr, nR);
}

} // namespace fr
