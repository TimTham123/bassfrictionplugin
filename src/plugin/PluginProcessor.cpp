#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"

namespace friction
{

// ============================================================================ AdvisorWorker

AdvisorWorker::AdvisorWorker (fr::SpscFifo<fr::AdvisorFrame>& fifo)
    : juce::Thread ("Friction Advisor"), queue (fifo)
{
}

AdvisorWorker::~AdvisorWorker()
{
    stop();
}

void AdvisorWorker::start()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);
}

void AdvisorWorker::stop()
{
    stopThread (2000);
}

fr::AdvisorResult AdvisorWorker::getResult (double& secondsLearned, bool& finalResult) const
{
    std::lock_guard<std::mutex> g (resultLock);
    secondsLearned = resultSeconds;
    finalResult = resultFinal;
    return result;
}

void AdvisorWorker::restoreResult (const fr::AdvisorResult& r)
{
    std::lock_guard<std::mutex> g (resultLock);
    result = r;
    resultSeconds = r.seconds;
    resultFinal = true;
    hasAny.store (r.valid || r.lowValid);
}

void AdvisorWorker::run()
{
    double lastPublish = 0.0;
    while (! threadShouldExit())
    {
        fr::AdvisorFrame f;
        int n = 0;
        while (queue.pop (f))
        {
            if (f.startOfLearn)
                advisor.reset();
            // when far behind, skip every other frame (statistics stay unbiased)
            if (queue.size() > 256 && (n & 1))
            {
                ++n;
                continue;
            }
            advisor.consume (f);
            dirty = true;
            ++n;
        }

        const bool learningNow = learning.load();
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (dirty && (! learningNow || now - lastPublish > 0.5))
        {
            fr::AdvisorResult r = advisor.finish();
            {
                std::lock_guard<std::mutex> g (resultLock);
                result = r;
                resultSeconds = advisor.secondsConsumed();
                resultFinal = ! learningNow;
            }
            hasAny.store (true);
            lastPublish = now;
            if (! learningNow)
                dirty = false;
        }
        wait (30);
    }
}

// ============================================================================ processor

FrictionAudioProcessor::FrictionAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "FrictionState", createParameterLayout())
{
    params.attach (apvts);
    eng = std::make_unique<fr::FrictionEngine>();
    eng->setParams (params.read());
    eng->prepare (lastSampleRate, lastBlockSize, 2);
    advisorWorker = std::make_unique<AdvisorWorker> (eng->advisorFifo());
    advisorWorker->start();
    setLatencySamples (eng->latencyFor (params.read().latencyMode));
    // forwards latency changes (Mix <-> Track) from the audio thread to the host; 5 Hz keeps the
    // message thread mostly idle, and the engine itself switches immediately with a 10 ms crossfade
    startTimerHz (5);
}

FrictionAudioProcessor::~FrictionAudioProcessor()
{
    stopTimer();
    advisorWorker->stop();
}

bool FrictionAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();
    if (mainIn != mainOut)
        return false;
    if (mainIn != juce::AudioChannelSet::mono() && mainIn != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void FrictionAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    lastSampleRate = sampleRate;
    lastBlockSize = std::max (1, samplesPerBlock);
    const int numCh = juce::jlimit (1, 2, getMainBusNumInputChannels());
    eng->setParams (params.read());
    eng->prepare (sampleRate, lastBlockSize, numCh);
    eng->setParams (params.read());
    setLatencySamples (eng->currentLatency());
    pendingLatency.store (-1);
}

void FrictionAudioProcessor::releaseResources()
{
}

void FrictionAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();

    const int numSamples = buffer.getNumSamples();
    auto mainBuf = getBusBuffer (buffer, true, 0);
    const int numCh = std::min (2, mainBuf.getNumChannels());

    const float* scPtr[2] = { nullptr, nullptr };
    int numSc = 0;
    bool scPresent = false;
    if (getBusCount (true) > 1)
    {
        if (auto* scBus = getBus (true, 1); scBus != nullptr && scBus->isEnabled())
        {
            auto scBuf = getBusBuffer (buffer, true, 1);
            numSc = std::min (2, scBuf.getNumChannels());
            for (int c = 0; c < numSc; ++c)
                scPtr[c] = scBuf.getReadPointer (c);
            scPresent = numSc > 0;
        }
    }

    fr::Params p = params.read();
    p.learn = learnFlag.load (std::memory_order_relaxed);
    eng->setParams (p);

    if (numCh > 0)
    {
        float* io[2] = { mainBuf.getWritePointer (0), numCh > 1 ? mainBuf.getWritePointer (1) : nullptr };
        eng->process (io, numCh, scPresent ? scPtr : nullptr, numSc, numSamples, scPresent);
    }

    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, numSamples);

    const int L = eng->currentLatency();
    if (L != getLatencySamples())
        pendingLatency.store (L, std::memory_order_relaxed);

    const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const double avail = numSamples / std::max (1.0, getSampleRate());
    const float load = (float) (secs / std::max (1.0e-9, avail));
    cpuLoad.store (0.9f * cpuLoad.load (std::memory_order_relaxed) + 0.1f * load, std::memory_order_relaxed);
}

void FrictionAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // keep the reported latency: run the engine with bypass forced on (latency-compensated dry)
    juce::ScopedNoDenormals noDenormals;
    auto mainBuf = getBusBuffer (buffer, true, 0);
    const int numCh = std::min (2, mainBuf.getNumChannels());
    fr::Params p = params.read();
    p.bypass = true;
    p.learn = false;
    eng->setParams (p);
    if (numCh > 0)
    {
        float* io[2] = { mainBuf.getWritePointer (0), numCh > 1 ? mainBuf.getWritePointer (1) : nullptr };
        eng->process (io, numCh, nullptr, 0, buffer.getNumSamples(), false);
    }
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* FrictionAudioProcessor::createEditor()
{
    return new FrictionAudioProcessorEditor (*this);
}

juce::AudioProcessorParameter* FrictionAudioProcessor::getBypassParameter() const
{
    return apvts.getParameter (ids::bypass);
}

int FrictionAudioProcessor::getNumPrograms()
{
    return (int) factoryPresets().size();
}

void FrictionAudioProcessor::setCurrentProgram (int index)
{
    const auto& presets = factoryPresets();
    if (index < 0 || index >= (int) presets.size())
        return;
    currentProgram = index;
    applyPreset (apvts, presets[(size_t) index]);
}

const juce::String FrictionAudioProcessor::getProgramName (int index)
{
    const auto& presets = factoryPresets();
    return index >= 0 && index < (int) presets.size() ? juce::String (presets[(size_t) index].name) : juce::String();
}

void FrictionAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto st = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (st.createXml());
    xml->setAttribute ("editorW", editorWidth);
    xml->setAttribute ("editorH", editorHeight);
    xml->setAttribute ("program", currentProgram);
    if (advisorWorker->hasResult())
    {
        double secs = 0.0;
        bool fin = false;
        const auto r = advisorWorker->getResult (secs, fin);
        auto* a = xml->createNewChildElement ("Advisor");
        a->setAttribute ("valid", r.valid ? 1 : 0);
        a->setAttribute ("best", r.bestCents);
        a->setAttribute ("reduction", r.reductionPct);
        a->setAttribute ("lowValid", r.lowValid ? 1 : 0);
        a->setAttribute ("lowBest", r.lowBestCents);
        a->setAttribute ("lowReduction", r.lowReductionPct);
        a->setAttribute ("kick", r.kickDetected ? 1 : 0);
        a->setAttribute ("kickF0", r.kickF0);
        a->setAttribute ("kickNewF0", r.kickNewF0);
        a->setAttribute ("seconds", r.seconds);
    }
    copyXmlToBinary (*xml, destData);
}

void FrictionAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    editorWidth = juce::jlimit (860, 2400, xml->getIntAttribute ("editorW", editorWidth));
    editorHeight = juce::jlimit (560, 1600, xml->getIntAttribute ("editorH", editorHeight));
    currentProgram = xml->getIntAttribute ("program", 0);
    if (auto* a = xml->getChildByName ("Advisor"))
    {
        fr::AdvisorResult r;
        r.valid = a->getIntAttribute ("valid") != 0;
        r.bestCents = (float) a->getDoubleAttribute ("best");
        r.reductionPct = (float) a->getDoubleAttribute ("reduction");
        r.lowValid = a->getIntAttribute ("lowValid") != 0;
        r.lowBestCents = (float) a->getDoubleAttribute ("lowBest");
        r.lowReductionPct = (float) a->getDoubleAttribute ("lowReduction");
        r.kickDetected = a->getIntAttribute ("kick") != 0;
        r.kickF0 = (float) a->getDoubleAttribute ("kickF0");
        r.kickNewF0 = (float) a->getDoubleAttribute ("kickNewF0");
        r.seconds = a->getDoubleAttribute ("seconds");
        advisorWorker->restoreResult (r);
        xml->removeChildElement (a, true);
    }
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

void FrictionAudioProcessor::setLearning (bool shouldLearn)
{
    learnFlag.store (shouldLearn);
    advisorWorker->setLearning (shouldLearn);
}

bool FrictionAudioProcessor::applyAdvisorOffset (bool useLowBandCurve)
{
    double secs = 0.0;
    bool fin = false;
    const auto r = advisorWorker->getResult (secs, fin);
    const bool ok = useLowBandCurve ? r.lowValid : r.valid;
    if (! ok)
        return false;
    const float cents = useLowBandCurve ? r.lowBestCents : r.bestCents;
    setParamPlain (apvts, ids::mode, 1.0f);
    setParamPlain (apvts, ids::retuneStyle, 2.0f);
    setParamPlain (apvts, ids::wholeOffset, cents);
    setParamPlain (apvts, ids::amount, 100.0f);
    return true;
}

void FrictionAudioProcessor::syncLatency()
{
    const int L = pendingLatency.exchange (-1);
    if (L >= 0 && L != getLatencySamples())
        setLatencySamples (L);
}

void FrictionAudioProcessor::timerCallback()
{
    syncLatency();
}

} // namespace friction

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new friction::FrictionAudioProcessor();
}
