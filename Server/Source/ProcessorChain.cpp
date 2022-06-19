/*
 * Copyright (c) 2020 Andreas Pohl
 * Licensed under MIT (https://github.com/apohl79/audiogridder/blob/master/COPYING)
 *
 * Author: Andreas Pohl
 */

#include "ProcessorChain.hpp"
#include "Processor.hpp"
#include "App.hpp"

namespace e47 {

void ProcessorChain::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    traceScope();
    setRateAndBufferSizeDetails(sampleRate, maximumExpectedSamplesPerBlock);
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    for (auto& proc : m_processors) {
        proc->prepareToPlay(sampleRate, maximumExpectedSamplesPerBlock);
    }
}

void ProcessorChain::releaseResources() {
    traceScope();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    for (auto& proc : m_processors) {
        proc->releaseResources();
    }
}

void ProcessorChain::setPlayHead(AudioPlayHead* ph) {
    AudioProcessor::setPlayHead(ph);
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    for (auto& proc : m_processors) {
        proc->setPlayHead(ph);
    }
}

void ProcessorChain::processBlock(AudioBuffer<float>& buffer, MidiBuffer& midiMessages) {
    processBlockInternal(buffer, midiMessages);
}

void ProcessorChain::processBlock(AudioBuffer<double>& buffer, MidiBuffer& midiMessages) {
    processBlockInternal(buffer, midiMessages);
}

double ProcessorChain::getTailLengthSeconds() const { return m_tailSecs; }

bool ProcessorChain::supportsDoublePrecisionProcessing() const { return m_supportsDoublePrecision; }

bool ProcessorChain::updateChannels(int channelsIn, int channelsOut, int channelsSC) {
    traceScope();
    AudioProcessor::BusesLayout layout;
    if (channelsIn == 1) {
        layout.inputBuses.add(AudioChannelSet::mono());
    } else if (channelsIn == 2) {
        layout.inputBuses.add(AudioChannelSet::stereo());
    } else if (channelsIn > 0) {
        layout.inputBuses.add(AudioChannelSet::discreteChannels(channelsIn));
    }
    if (channelsSC == 1) {
        layout.inputBuses.add(AudioChannelSet::mono());
    } else if (channelsSC == 2) {
        layout.inputBuses.add(AudioChannelSet::stereo());
    } else if (channelsSC > 0) {
        layout.inputBuses.add(AudioChannelSet::discreteChannels(channelsIn));
    }
    if (channelsOut == 1) {
        layout.outputBuses.add(AudioChannelSet::mono());
    } else if (channelsOut == 2) {
        layout.outputBuses.add(AudioChannelSet::stereo());
    } else if (channelsOut > 0) {
        layout.outputBuses.add(AudioChannelSet::discreteChannels(channelsOut));
    }
    logln("setting chain layout to: " << describeLayout(layout));
    if (!setBusesLayout(layout)) {
        logln("failed to set layout");
    }
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    m_extraChannels = 0;
    m_hasSidechain = channelsSC > 0;
    m_sidechainDisabled = false;
    for (auto& proc : m_processors) {
        setProcessorBusesLayout(proc.get(), proc->getLayout());
    }
    return true;
}

bool ProcessorChain::setProcessorBusesLayout(Processor* proc, const String& targetOutputLayout) {
    traceScope();

    if (!proc->isLoaded()) {
        return false;
    }

    auto layout = getBusesLayout();
    int chIn = getLayoutNumChannels(layout, true), chOut = getLayoutNumChannels(layout, false);
    auto procLayouts = proc->getSupportedBusLayouts();
    AudioProcessor::BusesLayout targetLayout;
    int targetChIn = 0, targetChOut = 0, extraInChannels = 0, extraOutChannels = 0;
    bool found = false;

    if (procLayouts.isEmpty()) {
        logln("no processor layouts cached, checking now...");
        procLayouts = Processor::findSupportedLayouts(proc);
    }

    if (targetOutputLayout.isNotEmpty() && targetOutputLayout != "Default") {
        for (auto& l : procLayouts) {
            int checkChIn = getLayoutNumChannels(l, true), checkChOut = getLayoutNumChannels(l, false);
            auto soutputs = describeLayout(l, false, true, true);
            if (soutputs == targetOutputLayout) {
                if (chIn == 0 || checkChIn == checkChOut) {
                    targetLayout = l;
                    targetChIn = checkChIn;
                    targetChOut = checkChOut;
                    found = true;
                    break;
                } else {
                    if (checkChIn > targetChIn) {
                        targetLayout = l;
                        targetChIn = checkChIn;
                        targetChOut = checkChOut;
                        found = true;
                    }
                }
            }
        }
    } else {
        if (procLayouts.contains(layout)) {
            targetLayout = layout;
            targetChIn = chIn;
            targetChOut = chOut;
            found = true;
        } else {
            // try to find a layout with a matching number of out/in channels
            for (auto& l : procLayouts) {
                int checkChIn = getLayoutNumChannels(l, true), checkChOut = getLayoutNumChannels(l, false);
                if (checkChOut == chOut && (chIn == 0 || checkChIn > targetChIn)) {
                    targetLayout = l;
                    targetChIn = checkChIn;
                    targetChOut = checkChOut;
                    found = true;
                }
            }

            if (!found) {
                // try to find the layout with the highest number of output channels followed by input channels
                for (auto& l : procLayouts) {
                    int checkChIn = getLayoutNumChannels(l, true), checkChOut = getLayoutNumChannels(l, false);
                    if (checkChOut > targetChOut || (checkChOut == targetChOut && checkChIn > targetChIn)) {
                        targetLayout = l;
                        targetChIn = checkChIn;
                        targetChOut = checkChOut;
                        found = true;
                    }
                }
            }
        }

        if (!found || !proc->setBusesLayout(targetLayout)) {
            logln("failed to set target layout, falling back to the current processors layout");
            targetLayout = proc->getBusesLayout();
            targetChIn = getLayoutNumChannels(targetLayout, true);
            targetChOut = getLayoutNumChannels(targetLayout, false);
        }
    }

    if (found) {
        extraInChannels = targetChIn - chIn;
        extraOutChannels = targetChOut - chOut;

        proc->setExtraChannels(extraInChannels, extraOutChannels);

        m_extraChannels = jmax(m_extraChannels, extraInChannels, extraOutChannels);

        logln(extraInChannels << " extra input(s), " << extraOutChannels << " extra output(s) -> " << m_extraChannels
                              << " extra channel(s) in total");

        logln("setting processor to I/O layout: " << describeLayout(targetLayout));
    } else {
        logln("no matching I/O layout found, targetOutputLayout=" << targetOutputLayout);
    }

    return found;
}

int ProcessorChain::getExtraChannels() {
    traceScope();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    return m_extraChannels;
}

bool ProcessorChain::initPluginInstance(Processor* proc, const String& layout, String& err) {
    traceScope();
    if (!setProcessorBusesLayout(proc, layout)) {
        err = "failed to find a working I/O configuration";
        return false;
    }
    AudioProcessor::ProcessingPrecision prec = AudioProcessor::singlePrecision;
    if (isUsingDoublePrecision() && supportsDoublePrecisionProcessing()) {
        if (proc->supportsDoublePrecisionProcessing()) {
            prec = AudioProcessor::doublePrecision;
        } else {
            logln("host wants double precission but plugin '" << proc->getName() << "' does not support it");
        }
    }
    proc->setProcessingPrecision(prec);
    proc->setPlayHead(getPlayHead());
    proc->prepareToPlay(getSampleRate(), getBlockSize());
    proc->enableAllBuses();
    if (prec == AudioProcessor::doublePrecision) {
        preProcessBlocks<double>(proc);
    } else {
        preProcessBlocks<float>(proc);
    }
    return true;
}

bool ProcessorChain::addPluginProcessor(const String& id, const String& settings, const String& layout, bool multiMono,
                                        uint64 monoChannels, String& err) {
    traceScope();

    bool success = false;

    if (multiMono) {
        err = "Multi-Mono layout not yet implemented";
    } else {
        auto proc = std::make_shared<Processor>(*this, id, getSampleRate(), getBlockSize());
        success = proc->load(settings, layout, multiMono, monoChannels, err);
        addProcessor(std::move(proc));
    }

    return success;
}

void ProcessorChain::addProcessor(std::shared_ptr<Processor> processor) {
    traceScope();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    processor->setChainIndex((int)m_processors.size());
    m_processors.push_back(processor);
    updateNoLock();
}

void ProcessorChain::delProcessor(int idx) {
    traceScope();
    int i = 0;
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    for (auto it = m_processors.begin(); it < m_processors.end(); it++) {
        if (i++ == idx) {
            (*it)->unload();
            m_processors.erase(it);
            break;
        }
    }
    updateNoLock();
}

void ProcessorChain::update() {
    traceScope();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    updateNoLock();
}

void ProcessorChain::updateNoLock() {
    traceScope();
    int latency = 0;
    bool supportsDouble = true;
    m_extraChannels = 0;
    m_sidechainDisabled = false;
    for (auto& proc : m_processors) {
        if (nullptr != proc) {
            latency += proc->getLatencySamples();
            if (!proc->supportsDoublePrecisionProcessing()) {
                supportsDouble = false;
            }
            m_extraChannels = jmax(m_extraChannels, proc->getExtraInChannels(), proc->getExtraOutChannels());
            m_sidechainDisabled = m_hasSidechain && (m_sidechainDisabled || proc->getNeedsDisabledSidechain());
        }
    }
    if (latency != getLatencySamples()) {
        logln("updating latency samples to " << latency);
        setLatencySamples(latency);
    }
    m_supportsDoublePrecision = supportsDouble;
    auto it = m_processors.rbegin();
    while (it != m_processors.rend() && (*it)->isSuspended()) {
        it++;
    }
    if (it != m_processors.rend()) {
        m_tailSecs = (*it)->getTailLengthSeconds();
    } else {
        m_tailSecs = 0.0;
    }
}

std::shared_ptr<Processor> ProcessorChain::getProcessor(int index) {
    traceScope();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    if (index > -1 && (size_t)index < m_processors.size()) {
        return m_processors[(size_t)index];
    }
    return nullptr;
}

void ProcessorChain::exchangeProcessors(int idxA, int idxB) {
    traceScope();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    if (idxA > -1 && (size_t)idxA < m_processors.size() && idxB > -1 && (size_t)idxB < m_processors.size()) {
        std::swap(m_processors[(size_t)idxA], m_processors[(size_t)idxB]);
        m_processors[(size_t)idxA]->setChainIndex(idxA);
        m_processors[(size_t)idxB]->setChainIndex(idxB);
    }
}

float ProcessorChain::getParameterValue(int idx, int paramIdx) {
    traceScope();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    if (idx > -1 && (size_t)idx < m_processors.size()) {
        if (auto p = m_processors[(size_t)idx]) {
            return p->getParameterValue(paramIdx);
        }
    }
    return 0.0f;
}

void ProcessorChain::clear() {
    traceScope();
    releaseResources();
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    for (auto& proc : m_processors) {
        proc->unload();
    }
    m_processors.clear();
}

String ProcessorChain::toString() {
    traceScope();
    String ret;
    std::lock_guard<std::mutex> lock(m_processorsMtx);
    bool first = true;
    for (auto& proc : m_processors) {
        if (!first) {
            ret << " > ";
        } else {
            first = false;
        }
        if (proc->isSuspended()) {
            ret << "<bypassed>";
        } else {
            ret << proc->getName();
        }
    }
    return ret;
}

template <typename T>
void ProcessorChain::processBlockInternal(AudioBuffer<T>& buffer, MidiBuffer& midiMessages) {
    traceScope();

    int latency = 0;
    if (getBusCount(true) > 1 && m_sidechainDisabled) {
        auto sidechainBuffer = getBusBuffer(buffer, true, 1);
        sidechainBuffer.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_processorsMtx);
        TimeTrace::addTracePoint("chain_lock");
        for (auto& proc : m_processors) {
            TimeTrace::startGroup();
            if (proc->processBlock(buffer, midiMessages)) {
                latency += proc->getLatencySamples();
            }
            TimeTrace::finishGroup("chain_process: " + proc->getName());
        }
    }

    if (latency != getLatencySamples()) {
        logln("updating latency samples to " << latency);
        setLatencySamples(latency);
        TimeTrace::addTracePoint("chain_set_latency");
    }
}

template <typename T>
void ProcessorChain::preProcessBlocks(Processor* proc) {
    traceScope();
    MidiBuffer midi;
    int channels = jmax(getTotalNumInputChannels(), getTotalNumOutputChannels()) + m_extraChannels;
    AudioBuffer<T> buf(channels, getBlockSize());
    buf.clear();
    int samplesProcessed = 0;
    do {
        proc->processBlock(buf, midi);
        samplesProcessed += getBlockSize();
    } while (samplesProcessed < 16384);
}

}  // namespace e47
