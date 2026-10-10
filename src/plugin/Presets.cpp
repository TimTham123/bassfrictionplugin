#include "Presets.h"
#include "Parameters.h"

namespace friction
{

const std::vector<FactoryPreset>& factoryPresets()
{
    using namespace ids;
    // choice indices: mode 0 Smooth 1 Retune 2 Tension; latency 0 Mix 1 Track; reference 0 Sidechain 1 Self;
    // focus 0 Low 1 High 2 Both; retuneStyle 0 Per-partial 1 Whole Auto 2 Whole Fixed; tensionStyle 0 Synth 1 Push
    static const std::vector<FactoryPreset> presets = {
        { "Init", {} },
        { "Kick <> Bass", { { mode, 0 }, { focus, 0 }, { crossover, 160 }, { amount, 70 }, { sensitivity, 70 },
                            { attack, 5 }, { release, 120 }, { rangeLow, 25 }, { rangeHigh, 400 } } },
        { "808 <> Bass", { { mode, 0 }, { focus, 0 }, { crossover, 200 }, { amount, 80 }, { sensitivity, 75 },
                           { attack, 10 }, { release, 250 }, { rangeLow, 25 }, { rangeHigh, 300 } } },
        { "Bass Retune (Whole Auto)", { { mode, 1 }, { retuneStyle, 1 }, { maxCents, 50 }, { focus, 0 }, { amount, 100 },
                                        { attack, 40 }, { release, 300 } } },
        { "Kick Tuning (use Learn + Apply)", { { mode, 1 }, { retuneStyle, 2 }, { wholeOffset, 0 }, { amount, 100 }, { focus, 0 },
                                               { crossover, 160 } } },
        { "Vocal <> Pad", { { mode, 0 }, { focus, 1 }, { rangeLow, 150 }, { rangeHigh, 8000 }, { amount, 50 },
                            { sensitivity, 60 }, { attack, 30 }, { release, 250 } } },
        { "Double Vocal Smoother", { { mode, 0 }, { focus, 1 }, { rangeLow, 120 }, { rangeHigh, 10000 }, { amount, 60 },
                                     { sensitivity, 75 }, { attack, 15 }, { release, 200 } } },
        { "Piano <> Synth", { { mode, 0 }, { focus, 2 }, { amount, 60 }, { sensitivity, 65 }, { attack, 20 }, { release, 180 } } },
        { "Guitar <> Keys", { { mode, 0 }, { focus, 1 }, { rangeLow, 80 }, { rangeHigh, 6000 }, { amount, 55 }, { sensitivity, 60 } } },
        { "Piano Retune (Per-partial)", { { mode, 1 }, { retuneStyle, 0 }, { maxCents, 30 }, { amount, 80 }, { attack, 30 }, { release, 300 } } },
        { "Detuned Stack Cleaner (Self)", { { mode, 0 }, { reference, 1 }, { amount, 50 }, { sensitivity, 70 } } },
        { "Tension Pad (Slow Wobble)", { { mode, 2 }, { tensionStyle, 0 }, { beatRate, 15 }, { amount, 50 }, { mix, 70 },
                                         { attack, 80 }, { release, 400 } } },
        { "Tension Grit", { { mode, 2 }, { tensionStyle, 0 }, { beatRate, 85 }, { amount, 60 }, { mix, 60 } } },
        { "Tension Push Lead", { { mode, 2 }, { tensionStyle, 1 }, { beatRate, 50 }, { maxCents, 40 }, { amount, 70 } } },
        { "Live Smooth (Track)", { { latency, 1 }, { mode, 0 }, { amount, 50 }, { sensitivity, 65 }, { attack, 10 }, { release, 120 } } },
    };
    return presets;
}

void applyPreset (juce::AudioProcessorValueTreeState& state, const FactoryPreset& preset)
{
    for (auto* p : state.processor.getParameters())
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            if (rp->getParameterID() == ids::bypass)
                continue; // presets never bypass
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (rp->getDefaultValue());
            rp->endChangeGesture();
        }
    }
    for (const auto& [id, value] : preset.values)
        setParamPlain (state, id, value);
}

} // namespace friction
