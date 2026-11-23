#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "DspUtils.h"

namespace lacs
{
//==============================================================================
// Console character (Analogue mode)
//==============================================================================

/** Analogue console voicings. Each changes the EQ curves, the harmonic profile and the bandwidth. */
enum class ConsoleType { british = 0, american, vintage, numTypes };

/**
    Small, repeatable component tolerances of one analogue channel (like a channel number on a real desk).
    Values are per side (0 = left, 1 = right), so a stereo channel is never perfectly symmetric.
*/
struct Tolerance
{
    std::array<std::array<float, 2>, 4> eqFreq {};  // frequency scale, ~1 +- 3 %
    std::array<std::array<float, 2>, 4> eqGain {};  // gain error in dB at full boost/cut, +- 0.4
    std::array<std::array<float, 2>, 4> eqQ {};     // Q scale, ~1 +- 5 %
    std::array<float, 2> filterFreq {};             // HPF / LPF frequency scale
    std::array<float, 2> levelDb {};                // channel gain mismatch, +- 0.08 dB
    std::array<float, 2> harmonics {};              // harmonic amount scale, ~1 +- 15 %
    float thresholdDb = 0.0f;                       // dynamics threshold error, +- 0.4 dB

    static Tolerance ideal() noexcept;
    /** Deterministic tolerances for channel 1..72. */
    static Tolerance forChannel (int channel) noexcept;
};

struct ConsoleParams
{
    bool analogue = false;
    ConsoleType type = ConsoleType::british;
    float drive = 0.3f;        // 0..1: how hot the desk is driven (harmonics at a given level)
    float noiseDb = -120.0f;   // noise floor in dBFS RMS, <= -119 = off
    int channel = 1;           // 1..72, selects the tolerance set
};

/**
    One analogue gain stage (input transformer or line amplifier): level-dependent harmonics with the
    voicing's even/odd balance, its bandwidth (DC/transformer high-pass, gentle top roll-off) and
    an optional noise floor. Does nothing in Digital mode.
*/
class ConsoleStage
{
public:
    void prepare (double sampleRate, uint32_t seed);
    void reset();
    void setParams (const ConsoleParams& p, const Tolerance& t, bool outputStage) noexcept;
    void process (float* l, float* r, int n) noexcept;

    /** The static non-linearity (for tests): k2 = even amount, k3 = odd amount. */
    static float shape (float x, float k2, float k3) noexcept;

private:
    double sr = 48000.0;
    ConsoleParams params;
    bool output = false, active = false;
    std::array<float, 2> k2 {}, k3 {}, level { 1.0f, 1.0f };
    std::array<Biquad, 2> dcBlock {}, lowBump {}, topRoll {};
    bool bump = false, roll = false;
    float noiseGain = 0.0f;
    std::array<Random, 2> rng { Random (1u), Random (2u) };
    std::array<float, 2> pinkState {};
    ConsoleType lastType = ConsoleType::numTypes;
    bool lastOutput = false;
};

//==============================================================================
// Input filters
//==============================================================================

struct FilterParams
{
    float hpfHz = 16.0f;    // <= 16.5 = off, 18 dB/oct
    float lpfHz = 22000.0f; // >= 21000 = off, 12 dB/oct
};

/** High-pass (18 dB/oct) and low-pass (12 dB/oct). In Analogue mode they have the slight resonance of a desk filter. */
class FilterModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const FilterParams& p, const ConsoleParams& c, const Tolerance& t) noexcept;
    void process (float* l, float* r, int n) noexcept;
    bool isActive() const noexcept { return hpfOn || lpfOn; }

    /** Combined response at f (left channel design, for displays). */
    double magnitudeDb (double f) const noexcept;

private:
    double sr = 48000.0;
    std::array<Biquad, 2> hp1 {}, hp2 {}, lp {};
    bool hpfOn = false, lpfOn = false;
    float lastHp = -1.0f, lastLp = -1.0f, lastHpQ = -1.0f, lastLpQ = -1.0f;
    std::array<float, 2> lastScale { -1.0f, -1.0f };
};

//==============================================================================
// EQ
//==============================================================================
constexpr int kEqBands = 4;

struct EqParams
{
    bool enabled = true;
    std::array<float, kEqBands> freq { 100.0f, 400.0f, 2500.0f, 8000.0f };
    std::array<float, kEqBands> gain {};
    std::array<float, kEqBands> q { 0.7f, 1.0f, 1.0f, 0.7f };
    bool lfBell = false, hfBell = false;
};

/**
    Four band channel EQ: LF and HF shelves (switchable to bells) and two parametric mids.
    Digital: exact, constant-Q curves. Analogue: the selected console's curves (proportional Q, shelf
    dips and bumps) with that channel's component tolerances.
*/
class EqModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const EqParams& p, const ConsoleParams& c, const Tolerance& t) noexcept;
    void process (float* l, float* r, int n) noexcept;

    /** Designs the filters of one band for one side. `extra` is the analogue shelf dip (bypassed when unused). */
    static void design (Biquad& main, Biquad& extra, double sr, int band, float freq, float gainDb, float q, bool bell,
                        const ConsoleParams& c, const Tolerance& t, int side) noexcept;
    /** Magnitude of the whole EQ at f (left side), for displays. */
    static double responseDb (double sr, const EqParams& p, const ConsoleParams& c, const Tolerance& t, double f) noexcept;

private:
    double sr = 48000.0;
    EqParams params;
    ConsoleParams console;
    Tolerance tol;
    bool first = true;
    std::array<std::array<Biquad, 2>, kEqBands> main {}, extra {};
    std::array<bool, kEqBands> active {}, extraActive {};
};

//==============================================================================
// Dynamics
//==============================================================================

/** Where the gate and compressor detectors listen. */
enum class KeySource { internal = 0, eq, external, numSources };

struct GateParams
{
    bool enabled = false;
    bool expander = false;     // false = gate, true = 1:2 downward expander
    float thresholdDb = -45.0f;
    float rangeDb = 40.0f;     // maximum attenuation (positive dB)
    float attackMs = 0.5f;
    float holdMs = 40.0f;
    float releaseMs = 150.0f;
    float hysteresisDb = 4.0f;
    float keyHpfHz = 20.0f;    // <= 20.5 = off
    float keyLpfHz = 20000.0f; // >= 19500 = off
    bool listen = false;       // output the filtered key signal
    KeySource key = KeySource::internal;
};

/** Gate / expander with hysteresis, hold and key filters. */
class GateModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const GateParams& p, const Tolerance& t) noexcept;
    /** keyL/keyR may be nullptr (then the audio itself is the key). */
    void process (float* l, float* r, const float* keyL, const float* keyR, int n) noexcept;
    float getReductionDb() const noexcept { return grDb; }
    float getKeyLevelDb() const noexcept { return keyDb; }
    bool isOpen() const noexcept { return open; }

    /** Static attenuation (dB, >= 0) for a key level, used by the transfer display. */
    static float staticReduction (const GateParams& p, float levelDb) noexcept;

private:
    double sr = 48000.0;
    GateParams params;
    float thresholdOffset = 0.0f;
    std::array<Biquad, 2> keyHp {}, keyLp {};
    float lastHp = -1.0f, lastLp = -1.0f;
    float env = 0.0f, envRel = 0.0f, gainDb = 0.0f, grDb = 0.0f, keyDb = -120.0f;
    int holdCounter = 0;
    bool open = false;
};

enum class CompCharacter { vca = 0, fet, opto, varimu, numCharacters };

struct CompParams
{
    bool enabled = false;
    CompCharacter character = CompCharacter::vca;
    float thresholdDb = -20.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    bool autoRelease = false;
    float kneeDb = 6.0f;
    float makeupDb = 0.0f;
    bool autoMakeup = false;
    float mix = 1.0f;
    float scHpfHz = 20.0f;     // <= 20.5 = off
    KeySource key = KeySource::internal;
};

/**
    Channel compressor with four characters (VCA, FET, Opto, Vari-Mu). Digital mode uses an exact
    feed-forward detector. Analogue mode uses a feedback detector (like the hardware: softer, program
    dependent) when the key is internal, adds the character's harmonics while compressing, and applies the
    channel's tolerances.
*/
class CompressorModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const CompParams& p, const ConsoleParams& c, const Tolerance& t) noexcept;
    void process (float* l, float* r, const float* keyL, const float* keyR, int n) noexcept;
    float getReductionDb() const noexcept { return grDb; }
    float getKeyLevelDb() const noexcept { return keyDb; }
    float getAutoMakeupDb() const noexcept { return autoMakeupDb; }

    /** Static gain reduction (dB, >= 0) of a soft-knee compressor. */
    static float staticReduction (float levelDb, float thresholdDb, float ratio, float kneeDb) noexcept;
    /** Effective ratio and knee of a character (Vari-Mu's ratio grows with level). */
    static float effectiveRatio (CompCharacter c, float ratio, float overDb) noexcept;
    static float effectiveKnee (CompCharacter c, float kneeDb) noexcept;

private:
    double sr = 48000.0;
    CompParams params;
    ConsoleParams console;
    float thresholdOffset = 0.0f;
    std::array<Biquad, 2> scHp {};
    float lastHp = -1.0f;
    float envDb = 0.0f, slowDb = 0.0f, rms = 0.0f, peak = 0.0f, charge = 0.0f, grDb = 0.0f, keyDb = -120.0f;
    float avgGr = 0.0f, autoMakeupDb = 0.0f;
    float lastOutL = 0.0f, lastOutR = 0.0f;
    Smoothed makeupSm, mixSm;
    std::array<Biquad, 2> dcBlock {};
};

//==============================================================================
struct DeEsserParams
{
    bool enabled = false;
    float freqHz = 6500.0f;
    float thresholdDb = -30.0f;
    float rangeDb = 8.0f;
    bool wide = false;    // false = split band (only the sibilance band moves)
    bool listen = false;
};

class DeEsserModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const DeEsserParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    float getReductionDb() const noexcept { return grDb; }
    float getBandLevelDb() const noexcept { return bandDb; }

private:
    double sr = 48000.0;
    DeEsserParams params;
    float lastFreq = -1.0f;
    Biquad scHp1, scHp2;
    std::array<LR4Crossover, 2> split;
    EnvelopeFollower env;
    float gainSm = 1.0f, grDb = 0.0f, bandDb = -120.0f;
};

//==============================================================================
struct TransientParams
{
    bool enabled = false;
    float attack = 0.0f;   // -1..1
    float sustain = 0.0f;  // -1..1
    float speedMs = 20.0f; // how long a transient lasts (slow envelope attack)
    float mix = 1.0f;
};

/** Level independent transient shaper: boosts or softens attacks and sustain. */
class TransientModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const TransientParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    float getGainDb() const noexcept { return shownDb; }

private:
    double sr = 48000.0;
    TransientParams params;
    EnvelopeFollower fast, slow, hold;
    float gainDb = 0.0f, shownDb = 0.0f;
    float smooth = 0.0f, smoothDown = 0.0f;
    Smoothed mixSm;
};

//==============================================================================
enum class DriveType { tape = 0, tube, transformer, transistor, numTypes };

struct DriveParams
{
    bool enabled = false;
    DriveType type = DriveType::tape;
    float driveDb = 6.0f;
    float tone = 0.0f;    // -1 (dark) .. 1 (bright)
    float mix = 1.0f;
    float outputDb = 0.0f;
};

/** Saturation with four flavours, 2x oversampled. Always adds the same latency (on or off). */
class DriveModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const DriveParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    static constexpr int getLatency() { return Oversampler2x::latency(); }
    static float shape (DriveType t, float x) noexcept;

private:
    double sr = 48000.0;
    DriveParams params;
    std::array<Oversampler2x, 2> os;
    std::array<LatencyDelay, 2> dry;
    std::array<Biquad, 2> dcBlock {}, toneLow {}, toneHigh {}, xfLow {}, tapeTop {};
    Smoothed driveSm, compSm, mixSm, outSm;
    float lastTone = 100.0f;
    float comp = 1.0f;
    float lastDrive = -100.0f;
    DriveType lastType = DriveType::numTypes;
};
} // namespace lacs
