// Friction - factory presets
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <utility>
#include <vector>

namespace friction
{

struct FactoryPreset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values; // plain values; unspecified parameters use defaults
};

const std::vector<FactoryPreset>& factoryPresets();

/** Resets every parameter to its default, then applies the preset. Message thread. */
void applyPreset (juce::AudioProcessorValueTreeState& state, const FactoryPreset& preset);

} // namespace friction
