#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "dsp/StripEngine.h"

namespace lacs
{
/** Lock-free single-producer / single-consumer queue (audio thread -> UI). */
template <typename T, int Capacity>
class SpscQueue
{
public:
    bool push (const T& item) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return false;
        data[(size_t) (n1 > 0 ? s1 : s2)] = item;
        fifo.finishedWrite (1);
        return true;
    }

    bool pop (T& item) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return false;
        item = data[(size_t) (n1 > 0 ? s1 : s2)];
        fifo.finishedRead (1);
        return true;
    }

private:
    juce::AbstractFifo fifo { Capacity };
    std::array<T, (size_t) Capacity> data {};
};

/** Block-based float FIFO for the analysers. */
class SampleFifo
{
public:
    void push (const float* data, int n) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        std::copy (data, data + n1, buffer.begin() + s1);
        std::copy (data + n1, data + n1 + n2, buffer.begin() + s2);
        fifo.finishedWrite (n1 + n2);
    }
    int pop (float* dest, int maxN) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (maxN, s1, n1, s2, n2);
        std::copy (buffer.begin() + s1, buffer.begin() + s1 + n1, dest);
        std::copy (buffer.begin() + s2, buffer.begin() + s2 + n2, dest + n1);
        fifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

private:
    static constexpr int size = 1 << 15;
    juce::AbstractFifo fifo { size };
    std::array<float, size> buffer {};
};

/** Meter values published by the audio thread. */
struct AtomicMeters
{
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f }, vuIn { -120.0f }, vuOut { -120.0f };
    std::atomic<float> gate { 0.0f }, comp { 0.0f }, deess { 0.0f }, transient { 0.0f };
    std::atomic<float> gateKey { -120.0f }, compKey { -120.0f }, deessBand { -120.0f }, autoMakeup { 0.0f };
    std::atomic<bool> gateOpen { true }, eqInSidechain { false }, sidechainActive { false };
};

class StripProcessor : public juce::AudioProcessor
{
public:
    StripProcessor();
    ~StripProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    int getNumPrograms() override { return (int) getFactoryPresets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- UI interface (message thread) ---------------------------------------------------------
    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }

    ModuleOrder getModuleOrder() const noexcept;
    void setModuleOrder (const ModuleOrder& order);

    /** A/B comparison: two complete settings slots (parameters and module order). */
    int getActiveSlot() const noexcept { return activeSlot; }
    void selectSlot (int slot);
    /** Copies the current settings into the other slot. */
    void copyToOtherSlot();

    /** Assistant: start listening, then finishAssist() analyses what it heard and applies the settings. */
    void startAssist();
    /** Returns a summary for the UI. */
    juce::String finishAssist();
    void cancelAssist();
    bool isAssistListening() const noexcept { return assistState.load() == 1; }
    double getAssistSeconds() const noexcept { return assistSeconds.load(); }

    // User presets
    static juce::File getUserPresetFolder();
    bool saveUserPreset (const juce::String& name);
    bool loadUserPreset (const juce::File& file);
    juce::String getCurrentPresetName() const { return currentPresetName; }

    double getEngineSampleRate() const noexcept { return engineSampleRate.load(); }
    bool isMonoSource() const noexcept { return monoSource.load(); }
    /** Tolerances of the current analogue channel (for displays). */
    Tolerance getTolerance() const noexcept;

    SpscQueue<DynFrame, 1024> dynQueue;
    SampleFifo spectrumIn, spectrumOut;
    AtomicMeters meters;

private:
    struct Listener : DynListener
    {
        StripProcessor* owner = nullptr;
        void dynFrame (const DynFrame& f) override { owner->dynQueue.push (f); }
    };

    void setParam (const char* id, float value);
    /** Sets every parameter in one pass: listed ones to their value, the rest to their default. */
    void applyParameterValues (const std::vector<std::pair<const char*, float>>& values);
    juce::ValueTree captureState();
    void restoreState (const juce::ValueTree& tree);

    juce::AudioProcessorValueTreeState apvts;
    ParameterCache params;
    StripEngine engine;
    EngineParams engineParams;
    Listener listener;

    std::atomic<uint32_t> packedOrder { 0 };
    std::atomic<bool> monoSource { false };

    // A/B
    int activeSlot = 0;
    juce::ValueTree otherSlot;

    // Assistant
    std::atomic<int> assistState { 0 }; // 0 idle, 1 listening, 2 stop requested, 3 stopped
    std::atomic<double> assistSeconds { 0.0 };
    std::atomic<int64_t> assistSamples { 0 };

    std::atomic<double> engineSampleRate { 48000.0 };
    int preparedBlockSize = 512;
    double preparedRate = 0.0;
    int currentProgram = 0;
    juce::String currentPresetName { "Default" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StripProcessor)
};
} // namespace lacs
