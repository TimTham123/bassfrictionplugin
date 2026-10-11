// Friction - parameter definitions (APVTS) and mapping to the engine
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Common.h"

namespace friction
{

namespace ids
{
    inline constexpr const char* mode = "mode";
    inline constexpr const char* latency = "latency";
    inline constexpr const char* reference = "reference";
    inline constexpr const char* amount = "amount";
    inline constexpr const char* sensitivity = "sensitivity";
    inline constexpr const char* attack = "attack";
    inline constexpr const char* release = "release";
    inline constexpr const char* mix = "mix";
    inline constexpr const char* trim = "trim";
    inline constexpr const char* rangeLow = "rangeLow";
    inline constexpr const char* rangeHigh = "rangeHigh";
    inline constexpr const char* focus = "focus";
    inline constexpr const char* crossover = "crossover";
    inline constexpr const char* delta = "delta";
    inline constexpr const char* bypass = "bypass";
    inline constexpr const char* maxCents = "maxCents";
    inline constexpr const char* retuneStyle = "retuneStyle";
    inline constexpr const char* wholeOffset = "wholeOffset";
    inline constexpr const char* beatRate = "beatRate";
    inline constexpr const char* tensionStyle = "tensionStyle";
}

constexpr int kParamVersion = 1;

/**
    On/off parameter that stores only 0 or 1.
    juce::AudioParameterBool keeps whatever normalised value a host sends (0.23 means "off"), while
    AudioProcessorValueTreeState compares snapped values. Restoring a saved "off" over 0.44 then
    changes nothing and the parameter reports 0.44 instead of the saved value (found by pluginval).
*/
class BoolParameter final : public juce::RangedAudioParameter
{
public:
    BoolParameter (const juce::ParameterID& pid, const juce::String& paramName, bool defaultValue)
        : RangedAudioParameter (pid, paramName), value (defaultValue ? 1.0f : 0.0f), defaultNorm (defaultValue ? 1.0f : 0.0f)
    {
    }

    bool get() const noexcept { return value.load (std::memory_order_relaxed) >= 0.5f; }
    const juce::NormalisableRange<float>& getNormalisableRange() const override { return range; }

private:
    float getValue() const override { return value.load (std::memory_order_relaxed); }
    void setValue (float v) override { value.store (v >= 0.5f ? 1.0f : 0.0f, std::memory_order_relaxed); }
    float getDefaultValue() const override { return defaultNorm; }
    int getNumSteps() const override { return 2; }
    bool isDiscrete() const override { return true; }
    bool isBoolean() const override { return true; }
    juce::String getText (float v, int) const override { return v >= 0.5f ? "On" : "Off"; }
    float getValueForText (const juce::String& text) const override
    {
        const auto t = text.trim().toLowerCase();
        return (t == "on" || t == "yes" || t == "true" || t.getIntValue() != 0) ? 1.0f : 0.0f;
    }

    std::atomic<float> value;
    const float defaultNorm;
    const juce::NormalisableRange<float> range { 0.0f, 1.0f, 1.0f };
};

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Cached raw parameter pointers for lock-free reads on the audio thread. */
struct ParamPointers
{
    void attach (juce::AudioProcessorValueTreeState& s);
    fr::Params read() const noexcept;

    std::atomic<float>* mode = nullptr;
    std::atomic<float>* latency = nullptr;
    std::atomic<float>* reference = nullptr;
    std::atomic<float>* amount = nullptr;
    std::atomic<float>* sensitivity = nullptr;
    std::atomic<float>* attack = nullptr;
    std::atomic<float>* release = nullptr;
    std::atomic<float>* mix = nullptr;
    std::atomic<float>* trim = nullptr;
    std::atomic<float>* rangeLow = nullptr;
    std::atomic<float>* rangeHigh = nullptr;
    std::atomic<float>* focus = nullptr;
    std::atomic<float>* crossover = nullptr;
    std::atomic<float>* delta = nullptr;
    std::atomic<float>* bypass = nullptr;
    std::atomic<float>* maxCents = nullptr;
    std::atomic<float>* retuneStyle = nullptr;
    std::atomic<float>* wholeOffset = nullptr;
    std::atomic<float>* beatRate = nullptr;
    std::atomic<float>* tensionStyle = nullptr;
};

/** Sets a parameter to a plain (de-normalised) value and notifies the host. Message thread. */
void setParamPlain (juce::AudioProcessorValueTreeState& s, const char* id, float plainValue);

} // namespace friction
