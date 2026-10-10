// Friction - JUCE audio processor
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Advisor.h"
#include "FrictionEngine.h"
#include "Parameters.h"

#include <memory>
#include <mutex>

namespace friction
{

/**
    Background worker for the Tuning Advisor. Drains the engine's lock-free FIFO, accumulates
    friction curves and publishes results for the GUI (mutex shared only with the GUI thread).
*/
class AdvisorWorker : private juce::Thread
{
public:
    explicit AdvisorWorker (fr::SpscFifo<fr::AdvisorFrame>& fifo);
    ~AdvisorWorker() override;

    void start();
    void stop();

    void setLearning (bool shouldLearn) noexcept { learning.store (shouldLearn); }
    bool isLearning() const noexcept { return learning.load(); }

    /** Latest (interim or final) result; seconds learned so far. GUI thread. */
    fr::AdvisorResult getResult (double& secondsLearned, bool& finalResult) const;
    bool hasResult() const noexcept { return hasAny.load(); }
    void restoreResult (const fr::AdvisorResult& r);

private:
    void run() override;

    fr::SpscFifo<fr::AdvisorFrame>& queue;
    fr::Advisor advisor;
    std::atomic<bool> learning { false };
    std::atomic<bool> hasAny { false };
    bool dirty = false;

    mutable std::mutex resultLock;
    fr::AdvisorResult result;
    double resultSeconds = 0.0;
    bool resultFinal = false;
};

class FrictionAudioProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    FrictionAudioProcessor();
    ~FrictionAudioProcessor() override;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Friction"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    // Friction API
    juce::AudioProcessorValueTreeState& state() noexcept { return apvts; }
    fr::FrictionEngine& engine() noexcept { return *eng; }
    AdvisorWorker& advisor() noexcept { return *advisorWorker; }

    void setLearning (bool shouldLearn);
    bool isLearning() const noexcept { return learnFlag.load(); }
    /** Sets Retune -> Whole Fixed to the Advisor's best offset (Amount 100 %). Message thread. */
    bool applyAdvisorOffset (bool useLowBandCurve);

    /** Applies any pending latency change to the host (message thread; also called by the timer). */
    void syncLatency();

    int editorWidth = 1080, editorHeight = 700;
    std::atomic<float> cpuLoad { 0.0f };

private:
    void timerCallback() override;

    juce::AudioProcessorValueTreeState apvts;
    ParamPointers params;
    std::unique_ptr<fr::FrictionEngine> eng;
    std::unique_ptr<AdvisorWorker> advisorWorker;
    std::atomic<bool> learnFlag { false };
    std::atomic<int> pendingLatency { -1 };
    int currentProgram = 0;
    double lastSampleRate = 48000.0;
    int lastBlockSize = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FrictionAudioProcessor)
};

} // namespace friction
