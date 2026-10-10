// Friction - bank of tracking TPT state-variable bell (peaking) filters
#pragma once

#include "Common.h"

#include <vector>

namespace fr
{

/**
    Cascade of up to N bell filters (Simper / Cytomic TPT SVF form), linked across channels.
    Each slot follows a target (frequency, gain dB, bandwidth Hz). Parameters are de-zippered with
    a 3 ms one-pole at control-tick rate and coefficients are recomputed every tick.
    With 0 dB gain a slot is an exact identity (m1 = k (A^2 - 1) = 0), and fully settled 0 dB
    slots are skipped entirely.
*/
class BellBank
{
public:
    void prepare (double sampleRate, int numSlots, int numChannels);
    void reset() noexcept;

    void setTarget (int slot, float freqHz, float gainDb, float bwHz) noexcept;
    /** Snap a slot to its target immediately (no de-zipper). */
    void snap (int slot) noexcept;

    /** Called every kControlTick samples, before process(). */
    void tick() noexcept;

    /** Processes up to one control tick of audio in place. */
    void process (float* const* io, int numCh, int start, int numSamples) noexcept;

    bool anyActive() const noexcept { return numActive > 0; }
    int numSlots() const noexcept { return (int) slots.size(); }
    float currentGainDb (int slot) const noexcept { return slots[(size_t) slot].curG; }
    float currentFreq (int slot) const noexcept { return slots[(size_t) slot].curF; }

private:
    struct Slot
    {
        float tgtF = 1000.0f, tgtG = 0.0f, tgtBw = 50.0f;
        float curF = 1000.0f, curG = 0.0f, curBw = 50.0f;
        bool running = false;
        double a1 = 0, a2 = 0, a3 = 0, m1 = 0;
        double ic1[kMaxChannels] {}, ic2[kMaxChannels] {};
    };

    void computeCoeffs (Slot& s) noexcept;

    double fs = 48000.0;
    int nCh = 2;
    float alpha = 0.3f;
    int numActive = 0;
    std::vector<Slot> slots;
};

} // namespace fr
