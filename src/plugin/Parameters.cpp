#include "Parameters.h"

namespace friction
{

namespace
{
    juce::NormalisableRange<float> logRange (float lo, float hi, float step = 0.0f)
    {
        juce::NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    juce::String hzText (float v, int)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
    }
    float hzValue (const juce::String& s)
    {
        const auto t = s.trim().toLowerCase();
        const float v = t.getFloatValue();
        return t.contains ("k") ? v * 1000.0f : v;
    }
    juce::String pctText (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }
    juce::String msText (float v, int) { return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; }
    juce::String dbText (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
    juce::String centsText (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, std::abs (v) < 100.0f ? 1 : 0) + " ct"; }
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto pid = [] (const char* id) { return ParameterID { id, kParamVersion }; };

    layout.add (std::make_unique<AudioParameterChoice> (pid (ids::mode), "Mode", StringArray { "Smooth", "Retune", "Tension" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (ids::latency), "Latency Mode", StringArray { "Mix", "Track" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (pid (ids::reference), "Reference", StringArray { "Sidechain", "Self" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::amount), "Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pctText)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::sensitivity), "Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 65.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pctText)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::attack), "Attack", logRange (1.0f, 500.0f, 0.1f), 20.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (msText)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::release), "Release", logRange (10.0f, 2000.0f, 0.1f), 150.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (msText)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::mix), "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pctText)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::trim), "Output Trim", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::rangeLow), "Range Low", logRange (20.0f, 20000.0f, 0.1f), 20.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText).withValueFromStringFunction (hzValue)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::rangeHigh), "Range High", logRange (20.0f, 20000.0f, 0.1f), 20000.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText).withValueFromStringFunction (hzValue)));
    layout.add (std::make_unique<AudioParameterChoice> (pid (ids::focus), "Focus", StringArray { "Low", "High", "Both" }, 2));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::crossover), "Crossover", logRange (80.0f, 400.0f, 0.1f), 200.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText).withValueFromStringFunction (hzValue)));
    layout.add (std::make_unique<AudioParameterBool> (pid (ids::delta), "Delta", false));
    layout.add (std::make_unique<AudioParameterBool> (pid (ids::bypass), "Bypass", false));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::maxCents), "Max Cents", NormalisableRange<float> (1.0f, 100.0f, 0.1f), 50.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ct").withStringFromValueFunction (centsText)));
    layout.add (std::make_unique<AudioParameterChoice> (pid (ids::retuneStyle), "Retune Style", StringArray { "Per-partial", "Whole Auto", "Whole Fixed" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::wholeOffset), "Whole Offset", NormalisableRange<float> (-1200.0f, 1200.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ct").withStringFromValueFunction (centsText)));
    layout.add (std::make_unique<AudioParameterFloat> (pid (ids::beatRate), "Beat Rate", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 40.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pctText)));
    layout.add (std::make_unique<AudioParameterChoice> (pid (ids::tensionStyle), "Tension Style", StringArray { "Synth", "Push" }, 0));
    return layout;
}

void ParamPointers::attach (juce::AudioProcessorValueTreeState& s)
{
    auto get = [&] (const char* id) { auto* p = s.getRawParameterValue (id); jassert (p != nullptr); return p; };
    mode = get (ids::mode);
    latency = get (ids::latency);
    reference = get (ids::reference);
    amount = get (ids::amount);
    sensitivity = get (ids::sensitivity);
    attack = get (ids::attack);
    release = get (ids::release);
    mix = get (ids::mix);
    trim = get (ids::trim);
    rangeLow = get (ids::rangeLow);
    rangeHigh = get (ids::rangeHigh);
    focus = get (ids::focus);
    crossover = get (ids::crossover);
    delta = get (ids::delta);
    bypass = get (ids::bypass);
    maxCents = get (ids::maxCents);
    retuneStyle = get (ids::retuneStyle);
    wholeOffset = get (ids::wholeOffset);
    beatRate = get (ids::beatRate);
    tensionStyle = get (ids::tensionStyle);
}

fr::Params ParamPointers::read() const noexcept
{
    auto ch = [] (std::atomic<float>* p, int maxIdx) { return juce::jlimit (0, maxIdx, juce::roundToInt (p->load (std::memory_order_relaxed))); };
    auto f = [] (std::atomic<float>* p) { return p->load (std::memory_order_relaxed); };
    fr::Params p;
    p.mode = (fr::Mode) ch (mode, 2);
    p.latencyMode = (fr::LatencyMode) ch (latency, 1);
    p.reference = (fr::ReferenceSource) ch (reference, 1);
    p.amount = f (amount) * 0.01f;
    p.sensitivity = f (sensitivity) * 0.01f;
    p.attackMs = f (attack);
    p.releaseMs = f (release);
    p.mix = f (mix) * 0.01f;
    p.trimDb = f (trim);
    p.rangeLowHz = f (rangeLow);
    p.rangeHighHz = f (rangeHigh);
    p.focus = (fr::Focus) ch (focus, 2);
    p.crossoverHz = f (crossover);
    p.delta = f (delta) > 0.5f;
    p.bypass = f (bypass) > 0.5f;
    p.maxCents = f (maxCents);
    p.retuneStyle = (fr::RetuneStyle) ch (retuneStyle, 2);
    p.wholeOffsetCents = f (wholeOffset);
    p.beatRate = f (beatRate) * 0.01f;
    p.tensionStyle = (fr::TensionStyle) ch (tensionStyle, 1);
    return p;
}

void setParamPlain (juce::AudioProcessorValueTreeState& s, const char* id, float plainValue)
{
    if (auto* p = s.getParameter (id))
    {
        const float norm = p->convertTo0to1 (plainValue);
        p->beginChangeGesture();
        p->setValueNotifyingHost (norm);
        p->endChangeGesture();
    }
}

} // namespace friction
