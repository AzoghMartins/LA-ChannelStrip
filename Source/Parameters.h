#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/StripEngine.h"

namespace lacs
{
// Every parameter: X (enum name, id string)
#define LACS_PARAMETERS(X)                                                                                         \
    X (mode, "mode") X (consoleType, "consoleType") X (consoleDrive, "consoleDrive") X (noise, "noise")              \
    X (channel, "channel")                                                                                         \
    X (inGain, "inGain") X (polarity, "polarity") X (hpf, "hpf") X (lpf, "lpf")                                    \
    X (gateOn, "gateOn") X (gateMode, "gateMode") X (gateThr, "gateThr") X (gateRange, "gateRange")                 \
    X (gateAttack, "gateAttack") X (gateHold, "gateHold") X (gateRelease, "gateRelease") X (gateHyst, "gateHyst")   \
    X (gateKeyHpf, "gateKeyHpf") X (gateKeyLpf, "gateKeyLpf") X (gateListen, "gateListen") X (gateKey, "gateKey")   \
    X (trOn, "trOn") X (trAttack, "trAttack") X (trSustain, "trSustain") X (trSpeed, "trSpeed") X (trMix, "trMix") \
    X (compOn, "compOn") X (compChar, "compChar") X (compThr, "compThr") X (compRatio, "compRatio")                 \
    X (compAttack, "compAttack") X (compRelease, "compRelease") X (compAutoRel, "compAutoRel") X (compKnee, "compKnee") \
    X (compMakeup, "compMakeup") X (compAutoGain, "compAutoGain") X (compMix, "compMix") X (compScHpf, "compScHpf") \
    X (compKey, "compKey")                                                                                         \
    X (eqOn, "eqOn") X (eqLfBell, "eqLfBell") X (eqHfBell, "eqHfBell")                                             \
    X (eq1Freq, "eq1Freq") X (eq1Gain, "eq1Gain") X (eq1Q, "eq1Q")                                                 \
    X (eq2Freq, "eq2Freq") X (eq2Gain, "eq2Gain") X (eq2Q, "eq2Q")                                                 \
    X (eq3Freq, "eq3Freq") X (eq3Gain, "eq3Gain") X (eq3Q, "eq3Q")                                                 \
    X (eq4Freq, "eq4Freq") X (eq4Gain, "eq4Gain") X (eq4Q, "eq4Q")                                                 \
    X (dsOn, "dsOn") X (dsFreq, "dsFreq") X (dsThr, "dsThr") X (dsRange, "dsRange") X (dsMode, "dsMode")           \
    X (dsListen, "dsListen")                                                                                       \
    X (drvOn, "drvOn") X (drvType, "drvType") X (drvDrive, "drvDrive") X (drvTone, "drvTone") X (drvMix, "drvMix") \
    X (drvOut, "drvOut")                                                                                           \
    X (outGain, "outGain") X (pan, "pan") X (width, "width") X (mix, "mix")

namespace ParamID
{
#define LACS_DECLARE_ID(name, id) inline constexpr auto name = id;
LACS_PARAMETERS (LACS_DECLARE_ID)
#undef LACS_DECLARE_ID
} // namespace ParamID

enum class P : int
{
#define LACS_DECLARE_ENUM(name, id) name,
    LACS_PARAMETERS (LACS_DECLARE_ENUM)
#undef LACS_DECLARE_ENUM
    count
};

/** Parameter IDs per EQ band (freq, gain, q), for loops in the editor and displays. */
namespace BandIDs
{
    extern const char* const eq[kEqBands][3];
}

/** IDs of the enable switch of each orderable module (indexed by ModuleId). */
const char* moduleEnableID (ModuleId m) noexcept;

const char* consoleTypeName (ConsoleType t) noexcept;
const char* compCharacterName (CompCharacter c) noexcept;
const char* driveTypeName (DriveType t) noexcept;

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Cached raw parameter pointers so the audio thread can build EngineParams cheaply. */
class ParameterCache
{
public:
    explicit ParameterCache (juce::AudioProcessorValueTreeState& state);
    /** Fills everything except the module order and monoSource. */
    void read (EngineParams& p) const noexcept;
    float get (P p) const noexcept { return values[(size_t) p]->load (std::memory_order_relaxed); }

private:
    std::array<std::atomic<float>*, (size_t) P::count> values {};
};

struct FactoryPreset
{
    FactoryPreset (juce::String n, std::vector<std::pair<const char*, float>> v, juce::String o = {})
        : name (std::move (n)), values (std::move (v)), order (std::move (o)) {}

    juce::String name;
    std::vector<std::pair<const char*, float>> values;
    juce::String order; // empty = default order
};

const std::vector<FactoryPreset>& getFactoryPresets();
} // namespace lacs
