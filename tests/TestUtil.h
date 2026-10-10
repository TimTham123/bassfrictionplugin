// Friction - test utilities: signal generators, WAV writer, measurements
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace ft
{

constexpr double kPi = 3.14159265358979323846;
using Buf = std::vector<float>;

// ------------------------------------------------------------------ generators

inline Buf sine (double fs, double seconds, double f, double amp, double phase = 0.0)
{
    Buf b ((size_t) std::lround (fs * seconds));
    for (size_t i = 0; i < b.size(); ++i)
        b[i] = (float) (amp * std::sin (2.0 * kPi * f * (double) i / fs + phase));
    return b;
}

/** Band-limited sawtooth (additive), amplitude of harmonic k = amp / k. */
inline Buf saw (double fs, double seconds, double f, double amp, int maxHarm = 400, double phase = 0.0)
{
    Buf b ((size_t) std::lround (fs * seconds), 0.0f);
    const int H = std::min (maxHarm, (int) (0.45 * fs / f));
    for (int k = 1; k <= H; ++k)
    {
        const double w = 2.0 * kPi * f * k / fs;
        const double a = amp / k * (2.0 / kPi);
        for (size_t i = 0; i < b.size(); ++i)
            b[i] += (float) (a * std::sin (w * (double) i + phase * k));
    }
    return b;
}

/** Harmonic tone with explicit partial amplitudes (index 0 = fundamental). */
inline Buf harmonic (double fs, double seconds, double f, const std::vector<double>& amps, double inharm = 0.0)
{
    Buf b ((size_t) std::lround (fs * seconds), 0.0f);
    for (size_t k = 0; k < amps.size(); ++k)
    {
        const double n = (double) (k + 1);
        const double fk = f * n * std::sqrt (1.0 + inharm * n * n);
        if (fk > 0.45 * fs)
            break;
        const double w = 2.0 * kPi * fk / fs;
        for (size_t i = 0; i < b.size(); ++i)
            b[i] += (float) (amps[k] * std::sin (w * (double) i + 0.3 * n));
    }
    return b;
}

inline void addInto (Buf& dst, const Buf& src, double gain = 1.0, size_t offset = 0)
{
    for (size_t i = 0; i < src.size() && i + offset < dst.size(); ++i)
        dst[i + offset] += (float) (gain * src[i]);
}

inline Buf mixOf (const Buf& a, const Buf& b)
{
    Buf r (std::max (a.size(), b.size()), 0.0f);
    addInto (r, a);
    addInto (r, b);
    return r;
}

inline Buf delayed (const Buf& x, int d)
{
    Buf r (x.size(), 0.0f);
    for (size_t i = (size_t) std::max (0, d); i < x.size(); ++i)
        r[i] = x[i - (size_t) d];
    return r;
}

/** Synthetic kick: exponential pitch drop, amplitude decay, short click. Returns per-sample f(t) too. */
struct KickParams { double fStart = 160.0, fEnd = 52.0, dropTau = 0.025, ampTau = 0.30, amp = 0.8, click = 0.15; };
inline void addKick (Buf& dst, double fs, size_t at, const KickParams& k, double seconds = 0.6)
{
    double phase = 0.0;
    const size_t n = (size_t) (seconds * fs);
    for (size_t i = 0; i < n && at + i < dst.size(); ++i)
    {
        const double tt = (double) i / fs;
        const double f = k.fEnd + (k.fStart - k.fEnd) * std::exp (-tt / k.dropTau);
        phase += 2.0 * kPi * f / fs;
        // smooth end (a truncated tail would be a step discontinuity in the test signal itself)
        const double tEnd = (double) (n - i) / fs;
        const double fadeOut = tEnd < 0.03 ? 0.5 - 0.5 * std::cos (kPi * tEnd / 0.03) : 1.0;
        const double env = std::exp (-tt / k.ampTau) * std::min (1.0, tt / 0.001) * fadeOut;
        double s = k.amp * env * std::sin (phase);
        if (tt < 0.004)
            s += k.click * (1.0 - tt / 0.004) * ((i * 7919u) % 13 < 6 ? 1.0 : -1.0);
        dst[at + i] += (float) s;
    }
}
inline double kickFreqAt (const KickParams& k, double tt) { return k.fEnd + (k.fStart - k.fEnd) * std::exp (-tt / k.dropTau); }
inline double kickAmpAt (const KickParams& k, double tt) { return k.amp * std::exp (-tt / k.ampTau) * std::min (1.0, tt / 0.001); }

inline Buf noise (double fs, double seconds, double amp, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> d (-1.0, 1.0);
    Buf b ((size_t) std::lround (fs * seconds));
    for (auto& v : b)
        v = (float) (amp * d (rng));
    return b;
}

// ------------------------------------------------------------------ WAV writer

inline bool writeWav (const std::string& path, const std::vector<Buf>& chans, double fs, int bits = 16)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f)
        return false;
    const uint16_t nc = (uint16_t) chans.size();
    const uint32_t frames = chans.empty() ? 0 : (uint32_t) chans[0].size();
    const uint16_t bps = (uint16_t) bits;
    const uint32_t dataBytes = frames * nc * (bps / 8);
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (1); w16 (nc); w32 ((uint32_t) fs);
    w32 ((uint32_t) fs * nc * (bps / 8)); w16 ((uint16_t) (nc * (bps / 8))); w16 (bps);
    std::fwrite ("data", 1, 4, f); w32 (dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < nc; ++c)
        {
            double v = std::clamp ((double) chans[c][i], -1.0, 1.0);
            if (bps == 16)
            {
                const int16_t s = (int16_t) std::lround (v * 32767.0);
                std::fwrite (&s, 2, 1, f);
            }
            else
            {
                const int32_t s = (int32_t) std::lround (v * 8388607.0);
                const uint8_t b3[3] = { (uint8_t) (s & 0xff), (uint8_t) ((s >> 8) & 0xff), (uint8_t) ((s >> 16) & 0xff) };
                std::fwrite (b3, 1, 3, f);
            }
        }
    std::fclose (f);
    return true;
}

// ------------------------------------------------------------------ measurements

inline double rms (const Buf& x, size_t a = 0, size_t b = SIZE_MAX)
{
    b = std::min (b, x.size());
    if (b <= a) return 0.0;
    double acc = 0.0;
    for (size_t i = a; i < b; ++i)
        acc += (double) x[i] * x[i];
    return std::sqrt (acc / (double) (b - a));
}

inline double db (double v) { return v > 1e-30 ? 20.0 * std::log10 (v) : -600.0; }

inline double maxAbsDiff (const Buf& a, const Buf& b, size_t from = 0, size_t to = SIZE_MAX)
{
    to = std::min ({ to, a.size(), b.size() });
    double m = 0.0;
    for (size_t i = from; i < to; ++i)
        m = std::max (m, (double) std::abs ((double) a[i] - (double) b[i]));
    return m;
}

inline bool allFinite (const Buf& x)
{
    for (float v : x)
        if (! std::isfinite (v))
            return false;
    return true;
}

/** Simple radix-2 complex FFT (double), for measurements only. */
inline void fftc (std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / (double) len * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& v : a) v /= (double) n;
}

inline size_t pow2Ceil (size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

/** Analytic signal of x restricted to the band [fLo, fHi] (FFT brick-wall), length = x.size(). */
inline std::vector<std::complex<double>> bandAnalytic (const Buf& x, double fs, double fLo, double fHi)
{
    const size_t n = pow2Ceil (x.size());
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    fftc (a, false);
    for (size_t k = 0; k < n; ++k)
    {
        const double f = (double) k * fs / (double) n;
        const bool pos = k > 0 && k < n / 2;
        if (! pos || f < fLo || f > fHi) a[k] = 0.0;
        else a[k] *= 2.0;
    }
    fftc (a, true);
    a.resize (x.size());
    return a;
}

/** Real band-pass (FFT brick-wall). */
inline Buf bandPass (const Buf& x, double fs, double fLo, double fHi)
{
    auto a = bandAnalytic (x, fs, fLo, fHi);
    Buf r (x.size());
    for (size_t i = 0; i < x.size(); ++i) r[i] = (float) a[i].real();
    return r;
}

/** Causal 4th-order Butterworth high-pass (no long-range ringing, unlike the FFT brick-wall filters). */
inline Buf highPass4 (const Buf& x, double fs, double fc)
{
    struct BQ { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
                double run (double v) { const double y = b0 * v + z1; z1 = b1 * v - a1 * y + z2; z2 = b2 * v - a2 * y; return y; } };
    auto make = [&] (double q)
    {
        const double w0 = 2.0 * kPi * fc / fs, c = std::cos (w0), sn = std::sin (w0), al = sn / (2.0 * q), a0 = 1.0 + al;
        return BQ { (1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0, (1.0 - al) / a0 };
    };
    BQ s1 = make (0.5412), s2 = make (1.3066);
    Buf y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        y[i] = (float) s2.run (s1.run (x[i]));
    return y;
}

/** Frequency of the dominant component in [fLo,fHi] over [a,b) via phase-slope regression. */
inline double bandFrequency (const Buf& x, double fs, double fLo, double fHi, size_t a, size_t b)
{
    auto z = bandAnalytic (x, fs, fLo, fHi);
    b = std::min (b, z.size());
    double prev = std::arg (z[a]), unwrapped = prev;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    size_t n = 0;
    for (size_t i = a; i < b; ++i)
    {
        const double ph = std::arg (z[i]);
        double d = ph - prev;
        while (d > kPi) d -= 2 * kPi;
        while (d < -kPi) d += 2 * kPi;
        unwrapped += (i == a) ? 0.0 : d;
        prev = ph;
        const double tt = (double) (i - a) / fs;
        sx += tt; sy += unwrapped; sxx += tt * tt; sxy += tt * unwrapped; ++n;
    }
    const double slope = ((double) n * sxy - sx * sy) / ((double) n * sxx - sx * sx);
    return slope / (2.0 * kPi);
}

/** Envelope (magnitude of the band analytic signal). */
inline Buf bandEnvelope (const Buf& x, double fs, double fLo, double fHi)
{
    auto z = bandAnalytic (x, fs, fLo, fHi);
    Buf e (x.size());
    for (size_t i = 0; i < x.size(); ++i) e[i] = (float) std::abs (z[i]);
    return e;
}

/** Modulation depth of an envelope over [a,b): 2 |E(fm)| / E(0), via a DFT at the modulation frequency. */
inline double modulationDepthAt (const Buf& env, double fs, double fm, size_t a, size_t b)
{
    b = std::min (b, env.size());
    // use an integer number of modulation periods
    const double period = fs / fm;
    const size_t periods = (size_t) std::floor ((double) (b - a) / period);
    if (periods == 0) return 0.0;
    const size_t len = (size_t) std::lround ((double) periods * period);
    std::complex<double> c (0.0, 0.0);
    double dc = 0.0;
    for (size_t i = 0; i < len; ++i)
    {
        const double v = env[a + i];
        dc += v;
        c += v * std::exp (std::complex<double> (0.0, -2.0 * kPi * fm * (double) i / fs));
    }
    dc /= (double) len;
    c /= (double) len;
    return dc > 1e-12 ? 2.0 * std::abs (c) / dc : 0.0;
}

/** Peak-to-peak modulation depth (max-min)/(max+min) of a smoothed envelope over [a,b). */
inline double modulationDepthPP (const Buf& env, size_t a, size_t b)
{
    b = std::min (b, env.size());
    double mx = 0.0, mn = 1e30;
    for (size_t i = a; i < b; ++i) { mx = std::max (mx, (double) env[i]); mn = std::min (mn, (double) env[i]); }
    return (mx + mn) > 0 ? (mx - mn) / (mx + mn) : 0.0;
}

/** RMS envelope with a sliding window (centred). */
inline Buf rmsEnvelope (const Buf& x, int win)
{
    Buf e (x.size(), 0.0f);
    double acc = 0.0;
    std::vector<double> sq (x.size());
    for (size_t i = 0; i < x.size(); ++i) sq[i] = (double) x[i] * x[i];
    const int h = win / 2;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const long lo = std::max (0L, (long) i - h), hi = std::min ((long) x.size() - 1, (long) i + h);
        if (i == 0) { acc = 0; for (long k = lo; k <= hi; ++k) acc += sq[(size_t) k]; }
        else
        {
            const long prevLo = std::max (0L, (long) i - 1 - h), prevHi = std::min ((long) x.size() - 1, (long) i - 1 + h);
            if (lo > prevLo) acc -= sq[(size_t) prevLo];
            if (hi > prevHi) acc += sq[(size_t) hi];
        }
        e[i] = (float) std::sqrt (std::max (0.0, acc) / (double) (hi - lo + 1));
    }
    return e;
}

/** Lag (in samples) of the maximum cross-correlation of y against x within [-maxLag, maxLag]. */
inline int bestLag (const Buf& x, const Buf& y, int maxLag, size_t a, size_t b)
{
    double best = -1e300;
    int bestL = 0;
    for (int L = -maxLag; L <= maxLag; ++L)
    {
        double acc = 0.0;
        for (size_t i = a; i < b; ++i)
        {
            const long j = (long) i + L;
            if (j < 0 || j >= (long) y.size()) continue;
            acc += (double) x[i] * y[(size_t) j];
        }
        if (acc > best) { best = acc; bestL = L; }
    }
    return bestL;
}

} // namespace ft
