#include "Parameters.h"

namespace lacs
{
namespace
{
using APF = juce::AudioParameterFloat;
using APFA = juce::AudioParameterFloatAttributes;

juce::String formatDb (float v, int) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String formatMs (float v, int)
{
    if (v >= 1000.0f) return juce::String (v / 1000.0f, 2) + " s";
    return (v < 10.0f ? juce::String (v, v < 1.0f ? 2 : 1) : juce::String (juce::roundToInt (v))) + " ms";
}
juce::String formatPct (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String formatSignedPct (float v, int) { return (v > 0.5f ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; }
juce::String formatHz (float v, int = 0)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
}
juce::String formatRatio (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + ":1"; }

juce::NormalisableRange<float> skewed (float lo, float hi, float centre, float step = 0.0f)
{
    juce::NormalisableRange<float> r (lo, hi, step);
    r.setSkewForCentre (centre);
    return r;
}

std::unique_ptr<APF> floatParam (const char* id, const juce::String& name, juce::NormalisableRange<float> range, float def,
                                 std::function<juce::String (float, int)> fmt, const juce::String& label = {})
{
    return std::make_unique<APF> (juce::ParameterID { id, 1 }, name, range, def,
                                  APFA().withLabel (label).withStringFromValueFunction (std::move (fmt)));
}

std::unique_ptr<juce::AudioParameterBool> boolParam (const char* id, const juce::String& name, bool def, bool automatable = true)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def,
                                                       juce::AudioParameterBoolAttributes().withAutomatable (automatable));
}

std::unique_ptr<juce::AudioParameterChoice> choiceParam (const char* id, const juce::String& name, const juce::StringArray& items,
                                                         int def, bool automatable = true)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def,
                                                         juce::AudioParameterChoiceAttributes().withAutomatable (automatable));
}

const juce::StringArray keySources { "Internal", "EQ", "External" };
} // namespace

namespace BandIDs
{
    const char* const eq[kEqBands][3] = { { ParamID::eq1Freq, ParamID::eq1Gain, ParamID::eq1Q },
                                          { ParamID::eq2Freq, ParamID::eq2Gain, ParamID::eq2Q },
                                          { ParamID::eq3Freq, ParamID::eq3Gain, ParamID::eq3Q },
                                          { ParamID::eq4Freq, ParamID::eq4Gain, ParamID::eq4Q } };
} // namespace BandIDs

const char* moduleEnableID (ModuleId m) noexcept
{
    switch (m)
    {
        case ModuleId::gate:       return ParamID::gateOn;
        case ModuleId::transient:  return ParamID::trOn;
        case ModuleId::comp:       return ParamID::compOn;
        case ModuleId::eq:         return ParamID::eqOn;
        case ModuleId::deesser:    return ParamID::dsOn;
        case ModuleId::drive:      return ParamID::drvOn;
        case ModuleId::numModules: break;
    }
    return ParamID::eqOn;
}

const char* consoleTypeName (ConsoleType t) noexcept
{
    switch (t)
    {
        case ConsoleType::british:  return "British";
        case ConsoleType::american: return "American";
        case ConsoleType::vintage:  return "Vintage";
        case ConsoleType::numTypes: break;
    }
    return "";
}

const char* compCharacterName (CompCharacter c) noexcept
{
    switch (c)
    {
        case CompCharacter::vca:    return "VCA";
        case CompCharacter::fet:    return "FET";
        case CompCharacter::opto:   return "Opto";
        case CompCharacter::varimu: return "Vari-Mu";
        case CompCharacter::numCharacters: break;
    }
    return "";
}

const char* driveTypeName (DriveType t) noexcept
{
    switch (t)
    {
        case DriveType::tape:        return "Tape";
        case DriveType::tube:        return "Tube";
        case DriveType::transformer: return "Transformer";
        case DriveType::transistor:  return "Transistor";
        case DriveType::numTypes: break;
    }
    return "";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    auto group = [] (const char* id, const char* name) { return std::make_unique<juce::AudioProcessorParameterGroup> (id, name, "|"); };
    auto offBelow = [] (float limit) { return [limit] (float v, int) { return v <= limit ? juce::String ("Off") : formatHz (v); }; };
    auto offAbove = [] (float limit) { return [limit] (float v, int) { return v >= limit ? juce::String ("Off") : formatHz (v); }; };

    // ---- Console ------------------------------------------------------------------------------------------
    auto console = group ("console", "Console");
    console->addChild (choiceParam (ParamID::mode, "Mode", { "Digital", "Analogue" }, 0));
    juce::StringArray types;
    for (int t = 0; t < (int) ConsoleType::numTypes; ++t) types.add (consoleTypeName ((ConsoleType) t));
    console->addChild (choiceParam (ParamID::consoleType, "Console", types, 0));
    console->addChild (floatParam (ParamID::consoleDrive, "Console Drive", { 0.0f, 100.0f, 1.0f }, 30.0f, formatPct, "%"));
    console->addChild (floatParam (ParamID::noise, "Console Noise", { -120.0f, -60.0f, 1.0f }, -120.0f,
                                   [] (float v, int) { return v <= -119.0f ? juce::String ("Off") : juce::String (juce::roundToInt (v)) + " dB"; }, "dB"));
    console->addChild (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { ParamID::channel, 1 }, "Console Channel", 1, 72, 1,
                                                                  juce::AudioParameterIntAttributes().withStringFromValueFunction (
                                                                      [] (int v, int) { return "Ch " + juce::String (v); })));
    layout.add (std::move (console));

    // ---- Input -----------------------------------------------------------------------------------------------
    auto input = group ("input", "Input");
    input->addChild (floatParam (ParamID::inGain, "Input Trim", { -24.0f, 24.0f, 0.1f }, 0.0f, formatDb, "dB"));
    input->addChild (boolParam (ParamID::polarity, "Polarity Invert", false));
    input->addChild (floatParam (ParamID::hpf, "High-Pass", skewed (16.0f, 500.0f, 80.0f, 1.0f), 16.0f, offBelow (16.5f), "Hz"));
    input->addChild (floatParam (ParamID::lpf, "Low-Pass", skewed (1000.0f, 22000.0f, 8000.0f, 10.0f), 22000.0f, offAbove (21000.0f), "Hz"));
    layout.add (std::move (input));

    // ---- Gate ---------------------------------------------------------------------------------------------------
    const GateParams gd;
    auto gate = group ("gate", "Gate");
    gate->addChild (boolParam (ParamID::gateOn, "Gate", false));
    gate->addChild (choiceParam (ParamID::gateMode, "Gate Mode", { "Gate", "Expander" }, 0));
    gate->addChild (floatParam (ParamID::gateThr, "Gate Threshold", { -80.0f, 0.0f, 0.1f }, gd.thresholdDb, formatDb, "dB"));
    gate->addChild (floatParam (ParamID::gateRange, "Gate Range", { 0.0f, 80.0f, 0.5f }, gd.rangeDb,
                                [] (float v, int) { return juce::String (v, 0) + " dB"; }, "dB"));
    gate->addChild (floatParam (ParamID::gateAttack, "Gate Attack", skewed (0.05f, 50.0f, 2.0f, 0.01f), gd.attackMs, formatMs, "ms"));
    gate->addChild (floatParam (ParamID::gateHold, "Gate Hold", skewed (0.0f, 500.0f, 60.0f, 1.0f), gd.holdMs, formatMs, "ms"));
    gate->addChild (floatParam (ParamID::gateRelease, "Gate Release", skewed (5.0f, 4000.0f, 200.0f, 1.0f), gd.releaseMs, formatMs, "ms"));
    gate->addChild (floatParam (ParamID::gateHyst, "Gate Hysteresis", { 0.0f, 12.0f, 0.1f }, gd.hysteresisDb,
                                [] (float v, int) { return juce::String (v, 1) + " dB"; }, "dB"));
    gate->addChild (floatParam (ParamID::gateKeyHpf, "Gate Key High-Pass", skewed (20.0f, 4000.0f, 250.0f, 1.0f), 20.0f, offBelow (20.5f), "Hz"));
    gate->addChild (floatParam (ParamID::gateKeyLpf, "Gate Key Low-Pass", skewed (500.0f, 20000.0f, 4000.0f, 10.0f), 20000.0f, offAbove (19500.0f), "Hz"));
    gate->addChild (boolParam (ParamID::gateListen, "Gate Key Listen", false, false));
    gate->addChild (choiceParam (ParamID::gateKey, "Gate Key", keySources, 0));
    layout.add (std::move (gate));

    // ---- Transient ------------------------------------------------------------------------------------------------
    auto tr = group ("transient", "Transient");
    tr->addChild (boolParam (ParamID::trOn, "Transient", false));
    tr->addChild (floatParam (ParamID::trAttack, "Transient Attack", { -100.0f, 100.0f, 1.0f }, 0.0f, formatSignedPct, "%"));
    tr->addChild (floatParam (ParamID::trSustain, "Transient Sustain", { -100.0f, 100.0f, 1.0f }, 0.0f, formatSignedPct, "%"));
    tr->addChild (floatParam (ParamID::trSpeed, "Transient Speed", skewed (2.0f, 100.0f, 20.0f, 0.1f), 20.0f, formatMs, "ms"));
    tr->addChild (floatParam (ParamID::trMix, "Transient Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    layout.add (std::move (tr));

    // ---- Compressor --------------------------------------------------------------------------------------------------
    const CompParams cd;
    auto comp = group ("comp", "Compressor");
    comp->addChild (boolParam (ParamID::compOn, "Compressor", false));
    juce::StringArray chars;
    for (int c = 0; c < (int) CompCharacter::numCharacters; ++c) chars.add (compCharacterName ((CompCharacter) c));
    comp->addChild (choiceParam (ParamID::compChar, "Compressor Character", chars, 0));
    comp->addChild (floatParam (ParamID::compThr, "Compressor Threshold", { -60.0f, 0.0f, 0.1f }, cd.thresholdDb, formatDb, "dB"));
    comp->addChild (floatParam (ParamID::compRatio, "Compressor Ratio", skewed (1.0f, 20.0f, 4.0f, 0.01f), cd.ratio, formatRatio));
    comp->addChild (floatParam (ParamID::compAttack, "Compressor Attack", skewed (0.05f, 100.0f, 8.0f, 0.01f), cd.attackMs, formatMs, "ms"));
    comp->addChild (floatParam (ParamID::compRelease, "Compressor Release", skewed (5.0f, 2000.0f, 150.0f, 1.0f), cd.releaseMs, formatMs, "ms"));
    comp->addChild (boolParam (ParamID::compAutoRel, "Compressor Auto Release", false));
    comp->addChild (floatParam (ParamID::compKnee, "Compressor Knee", { 0.0f, 18.0f, 0.1f }, cd.kneeDb,
                                [] (float v, int) { return v < 0.05f ? juce::String ("Hard") : juce::String (v, 1) + " dB"; }, "dB"));
    comp->addChild (floatParam (ParamID::compMakeup, "Compressor Makeup", { -12.0f, 24.0f, 0.1f }, 0.0f, formatDb, "dB"));
    comp->addChild (boolParam (ParamID::compAutoGain, "Compressor Auto Makeup", false));
    comp->addChild (floatParam (ParamID::compMix, "Compressor Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    comp->addChild (floatParam (ParamID::compScHpf, "Compressor Side-Chain HPF", skewed (20.0f, 500.0f, 100.0f, 1.0f), 20.0f, offBelow (20.5f), "Hz"));
    comp->addChild (choiceParam (ParamID::compKey, "Compressor Key", keySources, 0));
    layout.add (std::move (comp));

    // ---- EQ -----------------------------------------------------------------------------------------------------------
    const EqParams ed;
    auto eq = group ("eq", "EQ");
    eq->addChild (boolParam (ParamID::eqOn, "EQ", true));
    eq->addChild (boolParam (ParamID::eqLfBell, "EQ LF Bell", false));
    eq->addChild (boolParam (ParamID::eqHfBell, "EQ HF Bell", false));
    const char* names[kEqBands] = { "EQ LF", "EQ LMF", "EQ HMF", "EQ HF" };
    const float lo[kEqBands] = { 20.0f, 60.0f, 400.0f, 1500.0f }, hi[kEqBands] = { 600.0f, 3000.0f, 16000.0f, 20000.0f };
    const float mid[kEqBands] = { 100.0f, 400.0f, 2500.0f, 7000.0f };
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto k = (size_t) b;
        const juce::String n (names[b]);
        const bool shelf = b == 0 || b == kEqBands - 1;
        eq->addChild (floatParam (BandIDs::eq[b][0], n + " Freq", skewed (lo[b], hi[b], mid[b], 1.0f), ed.freq[k], formatHz, "Hz"));
        eq->addChild (floatParam (BandIDs::eq[b][1], n + " Gain", { -18.0f, 18.0f, 0.1f }, 0.0f, formatDb, "dB"));
        eq->addChild (floatParam (BandIDs::eq[b][2], n + (shelf ? " Q / Slope" : " Q"), skewed (0.3f, 6.0f, 1.0f, 0.01f), ed.q[k],
                                  [] (float v, int) { return juce::String (v, 2); }));
    }
    layout.add (std::move (eq));

    // ---- De-esser ---------------------------------------------------------------------------------------------------------
    const DeEsserParams dd;
    auto ds = group ("deesser", "De-Esser");
    ds->addChild (boolParam (ParamID::dsOn, "De-Esser", false));
    ds->addChild (floatParam (ParamID::dsFreq, "De-Esser Frequency", skewed (2000.0f, 16000.0f, 6500.0f, 10.0f), dd.freqHz, formatHz, "Hz"));
    ds->addChild (floatParam (ParamID::dsThr, "De-Esser Threshold", { -60.0f, 0.0f, 0.1f }, dd.thresholdDb, formatDb, "dB"));
    ds->addChild (floatParam (ParamID::dsRange, "De-Esser Range", { 0.0f, 20.0f, 0.1f }, dd.rangeDb,
                              [] (float v, int) { return juce::String (v, 1) + " dB"; }, "dB"));
    ds->addChild (choiceParam (ParamID::dsMode, "De-Esser Mode", { "Split", "Wide" }, 0));
    ds->addChild (boolParam (ParamID::dsListen, "De-Esser Listen", false, false));
    layout.add (std::move (ds));

    // ---- Drive --------------------------------------------------------------------------------------------------------------
    auto drv = group ("drive", "Drive");
    drv->addChild (boolParam (ParamID::drvOn, "Drive", false));
    juce::StringArray dtypes;
    for (int t = 0; t < (int) DriveType::numTypes; ++t) dtypes.add (driveTypeName ((DriveType) t));
    drv->addChild (choiceParam (ParamID::drvType, "Drive Type", dtypes, 0));
    drv->addChild (floatParam (ParamID::drvDrive, "Drive Amount", { 0.0f, 24.0f, 0.1f }, 6.0f, formatDb, "dB"));
    drv->addChild (floatParam (ParamID::drvTone, "Drive Tone", { -100.0f, 100.0f, 1.0f }, 0.0f, formatSignedPct, "%"));
    drv->addChild (floatParam (ParamID::drvMix, "Drive Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    drv->addChild (floatParam (ParamID::drvOut, "Drive Output", { -12.0f, 12.0f, 0.1f }, 0.0f, formatDb, "dB"));
    layout.add (std::move (drv));

    // ---- Output ---------------------------------------------------------------------------------------------------------------
    auto out = group ("output", "Output");
    out->addChild (floatParam (ParamID::outGain, "Output Gain", { -24.0f, 12.0f, 0.1f }, 0.0f, formatDb, "dB"));
    out->addChild (floatParam (ParamID::pan, "Pan", { -100.0f, 100.0f, 1.0f }, 0.0f,
                               [] (float v, int) { return std::abs (v) < 0.5f ? juce::String ("C") : juce::String (juce::roundToInt (std::abs (v))) + (v < 0 ? " L" : " R"); }, "%"));
    out->addChild (floatParam (ParamID::width, "Width", { 0.0f, 200.0f, 1.0f }, 100.0f, formatPct, "%"));
    out->addChild (floatParam (ParamID::mix, "Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    layout.add (std::move (out));

    return layout;
}

//==============================================================================
ParameterCache::ParameterCache (juce::AudioProcessorValueTreeState& s)
{
    const char* ids[] = {
#define LACS_ID_STRING(name, id) id,
        LACS_PARAMETERS (LACS_ID_STRING)
#undef LACS_ID_STRING
    };
    for (size_t i = 0; i < values.size(); ++i)
    {
        values[i] = s.getRawParameterValue (ids[i]);
        jassert (values[i] != nullptr);
    }
}

void ParameterCache::read (EngineParams& p) const noexcept
{
    auto f = [this] (P id) { return get (id); };
    auto b = [this] (P id) { return get (id) > 0.5f; };
    auto i = [this] (P id) { return (int) std::lround (get (id)); };
    auto at = [] (P first, int offset) { return (P) ((int) first + offset); };

    p.console.analogue = i (P::mode) == 1;
    p.console.type = (ConsoleType) juce::jlimit (0, (int) ConsoleType::numTypes - 1, i (P::consoleType));
    p.console.drive = f (P::consoleDrive) * 0.01f;
    p.console.noiseDb = f (P::noise);
    p.console.channel = juce::jlimit (1, 72, i (P::channel));

    p.inputGainDb = f (P::inGain);
    p.polarity = b (P::polarity);
    p.filters.hpfHz = f (P::hpf);
    p.filters.lpfHz = f (P::lpf);

    p.gate.enabled = b (P::gateOn);
    p.gate.expander = i (P::gateMode) == 1;
    p.gate.thresholdDb = f (P::gateThr);
    p.gate.rangeDb = f (P::gateRange);
    p.gate.attackMs = f (P::gateAttack);
    p.gate.holdMs = f (P::gateHold);
    p.gate.releaseMs = f (P::gateRelease);
    p.gate.hysteresisDb = f (P::gateHyst);
    p.gate.keyHpfHz = f (P::gateKeyHpf);
    p.gate.keyLpfHz = f (P::gateKeyLpf);
    p.gate.listen = b (P::gateListen);
    p.gate.key = (KeySource) juce::jlimit (0, 2, i (P::gateKey));

    p.transient.enabled = b (P::trOn);
    p.transient.attack = f (P::trAttack) * 0.01f;
    p.transient.sustain = f (P::trSustain) * 0.01f;
    p.transient.speedMs = f (P::trSpeed);
    p.transient.mix = f (P::trMix) * 0.01f;

    p.comp.enabled = b (P::compOn);
    p.comp.character = (CompCharacter) juce::jlimit (0, (int) CompCharacter::numCharacters - 1, i (P::compChar));
    p.comp.thresholdDb = f (P::compThr);
    p.comp.ratio = f (P::compRatio);
    p.comp.attackMs = f (P::compAttack);
    p.comp.releaseMs = f (P::compRelease);
    p.comp.autoRelease = b (P::compAutoRel);
    p.comp.kneeDb = f (P::compKnee);
    p.comp.makeupDb = f (P::compMakeup);
    p.comp.autoMakeup = b (P::compAutoGain);
    p.comp.mix = f (P::compMix) * 0.01f;
    p.comp.scHpfHz = f (P::compScHpf);
    p.comp.key = (KeySource) juce::jlimit (0, 2, i (P::compKey));

    p.eq.enabled = b (P::eqOn);
    p.eq.lfBell = b (P::eqLfBell);
    p.eq.hfBell = b (P::eqHfBell);
    for (int k = 0; k < kEqBands; ++k)
    {
        const P base = at (P::eq1Freq, 3 * k);
        p.eq.freq[(size_t) k] = f (base);
        p.eq.gain[(size_t) k] = f (at (base, 1));
        p.eq.q[(size_t) k] = f (at (base, 2));
    }

    p.deess.enabled = b (P::dsOn);
    p.deess.freqHz = f (P::dsFreq);
    p.deess.thresholdDb = f (P::dsThr);
    p.deess.rangeDb = f (P::dsRange);
    p.deess.wide = i (P::dsMode) == 1;
    p.deess.listen = b (P::dsListen);

    p.drive.enabled = b (P::drvOn);
    p.drive.type = (DriveType) juce::jlimit (0, (int) DriveType::numTypes - 1, i (P::drvType));
    p.drive.driveDb = f (P::drvDrive);
    p.drive.tone = f (P::drvTone) * 0.01f;
    p.drive.mix = f (P::drvMix) * 0.01f;
    p.drive.outputDb = f (P::drvOut);

    p.outputGainDb = f (P::outGain);
    p.pan = f (P::pan) * 0.01f;
    p.width = f (P::width) * 0.01f;
    p.mix = f (P::mix) * 0.01f;
}

//==============================================================================
const std::vector<FactoryPreset>& getFactoryPresets()
{
    using namespace ParamID;
    // Module order digits: 0 gate, 1 transient, 2 comp, 3 eq, 4 de-esser, 5 drive
    static const std::vector<FactoryPreset> presets {
        { "Default", {} },
        { "Lead Vocal - Clean",
          { { hpf, 90 }, { compOn, 1 }, { compThr, -22 }, { compRatio, 3 }, { compAttack, 8 }, { compRelease, 120 }, { compMakeup, 3 },
            { eq2Freq, 300 }, { eq2Gain, -2 }, { eq2Q, 1.2f }, { eq3Freq, 3500 }, { eq3Gain, 2 }, { eq4Freq, 11000 }, { eq4Gain, 3 },
            { dsOn, 1 }, { dsFreq, 6500 }, { dsThr, -32 }, { dsRange, 8 } } },
        { "Lead Vocal - Analogue Warm",
          { { mode, 1 }, { consoleType, 2 }, { consoleDrive, 45 }, { noise, -100 }, { hpf, 80 }, { compOn, 1 }, { compChar, 2 },
            { compThr, -24 }, { compRatio, 4 }, { compAttack, 10 }, { compRelease, 300 }, { compMakeup, 4 }, { eq1Freq, 120 },
            { eq1Gain, 1.5f }, { eq3Freq, 2500 }, { eq3Gain, 1.5f }, { eq4Freq, 10000 }, { eq4Gain, 2 }, { dsOn, 1 }, { dsThr, -30 },
            { drvOn, 1 }, { drvDrive, 4 }, { drvMix, 40 } } },
        { "Kick - Punch",
          { { hpf, 30 }, { gateOn, 1 }, { gateThr, -30 }, { gateRange, 30 }, { gateAttack, 0.1f }, { gateHold, 60 }, { gateRelease, 120 },
            { trOn, 1 }, { trAttack, 50 }, { trSustain, -20 }, { compOn, 1 }, { compChar, 1 }, { compThr, -18 }, { compRatio, 4 },
            { compAttack, 15 }, { compRelease, 80 }, { compMakeup, 2 }, { eqLfBell, 1 }, { eq1Freq, 60 }, { eq1Gain, 3 }, { eq1Q, 1.2f },
            { eq2Freq, 350 }, { eq2Gain, -4 }, { eq2Q, 1.5f }, { eq3Freq, 3500 }, { eq3Gain, 3 }, { eq3Q, 1.4f } } },
        { "Snare - Crack",
          { { hpf, 80 }, { gateOn, 1 }, { gateThr, -28 }, { gateRange, 24 }, { gateAttack, 0.1f }, { gateHold, 40 }, { gateRelease, 180 },
            { trOn, 1 }, { trAttack, 35 }, { trSustain, 10 }, { compOn, 1 }, { compThr, -20 }, { compRatio, 4 }, { compAttack, 5 },
            { compRelease, 100 }, { compMakeup, 2 }, { eqLfBell, 1 }, { eq1Freq, 200 }, { eq1Gain, 2 }, { eq2Freq, 800 }, { eq2Gain, -2 },
            { eq3Freq, 5000 }, { eq3Gain, 3 }, { eq4Freq, 10000 }, { eq4Gain, 2 } } },
        { "Toms - Gated",
          { { hpf, 50 }, { gateOn, 1 }, { gateThr, -32 }, { gateRange, 40 }, { gateAttack, 0.2f }, { gateHold, 80 }, { gateRelease, 300 },
            { gateKeyHpf, 60 }, { gateKeyLpf, 2000 }, { eq2Freq, 400 }, { eq2Gain, -4 }, { eq3Freq, 4000 }, { eq3Gain, 2 } } },
        { "Overheads - Air",
          { { hpf, 120 }, { compOn, 1 }, { compThr, -24 }, { compRatio, 2 }, { compAttack, 20 }, { compRelease, 200 }, { compMakeup, 1.5f },
            { eq2Freq, 500 }, { eq2Gain, -2 }, { eq2Q, 0.8f }, { eq4Freq, 12000 }, { eq4Gain, 3 } } },
        { "Bass DI - Solid",
          { { mode, 1 }, { consoleType, 1 }, { consoleDrive, 40 }, { hpf, 30 }, { compOn, 1 }, { compChar, 2 }, { compThr, -24 },
            { compRatio, 4 }, { compMakeup, 4 }, { eq1Freq, 80 }, { eq1Gain, 2 }, { eq2Freq, 250 }, { eq2Gain, -2 }, { eq3Freq, 800 },
            { eq3Gain, 2 }, { drvOn, 1 }, { drvType, 1 }, { drvDrive, 8 }, { drvMix, 50 } } },
        { "Electric Guitar",
          { { hpf, 90 }, { lpf, 12000 }, { compOn, 1 }, { compChar, 1 }, { compThr, -20 }, { compRatio, 3 }, { compAttack, 10 },
            { compMakeup, 2 }, { eq2Freq, 400 }, { eq2Gain, -2 }, { eq3Freq, 2500 }, { eq3Gain, 2 } } },
        { "Acoustic Guitar",
          { { hpf, 70 }, { trOn, 1 }, { trAttack, 15 }, { compOn, 1 }, { compChar, 2 }, { compThr, -22 }, { compRatio, 3 }, { compMakeup, 2.5f },
            { eq2Freq, 200 }, { eq2Gain, -3 }, { eq4Freq, 10000 }, { eq4Gain, 2.5f } } },
        { "Piano - Natural",
          { { mode, 1 }, { consoleType, 0 }, { consoleDrive, 20 }, { hpf, 40 }, { compOn, 1 }, { compChar, 3 }, { compThr, -20 },
            { compRatio, 2 }, { compMakeup, 1.5f }, { eq2Freq, 300 }, { eq2Gain, -1.5f }, { eq4Freq, 8000 }, { eq4Gain, 1.5f } } },
        { "Podcast / Voice-Over",
          { { hpf, 80 }, { gateOn, 1 }, { gateMode, 1 }, { gateThr, -50 }, { gateRange, 15 }, { compOn, 1 }, { compThr, -24 },
            { compRatio, 4 }, { compAutoGain, 1 }, { eq2Freq, 250 }, { eq2Gain, -2 }, { eq3Freq, 3000 }, { eq3Gain, 2 }, { dsOn, 1 },
            { dsThr, -30 } } },
        { "Synth Bass - Glue",
          { { compOn, 1 }, { compChar, 3 }, { compThr, -20 }, { compRatio, 3 }, { compMakeup, 2 }, { drvOn, 1 }, { drvType, 2 },
            { drvDrive, 6 }, { eq1Freq, 60 }, { eq1Gain, 2 } } },
        { "Drums - Parallel Crush",
          { { trOn, 1 }, { trAttack, 30 }, { compOn, 1 }, { compChar, 1 }, { compThr, -40 }, { compRatio, 20 }, { compAttack, 0.5f },
            { compRelease, 60 }, { compMakeup, 12 }, { compMix, 40 }, { drvOn, 1 }, { drvType, 3 }, { drvDrive, 10 }, { drvMix, 30 } } },
        { "Console Bus - Analogue",
          { { mode, 1 }, { consoleType, 0 }, { consoleDrive, 40 }, { noise, -100 }, { hpf, 25 }, { compOn, 1 }, { compThr, -18 },
            { compRatio, 2 }, { compAttack, 10 }, { compAutoRel, 1 }, { compMakeup, 1 }, { eq4Freq, 12000 }, { eq4Gain, 1 } } },
        { "Analogue Desk Only",
          { { mode, 1 }, { consoleType, 0 }, { consoleDrive, 30 } } },
    };
    return presets;
}
} // namespace lacs
