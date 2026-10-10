#include "RealFFT.h"
#include "Common.h"

#include <cassert>

namespace fr
{

void RealFFT::prepare (int size)
{
    assert (size >= 8 && (size & (size - 1)) == 0);
    n = size;
    m = size / 2;

    twRe.resize ((size_t) std::max (1, m / 2));
    twIm.resize ((size_t) std::max (1, m / 2));
    for (int k = 0; k < m / 2; ++k)
    {
        const double a = -kTwoPi * k / m;
        twRe[(size_t) k] = (float) std::cos (a);
        twIm[(size_t) k] = (float) std::sin (a);
    }

    splitRe.resize ((size_t) m + 1);
    splitIm.resize ((size_t) m + 1);
    for (int k = 0; k <= m; ++k)
    {
        const double a = -kTwoPi * k / n;
        splitRe[(size_t) k] = (float) std::cos (a);
        splitIm[(size_t) k] = (float) std::sin (a);
    }

    int bits = 0;
    while ((1 << bits) < m)
        ++bits;
    bitrev.resize ((size_t) m);
    for (int i = 0; i < m; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        bitrev[(size_t) i] = r;
    }

    workRe.assign ((size_t) m, 0.0f);
    workIm.assign ((size_t) m, 0.0f);
}

void RealFFT::complexTransform (float* re, float* im, bool inverseDir) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        const int j = bitrev[(size_t) i];
        if (j > i)
        {
            std::swap (re[i], re[j]);
            std::swap (im[i], im[j]);
        }
    }

    const float sign = inverseDir ? -1.0f : 1.0f;

    // size-2 butterflies (twiddle = 1)
    for (int i = 0; i + 1 < m; i += 2)
    {
        const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
        re[i] = ar + br; im[i] = ai + bi;
        re[i + 1] = ar - br; im[i + 1] = ai - bi;
    }

    for (int len = 4; len <= m; len <<= 1)
    {
        const int half = len >> 1;
        const int step = m / len;
        for (int start = 0; start < m; start += len)
        {
            float* r0 = re + start;
            float* i0 = im + start;
            float* r1 = r0 + half;
            float* i1 = i0 + half;
            for (int k = 0; k < half; ++k)
            {
                const float wr = twRe[(size_t) (k * step)];
                const float wi = sign * twIm[(size_t) (k * step)];
                const float tr = wr * r1[k] - wi * i1[k];
                const float ti = wr * i1[k] + wi * r1[k];
                r1[k] = r0[k] - tr;
                i1[k] = i0[k] - ti;
                r0[k] += tr;
                i0[k] += ti;
            }
        }
    }
}

void RealFFT::forward (const float* in, cfloat* out) noexcept
{
    float* re = workRe.data();
    float* im = workIm.data();
    for (int i = 0; i < m; ++i)
    {
        re[i] = in[2 * i];
        im[i] = in[2 * i + 1];
    }

    complexTransform (re, im, false);

    // split: X[k] = E[k] + W^k O[k]
    out[0] = cfloat (re[0] + im[0], 0.0f);
    out[m] = cfloat (re[0] - im[0], 0.0f);
    for (int k = 1; k < m; ++k)
    {
        const float zr = re[k], zi = im[k];
        const float cr = re[m - k], ci = -im[m - k]; // conj(Z[m-k])
        const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);
        // O = (Z - conj(Z[m-k])) / (2j)
        const float dr = 0.5f * (zr - cr), di = 0.5f * (zi - ci);
        const float orr = di, oi = -dr;
        const float wr = splitRe[(size_t) k], wi = splitIm[(size_t) k];
        out[k] = cfloat (er + wr * orr - wi * oi, ei + wr * oi + wi * orr);
    }
}

void RealFFT::inverse (const cfloat* in, float* out) noexcept
{
    float* re = workRe.data();
    float* im = workIm.data();

    for (int k = 0; k < m; ++k)
    {
        const float xr = in[k].real(), xi = in[k].imag();
        const float cr = in[m - k].real(), ci = -in[m - k].imag(); // conj(X[m-k])
        const float er = 0.5f * (xr + cr), ei = 0.5f * (xi + ci);
        // O = (X - conj(X[m-k])) / (2 W^k),  1/W^k = conj(W^k)
        const float dr = 0.5f * (xr - cr), di = 0.5f * (xi - ci);
        const float wr = splitRe[(size_t) k], wi = -splitIm[(size_t) k];
        const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;
        // Z = E + j O
        re[k] = er - oi;
        im[k] = ei + orr;
    }

    complexTransform (re, im, true);

    const float scale = 1.0f / (float) m;
    for (int i = 0; i < m; ++i)
    {
        out[2 * i] = re[i] * scale;
        out[2 * i + 1] = im[i] * scale;
    }
}

} // namespace fr
