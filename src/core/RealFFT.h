// Friction - real-input FFT (radix-2, half-size complex transform + split)
#pragma once

#include <complex>
#include <vector>

namespace fr
{

using cfloat = std::complex<float>;

/**
    Real FFT of power-of-two size N.
    forward(): N real samples -> N/2+1 complex bins, X[k] = sum x[n] e^{-j 2 pi k n / N} (unnormalised).
    inverse(): N/2+1 complex bins -> N real samples, normalised so that inverse(forward(x)) == x.
    All memory is allocated in prepare(); forward/inverse are allocation-free.
*/
class RealFFT
{
public:
    void prepare (int size);
    int size() const noexcept { return n; }

    void forward (const float* in, cfloat* out) noexcept;
    void inverse (const cfloat* in, float* out) noexcept;

private:
    void complexTransform (float* re, float* im, bool inverseDir) noexcept;

    int n = 0, m = 0; // m = n / 2 (complex transform size)
    std::vector<float> twRe, twIm;        // e^{-j 2 pi k / m}, k < m/2
    std::vector<float> splitRe, splitIm;  // e^{-j 2 pi k / n}, k <= m
    std::vector<int> bitrev;
    std::vector<float> workRe, workIm;
};

} // namespace fr
