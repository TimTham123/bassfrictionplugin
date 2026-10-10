// Friction - oscillator bank for Tension mode (detuned partials that beat against the reference)
#pragma once

#include "Common.h"

#include <vector>

namespace fr
{

struct VoiceTarget
{
    int key = -1;        // identity of the reference partial being followed
    float freq = 0.0f;   // Hz
    float amp = 0.0f;    // linear peak amplitude
};

/**
    Phase-continuous sine voices keyed by reference partial identity. Frequencies and amplitudes
    ramp linearly to new targets; voices whose key disappears fade out and are recycled.
*/
class SynthBank
{
public:
    void prepare (double sampleRate, int maxVoices);
    void reset() noexcept;

    /** New targets; ramps take rampSamples, voice fade-outs take fadeSamples. */
    void setTargets (const VoiceTarget* t, int n, int rampSamples, int fadeSamples) noexcept;

    /** Adds the mono synthesis to out[0..numSamples). */
    void render (float* out, int numSamples) noexcept;

    int activeVoices() const noexcept;
    /** Copies current voice frequencies/amplitudes (for display). Returns the count. */
    int snapshot (float* freq, float* amp, int maxVoices) const noexcept;
    bool isSilent() const noexcept { return activeVoices() == 0; }

private:
    struct Voice
    {
        bool used = false;
        int key = -1;
        double phase = 0.0;
        float f = 0.0f, fStep = 0.0f;
        float a = 0.0f, aStep = 0.0f;
        int fRemain = 0, aRemain = 0;
        float fTarget = 0.0f, aTarget = 0.0f;
        bool seen = false;
    };

    double fs = 48000.0;
    std::vector<Voice> voices;
    static constexpr int kSinSize = 4096;
    float sinTab[kSinSize + 1] {};
};

} // namespace fr
