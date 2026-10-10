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
