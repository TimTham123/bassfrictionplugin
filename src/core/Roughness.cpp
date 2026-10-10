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

float computeCrossFriction (Partial* target, int nT, const Partial* ref, int nR, Clash* worst) noexcept
{
    const Roughness& R = Roughness::get();
    float levR[kMaxPartials];
    const int nr = std::min (nR, kMaxPartials);
    for (int j = 0; j < nr; ++j)
        levR[j] = Roughness::level (ref[j].amp);

    float total = 0.0f;
    float worstR = 0.0f;
    for (int i = 0; i < nT; ++i)
    {
        Partial& p = target[i];
        const float li = Roughness::level (p.amp);
        float acc = 0.0f;
        for (int j = 0; j < nr; ++j)
        {
            const float r = R.pairL (p.freq, p.amp, li, ref[j].freq, ref[j].amp, levR[j]);
            if (r > 0.0f)
            {
                acc += r;
                if (worst != nullptr && r > worstR)
                {
                    worstR = r;
                    worst->valid = true;
                    worst->targetHz = p.freq;
                    worst->refHz = ref[j].freq;
                    worst->cents = (float) centsBetween (ref[j].freq, p.freq);
                    worst->beatHz = std::abs (p.freq - ref[j].freq);
                    worst->roughness = r;
                }
            }
        }
        p.friction = acc;
        total += acc;
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
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            const float r = R.pairL (target[i].freq, target[i].amp, lev[i], target[j].freq, target[j].amp, lev[j]);
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
    const Roughness& R = Roughness::get();
    const float l = Roughness::level (a);
    float acc = 0.0f;
    for (int j = 0; j < nR; ++j)
        acc += R.pairL (f, a, l, ref[j].freq, ref[j].amp, Roughness::level (ref[j].amp));
    return acc;
}

} // namespace fr
