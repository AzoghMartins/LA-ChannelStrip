#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Modules.h"

namespace lacs
{
/** Modules whose order in the chain can be changed (input filters come first, the output stage last). */
enum class ModuleId { gate, transient, comp, eq, deesser, drive, numModules };
constexpr int kNumOrderable = (int) ModuleId::numModules;
using ModuleOrder = std::array<int, kNumOrderable>;

const char* moduleShortName (ModuleId m) noexcept;
ModuleOrder defaultModuleOrder() noexcept;
/** Packs / unpacks an order into a string such as "012345" (invalid strings give the default). */
ModuleOrder orderFromString (const char* text) noexcept;
void orderToString (const ModuleOrder& order, char* out6plusNull) noexcept;

struct EngineParams
{
    // Input
    float inputGainDb = 0.0f;
    bool polarity = false;
    FilterParams filters;
    ConsoleParams console;

    GateParams gate;
    TransientParams transient;
    CompParams comp;
    EqParams eq;
    DeEsserParams deess;
    DriveParams drive;
    ModuleOrder order = defaultModuleOrder();

    // Output
    float outputGainDb = 0.0f;
    float pan = 0.0f;    // -1..1
    float width = 1.0f;  // 0 = mono, 1 = unchanged, 2 = double side
    float mix = 1.0f;    // global dry/wet (parallel processing)
    bool monoSource = false; // pan as a mono source (constant power) instead of balance
};

/** One 10 ms frame for the dynamics history display. */
struct DynFrame
{
    float inDb = -120.0f, outDb = -120.0f, compGrDb = 0.0f, gateGrDb = 0.0f;
};

struct DynListener
{
    virtual ~DynListener() = default;
    virtual void dynFrame (const DynFrame&) = 0;
};

/** Meter values for the UI, valid after process(). */
struct MeterData
{
    float inPeak = 0.0f, outPeak = 0.0f;
    float vuInDb = -120.0f, vuOutDb = -120.0f; // 300 ms RMS
    float gateDb = 0.0f, compDb = 0.0f, deessDb = 0.0f, transientDb = 0.0f;
    float gateKeyDb = -120.0f, compKeyDb = -120.0f, deessBandDb = -120.0f;
    float autoMakeupDb = 0.0f;
    bool gateOpen = false;
    bool eqInSidechain = false;
};

/**
    Level statistics of the raw input in 10 ms frames, gathered while the Assistant listens.
    Histograms run from -100 to 0 dBFS in 0.5 dB steps.
*/
struct AnalysisStats
{
    static constexpr int bins = 200;
    std::array<uint32_t, bins> rms {}, peak {}, sib {};
    uint32_t frames = 0;

    void reset() noexcept { rms.fill (0); peak.fill (0); sib.fill (0); frames = 0; }
    void add (float rmsDb, float peakDb, float sibDb) noexcept;
    static int binOf (float db) noexcept;
    static float dbOf (int bin) noexcept { return -100.0f + 0.5f * (float) bin + 0.25f; }
    /** p in 0..1 of a histogram, counting only bins at or above `fromDb`. Returns -120 if empty. */
    static float percentile (const std::array<uint32_t, bins>& h, float p, float fromDb = -100.0f) noexcept;
    /** Energy (power) mean in dB of the bins at or above `fromDb`, like a VU or RMS meter averages. */
    static float energyMean (const std::array<uint32_t, bins>& h, float fromDb) noexcept;
};

/** Settings suggested by the Assistant from the statistics (dB values are after the suggested trim). */
struct AssistResult
{
    bool ok = false;
    float trimDb = 0.0f;
    bool gateOn = false;
    float gateThresholdDb = -60.0f;
    float compThresholdDb = -20.0f;
    bool deessOn = false;
    float deessThresholdDb = -30.0f;
    char summary[256] {};
};

/** Turns the statistics into gain staging (-18 dBFS RMS, like 0 VU on a desk), gate, compressor and de-esser settings. */
AssistResult analyseStats (const AnalysisStats& stats, float compRatio) noexcept;

/**
    Complete channel strip, independent of JUCE:
    Input (polarity, trim, console input stage, high/low-pass) -> re-orderable modules (Gate, Transient,
    Compressor, EQ, De-esser, Drive) -> console output stage -> output gain, pan, width, dry/wet.
*/
class StripEngine
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void setParams (const EngineParams& p) noexcept;

    /**
        Processes up to maxBlockSize samples in place. `channels` is 1 or 2.
        `sidechain` is nullptr or two channel pointers (external key for the gate and compressor).
    */
    void process (float* const* io, int channels, int numSamples, const float* const* sidechain, DynListener* listener) noexcept;

    int getLatency() const noexcept { return DriveModule::getLatency(); }
    const MeterData& getMeters() const noexcept { return meters; }
    double getSampleRate() const noexcept { return sr; }
    const Tolerance& getTolerance() const noexcept { return tolerance; }
    const EngineParams& getParams() const noexcept { return params; }

    /** Response of the input filters (for displays). */
    double filterResponseDb (double f) const noexcept { return filters.magnitudeDb (f); }

    /** Assistant statistics: gathered only while enabled. Read them after disabling. */
    void setStatsEnabled (bool on) noexcept;
    const AnalysisStats& getStats() const noexcept { return stats; }

    // Analysis buffers (valid after process())
    const float* getInputAnalysis() const noexcept { return analysisIn.data(); }
    const float* getOutputAnalysis() const noexcept { return analysisOut.data(); }

private:
    void runModule (ModuleId id, float* l, float* r, const float* scL, const float* scR, int n) noexcept;
    bool eqFeedsKey() const noexcept;

    double sr = 48000.0;
    int maxBlock = 512;
    EngineParams params;
    Tolerance tolerance = Tolerance::ideal();
    int lastChannel = -1;
    bool lastAnalogue = false;

    ConsoleStage consoleIn, consoleOut;
    FilterModule filters;
    GateModule gate;
    TransientModule transient;
    CompressorModule comp;
    EqModule eq, keyEqGate, keyEqComp;
    DeEsserModule deess;
    DriveModule drive;

    Smoothed inGain, outGain, panSm, widthSm, mixSm;
    std::array<LatencyDelay, 2> dryDelay;
    std::vector<float> rawL, rawR, workL, workR, keyL, keyR, analysisIn, analysisOut;

    float vuIn = 0.0f, vuOut = 0.0f, vuCoeff = 0.0f;
    int hopLength = 480, hopPos = 0;
    DynFrame hop;

    bool statsOn = false;
    AnalysisStats stats;
    double statSum = 0.0, sibSum = 0.0;
    float statPeak = 0.0f;
    Biquad sibHp1, sibHp2;

    MeterData meters;
};
} // namespace lacs
