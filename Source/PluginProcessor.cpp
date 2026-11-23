#include "PluginProcessor.h"
#include "gui/PluginEditor.h"

namespace lacs
{
namespace
{
uint32_t packOrder (const ModuleOrder& o) noexcept
{
    uint32_t v = 0;
    for (int i = 0; i < kNumOrderable; ++i)
        v |= (uint32_t) (o[(size_t) i] & 7) << (3 * i);
    return v | 0x80000000u;
}

ModuleOrder unpackOrder (uint32_t v) noexcept
{
    if ((v & 0x80000000u) == 0)
        return defaultModuleOrder();
    ModuleOrder o {};
    for (int i = 0; i < kNumOrderable; ++i)
        o[(size_t) i] = (int) ((v >> (3 * i)) & 7);
    return o;
}

juce::String orderString (const ModuleOrder& o)
{
    char buf[kNumOrderable + 1];
    orderToString (o, buf);
    return buf;
}

constexpr const char* kOtherSlotTag = "OTHER_SLOT";
} // namespace

StripProcessor::StripProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "LAChannelStrip", createParameterLayout()),
      params (apvts)
{
    listener.owner = this;
    packedOrder.store (packOrder (defaultModuleOrder()));
}

bool StripProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    const auto& in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    if (in.size() > out.size())
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto& sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void StripProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedRate = sampleRate;
    preparedBlockSize = std::max (32, samplesPerBlock);
    monoSource.store (getMainBusNumInputChannels() == 1);
    params.read (engineParams);
    engineParams.order = unpackOrder (packedOrder.load());
    engineParams.monoSource = monoSource.load();
    engine.setParams (engineParams);
    engine.prepare (preparedRate, preparedBlockSize);
    engineSampleRate.store (preparedRate);
    setLatencySamples (engine.getLatency());
}

Tolerance StripProcessor::getTolerance() const noexcept
{
    const bool analogue = juce::roundToInt (params.get (P::mode)) == 1;
    return analogue ? Tolerance::forChannel (juce::roundToInt (params.get (P::channel))) : Tolerance::ideal();
}

void StripProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();

    auto mainIn = getBusBuffer (buffer, true, 0);
    auto mainOut = getBusBuffer (buffer, false, 0);
    const int numIn = mainIn.getNumChannels();
    const int numOut = mainOut.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numIn == 1 && numOut > 1)
        mainOut.copyFrom (1, 0, mainOut, 0, 0, numSamples); // mono in, stereo out
    const int channels = std::min (2, numOut);

    // Side-chain (only when the host has enabled and connected the bus)
    const float* scPtrs[2] = { nullptr, nullptr };
    bool scActive = false;
    if (getBusCount (true) > 1 && getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled())
    {
        auto sc = getBusBuffer (buffer, true, 1);
        if (sc.getNumChannels() > 0)
        {
            scPtrs[0] = sc.getReadPointer (0);
            scPtrs[1] = sc.getReadPointer (std::min (1, sc.getNumChannels() - 1));
            scActive = true;
        }
    }
    meters.sidechainActive.store (scActive, std::memory_order_relaxed);

    // ---- Assistant ---------------------------------------------------------------------------------
    const int as = assistState.load();
    if (as == 1)
    {
        engine.setStatsEnabled (true);
        assistSamples.fetch_add (numSamples, std::memory_order_relaxed);
        assistSeconds.store ((double) assistSamples.load (std::memory_order_relaxed) / std::max (1.0, preparedRate));
    }
    else
    {
        engine.setStatsEnabled (false); // also covers a finish while no audio was running
        if (as == 2)
            assistState.store (3);
    }

    params.read (engineParams);
    engineParams.order = unpackOrder (packedOrder.load (std::memory_order_relaxed));
    engineParams.monoSource = monoSource.load (std::memory_order_relaxed);
    engine.setParams (engineParams);

    for (int start = 0; start < numSamples; start += preparedBlockSize)
    {
        const int n = std::min (preparedBlockSize, numSamples - start);
        float* ptrs[2] = { mainOut.getWritePointer (0, start), mainOut.getWritePointer (channels > 1 ? 1 : 0, start) };
        const float* sc[2] = { scPtrs[0] != nullptr ? scPtrs[0] + start : nullptr, scPtrs[1] != nullptr ? scPtrs[1] + start : nullptr };
        engine.process (ptrs, channels, n, scActive ? sc : nullptr, &listener);
        spectrumIn.push (engine.getInputAnalysis(), n);
        spectrumOut.push (engine.getOutputAnalysis(), n);
    }

    const auto& m = engine.getMeters();
    auto peakMax = [] (std::atomic<float>& a, float v) { a.store (std::max (a.load (std::memory_order_relaxed), v), std::memory_order_relaxed); };
    peakMax (meters.inPeak, m.inPeak);
    peakMax (meters.outPeak, m.outPeak);
    meters.gate.store (m.gateDb, std::memory_order_relaxed);
    meters.comp.store (m.compDb, std::memory_order_relaxed);
    meters.deess.store (m.deessDb, std::memory_order_relaxed);
    meters.vuIn.store (m.vuInDb, std::memory_order_relaxed);
    meters.vuOut.store (m.vuOutDb, std::memory_order_relaxed);
    meters.transient.store (m.transientDb, std::memory_order_relaxed);
    meters.gateKey.store (m.gateKeyDb, std::memory_order_relaxed);
    meters.compKey.store (m.compKeyDb, std::memory_order_relaxed);
    meters.deessBand.store (m.deessBandDb, std::memory_order_relaxed);
    meters.autoMakeup.store (m.autoMakeupDb, std::memory_order_relaxed);
    meters.gateOpen.store (m.gateOpen, std::memory_order_relaxed);
    meters.eqInSidechain.store (m.eqInSidechain, std::memory_order_relaxed);
}

//==============================================================================
ModuleOrder StripProcessor::getModuleOrder() const noexcept
{
    return unpackOrder (packedOrder.load());
}

void StripProcessor::setModuleOrder (const ModuleOrder& order)
{
    packedOrder.store (packOrder (order));
    apvts.state.setProperty ("order", orderString (order), nullptr);
}

void StripProcessor::setParam (const char* id, float value)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

//==============================================================================
juce::ValueTree StripProcessor::captureState()
{
    auto state = apvts.copyState();
    state.setProperty ("order", orderString (getModuleOrder()), nullptr);
    if (auto other = state.getChildWithName (kOtherSlotTag); other.isValid())
        state.removeChild (other, nullptr);
    return state;
}

void StripProcessor::restoreState (const juce::ValueTree& source)
{
    auto tree = source.createCopy();
    if (auto other = tree.getChildWithName (kOtherSlotTag); other.isValid())
        tree.removeChild (other, nullptr);
    apvts.replaceState (tree);
    setModuleOrder (orderFromString (tree.getProperty ("order").toString().toRawUTF8()));
}

void StripProcessor::selectSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == activeSlot)
        return;
    auto current = captureState();
    if (otherSlot.isValid())
        restoreState (otherSlot);
    otherSlot = current;
    activeSlot = slot;
}

void StripProcessor::copyToOtherSlot()
{
    otherSlot = captureState();
}

//==============================================================================
void StripProcessor::startAssist()
{
    if (assistState.load() != 0)
        return;
    assistSamples.store (0);
    assistSeconds.store (0.0);
    assistState.store (1);
}

void StripProcessor::cancelAssist()
{
    if (assistState.load() == 1)
    {
        assistState.store (2);
        for (int i = 0; i < 40 && assistState.load() != 3; ++i)
            juce::Thread::sleep (5);
    }
    assistState.store (0);
}

juce::String StripProcessor::finishAssist()
{
    if (assistState.load() != 1)
        return {};
    assistState.store (2);
    for (int i = 0; i < 40 && assistState.load() != 3; ++i) // wait for the audio thread to stop gathering
        juce::Thread::sleep (5);
    assistState.store (0);

    const auto r = analyseStats (engine.getStats(), params.get (P::compRatio));
    if (! r.ok)
        return "Assistant: " + juce::String (r.summary) + " - play the track for 5-30 seconds";

    setParam (ParamID::inGain, r.trimDb);
    if (r.gateOn)
    {
        setParam (ParamID::gateOn, 1.0f);
        setParam (ParamID::gateThr, r.gateThresholdDb);
    }
    setParam (ParamID::compOn, 1.0f);
    setParam (ParamID::compThr, r.compThresholdDb);
    if (r.deessOn)
    {
        setParam (ParamID::dsThr, r.deessThresholdDb);
        if (params.get (P::dsOn) < 0.5f && r.deessOn)
            setParam (ParamID::dsOn, 1.0f);
    }
    return "Assistant: " + juce::String (r.summary);
}

//==============================================================================
void StripProcessor::resetToDefaults()
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());
}

void StripProcessor::setCurrentProgram (int index)
{
    const auto& presets = getFactoryPresets();
    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;
    currentProgram = index;
    currentPresetName = presets[(size_t) index].name;
    resetToDefaults();
    for (const auto& [id, value] : presets[(size_t) index].values)
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    setModuleOrder (presets[(size_t) index].order.isEmpty() ? defaultModuleOrder()
                                                           : orderFromString (presets[(size_t) index].order.toRawUTF8()));
}

const juce::String StripProcessor::getProgramName (int index)
{
    const auto& presets = getFactoryPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? presets[(size_t) index].name : juce::String();
}

juce::File StripProcessor::getUserPresetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("LA Audio").getChildFile ("LA-ChannelStrip").getChildFile ("Presets");
}

bool StripProcessor::saveUserPreset (const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty())
        return false;
    auto folder = getUserPresetFolder();
    if (! folder.createDirectory())
        return false;
    auto state = captureState();
    state.setProperty ("presetName", name.trim(), nullptr);
    auto xml = state.createXml();
    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (clean + ".lastrip")))
        return false;
    currentPresetName = name.trim();
    return true;
}

bool StripProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;
    auto tree = juce::ValueTree::fromXml (*xml);
    restoreState (tree);
    currentPresetName = tree.getProperty ("presetName", file.getFileNameWithoutExtension()).toString();
    currentProgram = -1;
    return true;
}

void StripProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = captureState();
    state.setProperty ("program", currentProgram, nullptr);
    state.setProperty ("presetName", currentPresetName, nullptr);
    state.setProperty ("slot", activeSlot, nullptr);
    if (otherSlot.isValid())
    {
        juce::ValueTree holder (kOtherSlotTag);
        holder.appendChild (otherSlot.createCopy(), nullptr);
        state.appendChild (holder, nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void StripProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    auto tree = juce::ValueTree::fromXml (*xml);
    currentProgram = (int) tree.getProperty ("program", 0);
    currentPresetName = tree.getProperty ("presetName", "Default").toString();
    activeSlot = juce::jlimit (0, 1, (int) tree.getProperty ("slot", 0));
    otherSlot = {};
    if (auto holder = tree.getChildWithName (kOtherSlotTag); holder.isValid() && holder.getNumChildren() > 0)
        otherSlot = holder.getChild (0).createCopy();
    restoreState (tree);
}

juce::AudioProcessorEditor* StripProcessor::createEditor()
{
    return new StripEditor (*this);
}
} // namespace lacs

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new lacs::StripProcessor();
}
