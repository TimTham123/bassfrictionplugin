#include "SynthBank.h"

namespace fr
{

void SynthBank::prepare (double sampleRate, int maxVoices)
{
    fs = sampleRate;
    voices.assign ((size_t) maxVoices, Voice());
    for (int i = 0; i <= kSinSize; ++i)
        sinTab[i] = (float) std::sin (kTwoPi * i / kSinSize);
}

void SynthBank::reset() noexcept
{
    for (auto& v : voices)
        v = Voice();
}

int SynthBank::activeVoices() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        if (v.used)
            ++n;
    return n;
}

int SynthBank::snapshot (float* freq, float* amp, int maxVoices) const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        if (v.used && n < maxVoices && v.a > 1.0e-5f)
        {
            freq[n] = v.f;
            amp[n] = v.a;
            ++n;
        }
    return n;
}

void SynthBank::setTargets (const VoiceTarget* t, int n, int rampSamples, int fadeSamples) noexcept
{
    rampSamples = std::max (1, rampSamples);
    fadeSamples = std::max (1, fadeSamples);
    for (auto& v : voices)
        v.seen = false;

    for (int i = 0; i < n; ++i)
    {
        const VoiceTarget& tg = t[i];
        Voice* match = nullptr;
        for (auto& v : voices)
            if (v.used && v.key == tg.key)
            {
                match = &v;
                break;
            }
        if (match == nullptr)
        {
            for (auto& v : voices)
                if (! v.used)
                {
                    match = &v;
                    v = Voice();
                    v.used = true;
                    v.key = tg.key;
                    v.f = tg.freq;
                    v.phase = 0.0;
                    break;
                }
        }
        if (match == nullptr)
            continue;

        Voice& v = *match;
        v.seen = true;
        v.fTarget = tg.freq;
        v.aTarget = tg.amp;
        v.fRemain = rampSamples;
        v.fStep = (v.fTarget - v.f) / (float) rampSamples;
        v.aRemain = rampSamples;
        v.aStep = (v.aTarget - v.a) / (float) rampSamples;
    }

    for (auto& v : voices)
    {
        if (v.used && ! v.seen)
        {
            v.aTarget = 0.0f;
            v.aRemain = fadeSamples;
            v.aStep = (0.0f - v.a) / (float) fadeSamples;
            v.fRemain = 0;
            v.fStep = 0.0f;
            v.key = -1 - (int) (&v - voices.data()); // orphaned: cannot be matched again
        }
    }
}

void SynthBank::render (float* out, int numSamples) noexcept
{
    const double inv = 1.0 / fs;
    for (auto& v : voices)
    {
        if (! v.used)
            continue;
        for (int i = 0; i < numSamples; ++i)
        {
            if (v.fRemain > 0) { v.f += v.fStep; if (--v.fRemain == 0) v.f = v.fTarget; }
            if (v.aRemain > 0) { v.a += v.aStep; if (--v.aRemain == 0) v.a = v.aTarget; }
            const double p = v.phase * kSinSize;
            const int i0 = (int) p;
            const float fr = (float) (p - i0);
            const float s = sinTab[i0] + fr * (sinTab[i0 + 1] - sinTab[i0]);
            out[i] += v.a * s;
            v.phase += (double) v.f * inv;
            if (v.phase >= 1.0)
                v.phase -= std::floor (v.phase);
        }
        if (v.aRemain == 0 && v.aTarget == 0.0f && v.key < 0)
            v.used = false;
    }
}

} // namespace fr
