// Integration test: drives the real LA-ChannelStrip processor + editor the way a host would.
//  - checks bus layouts (incl. side-chain), latency, presets, state save/restore (module order, A/B slots), user presets,
//    the Assistant, the external side-chain, and renders the editor:  lacs_integration [screenshot.png [page [analogue]]]
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/PluginProcessor.h"
#include "../Source/gui/PluginEditor.h"

#include <random>

using namespace lacs;

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

constexpr double sr = 48000.0;

/** A vocal-ish test signal: phrases of a harmonic tone with vibrato and sibilant bursts, a little room noise between. */
juce::AudioBuffer<float> makeVocal (double seconds, float amp = 0.1f, unsigned seed = 1)
{
    juce::AudioBuffer<float> buf (2, (int) (seconds * sr));
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    const double pi = juce::MathConstants<double>::pi;
    float hp = 0.0f, prev = 0.0f;
    double phase = 0.0;
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const double t = i / sr, tp = std::fmod (t, 2.0);
        const bool phrase = tp < 1.5;
        const float env = phrase ? (float) (std::min (1.0, tp * 20.0) * std::min (1.0, (1.5 - tp) * 20.0)) * (0.6f + 0.4f * (float) std::sin (t * 3.1)) : 0.0f;
        phase += 2.0 * pi * (196.0 + 4.0 * std::sin (2.0 * pi * 5.5 * t)) / sr;
        float v = 0.0f;
        for (int h = 1; h <= 8; ++h)
            v += (float) std::sin (phase * h) / (float) (h * h * 0.5 + 0.5);
        const float n = nd (rng);
        hp = 0.85f * (hp + n - prev);
        prev = n;
        const bool ess = phrase && std::fmod (tp, 0.5) < 0.06;
        const float s = amp * (env * 0.5f * v + (ess ? 0.35f * hp : 0.0f)) + 0.0004f * n;
        buf.setSample (0, i, s);
        buf.setSample (1, i, s);
    }
    return buf;
}

/** Kick-and-snare drum loop. */
juce::AudioBuffer<float> makeDrums (double seconds, float amp = 0.5f, unsigned seed = 2)
{
    juce::AudioBuffer<float> buf (2, (int) (seconds * sr));
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    const double pi = juce::MathConstants<double>::pi;
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const double t = i / sr, tb = std::fmod (t, 0.5), ts = std::fmod (t + 0.25, 0.5);
        const float kick = (float) (std::exp (-tb * 14.0) * std::sin (2.0 * pi * (50.0 + 110.0 * std::exp (-tb * 30.0)) * tb));
        const float snare = (float) std::exp (-ts * 25.0) * (0.5f * nd (rng) + 0.5f * (float) std::sin (2.0 * pi * 190.0 * ts));
        buf.setSample (0, i, amp * (kick + 0.6f * snare));
        buf.setSample (1, i, amp * (kick + 0.5f * snare));
    }
    return buf;
}

void run (StripProcessor& proc, juce::AudioBuffer<float>& audio, int block, StripEditor* editor = nullptr)
{
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (int start = 0; start + block <= audio.getNumSamples(); start += block)
    {
        for (int ch = 0; ch < 2; ++ch)
            buf.copyFrom (ch, 0, audio, ch, start, block);
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            audio.copyFrom (ch, start, buf, ch, 0, block);
        if (editor != nullptr && (start / block) % 8 == 0)
            editor->updateDisplays();
    }
}

void setParam (StripProcessor& p, const char* id, float value)
{
    auto* param = p.getState().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

float param (StripProcessor& p, const char* id) { return p.getState().getRawParameterValue (id)->load(); }

float rmsDb (const juce::AudioBuffer<float>& b, int from)
{
    double acc = 0.0;
    for (int i = from; i < b.getNumSamples(); ++i) acc += (double) b.getSample (0, i) * b.getSample (0, i);
    return (float) (10.0 * std::log10 (acc / std::max (1, b.getNumSamples() - from) + 1.0e-20));
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("LA-ChannelStrip plugin integration test\n");

    auto proc = std::make_unique<StripProcessor>();
    {
        using CS = juce::AudioChannelSet;
        auto layout = [] (CS in, CS out, CS sc)
        {
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (in);
            l.inputBuses.add (sc);
            l.outputBuses.add (out);
            return l;
        };
        check (proc->checkBusesLayoutSupported (layout (CS::mono(), CS::mono(), CS::disabled()))
                   && proc->checkBusesLayoutSupported (layout (CS::stereo(), CS::stereo(), CS::disabled()))
                   && proc->checkBusesLayoutSupported (layout (CS::mono(), CS::stereo(), CS::mono()))
                   && proc->checkBusesLayoutSupported (layout (CS::stereo(), CS::stereo(), CS::stereo()))
                   && ! proc->checkBusesLayoutSupported (layout (CS::create5point1(), CS::create5point1(), CS::disabled()))
                   && ! proc->checkBusesLayoutSupported (layout (CS::stereo(), CS::mono(), CS::disabled())),
               "supports mono / stereo / mono->stereo with an optional side-chain; rejects 5.1 and stereo->mono");
    }

    proc->setPlayConfigDetails (2, 2, sr, 512);
    proc->prepareToPlay (sr, 512);
    const int latency = proc->getLatencySamples();
    check (latency > 0 && latency < 64, "reports latency " + juce::String (latency) + " samples (" + juce::String (1000.0 * latency / sr, 2) + " ms)");

    // ---- Presets ---------------------------------------------------------------------------------------
    {
        bool presetsOk = proc->getNumPrograms() >= 14;
        for (int i = 0; i < proc->getNumPrograms(); ++i)
        {
            proc->setCurrentProgram (i);
            proc->prepareToPlay (sr, 512);
            auto audio = (i % 2 == 0) ? makeVocal (3.0, 0.2f) : makeDrums (3.0, 0.4f);
            run (*proc, audio, 512);
            bool finite = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < audio.getNumSamples(); ++s)
                    finite &= std::isfinite (audio.getSample (ch, s));
            const float peak = audio.getMagnitude (0, audio.getNumSamples());
            const bool ok = finite && peak < 2.5f && peak > 0.01f;
            presetsOk &= ok;
            if (! ok)
                std::printf ("        preset %s: peak %.3f finite %d\n", proc->getProgramName (i).toRawUTF8(), peak, (int) finite);
        }
        check (presetsOk, juce::String (proc->getNumPrograms()) + " factory presets all produce clean, sensible output");
        proc->setCurrentProgram (2);
        check (proc->getProgramName (2) == "Lead Vocal - Analogue Warm" && juce::roundToInt (param (*proc, ParamID::mode)) == 1
                   && juce::roundToInt (param (*proc, ParamID::compChar)) == 2,
               "factory preset applies (Lead Vocal - Analogue Warm: analogue mode, opto compressor)");
        proc->setCurrentProgram (0);
        check (juce::roundToInt (param (*proc, ParamID::mode)) == 0 && param (*proc, ParamID::compOn) < 0.5f, "Default preset resets everything");
        proc->prepareToPlay (sr, 512);
    }

    // ---- State round trip and A/B ----------------------------------------------------------------------------
    {
        setParam (*proc, ParamID::eq3Gain, -2.5f);
        setParam (*proc, ParamID::compRatio, 3.3f);
        setParam (*proc, ParamID::mode, 1.0f);
        setParam (*proc, ParamID::channel, 17.0f);
        ModuleOrder o = defaultModuleOrder();
        std::reverse (o.begin(), o.end());
        proc->setModuleOrder (o);
        juce::MemoryBlock data;
        proc->getStateInformation (data);
        StripProcessor copy;
        copy.setStateInformation (data.getData(), (int) data.getSize());
        bool same = true;
        for (auto* p : proc->getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                same &= std::abs (rp->getValue() - copy.getState().getParameter (rp->paramID)->getValue()) < 1.0e-5f;
        check (same, "state save/restore round trip (" + juce::String (proc->getParameters().size()) + " parameters)");
        check (copy.getModuleOrder() == o, "module order is saved with the session");

        // A/B: B starts as a copy of A, then they diverge
        proc->selectSlot (1);
        const bool copied = std::abs (param (*proc, ParamID::eq3Gain) + 2.5f) < 0.01f && proc->getModuleOrder() == o;
        setParam (*proc, ParamID::eq3Gain, 4.0f);
        setParam (*proc, ParamID::mode, 0.0f);
        proc->setModuleOrder (defaultModuleOrder());
        proc->selectSlot (0);
        const bool backToA = std::abs (param (*proc, ParamID::eq3Gain) + 2.5f) < 0.01f && juce::roundToInt (param (*proc, ParamID::mode)) == 1
                             && proc->getModuleOrder() == o;
        proc->selectSlot (1);
        const bool backToB = std::abs (param (*proc, ParamID::eq3Gain) - 4.0f) < 0.01f && proc->getModuleOrder() == defaultModuleOrder();
        check (copied && backToA && backToB, "A/B slots: B starts as a copy, then each slot keeps its own settings and module order");

        juce::MemoryBlock ab;
        proc->getStateInformation (ab);
        StripProcessor abCopy;
        abCopy.setStateInformation (ab.getData(), (int) ab.getSize());
        const bool restoredB = abCopy.getActiveSlot() == 1 && std::abs (abCopy.getState().getRawParameterValue (ParamID::eq3Gain)->load() - 4.0f) < 0.01f;
        abCopy.selectSlot (0);
        const bool restoredA = std::abs (abCopy.getState().getRawParameterValue (ParamID::eq3Gain)->load() + 2.5f) < 0.01f && abCopy.getModuleOrder() == o;
        check (restoredB && restoredA, "both A/B slots are saved with the session");

        proc->copyToOtherSlot();
        proc->selectSlot (0);
        check (std::abs (param (*proc, ParamID::eq3Gain) - 4.0f) < 0.01f, "copy to the other slot");
        proc->setCurrentProgram (0);
    }

    // ---- User preset save / load --------------------------------------------------------------------------------
    {
        setParam (*proc, ParamID::compMix, 37.0f);
        ModuleOrder o = { 3, 2, 0, 1, 4, 5 };
        proc->setModuleOrder (o);
        const bool saved = proc->saveUserPreset ("Integration Test Preset");
        setParam (*proc, ParamID::compMix, 100.0f);
        proc->setModuleOrder (defaultModuleOrder());
        const auto file = StripProcessor::getUserPresetFolder().getChildFile ("Integration Test Preset.lastrip");
        const bool loaded = proc->loadUserPreset (file);
        check (saved && loaded && std::abs (param (*proc, ParamID::compMix) - 37.0f) < 0.01f && proc->getModuleOrder() == o,
               "user preset save and load (with module order)");
        file.deleteFile();
        proc->setCurrentProgram (0);
    }

    // ---- Assistant ----------------------------------------------------------------------------------------------
    {
        proc->prepareToPlay (sr, 512);
        auto audio = makeVocal (12.0, 0.05f, 3);
        proc->startAssist();
        run (*proc, audio, 512);
        check (proc->isAssistListening() && proc->getAssistSeconds() > 11.0, "assistant listens");
        const auto summary = proc->finishAssist();
        std::printf ("        %s\n", summary.toRawUTF8());
        const float trim = param (*proc, ParamID::inGain);
        check (! proc->isAssistListening() && trim > 3.0f && param (*proc, ParamID::compOn) > 0.5f, "assistant applies a trim and compressor settings");

        proc->prepareToPlay (sr, 512);
        auto again = makeVocal (7.0, 0.05f, 3); // ends 1 s into a phrase
        run (*proc, again, 512);
        const float vu = proc->meters.vuIn.load() + 3.01f + 18.0f;
        check (std::abs (vu) < 4.0f, "after the assistant the input sits around 0 VU while singing (" + juce::String (vu, 1) + " VU)");

        juce::AudioBuffer<float> silence (2, (int) sr * 3);
        silence.clear();
        proc->setCurrentProgram (0);
        proc->startAssist();
        run (*proc, silence, 512);
        const auto none = proc->finishAssist();
        check (none.contains ("not enough audio") && std::abs (param (*proc, ParamID::inGain)) < 0.01f, "assistant leaves the settings alone when it hears nothing");
    }

    // ---- External side-chain ---------------------------------------------------------------------------------------
    {
        StripProcessor sc;
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (juce::AudioChannelSet::stereo());
        l.inputBuses.add (juce::AudioChannelSet::stereo());
        l.outputBuses.add (juce::AudioChannelSet::stereo());
        const bool layoutOk = sc.setBusesLayout (l);
        sc.setRateAndBufferSizeDetails (sr, 512);
        sc.prepareToPlay (sr, 512);
        setParam (sc, ParamID::compOn, 1.0f);
        setParam (sc, ParamID::compThr, -30.0f);
        setParam (sc, ParamID::compRatio, 10.0f);
        setParam (sc, ParamID::compKey, 2.0f);
        auto render = [&] (float keyAmp)
        {
            sc.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> buf (4, 512);
            juce::MidiBuffer midi;
            double last = 0.0;
            for (int b = 0; b < 60; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (b * 512 + i) / sr;
                    const float tone = 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 500.0 * t);
                    const float key = keyAmp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 60.0 * t);
                    buf.setSample (0, i, tone);
                    buf.setSample (1, i, tone);
                    buf.setSample (2, i, key);
                    buf.setSample (3, i, key);
                }
                sc.processBlock (buf, midi);
                last = buf.getRMSLevel (0, 0, 512);
            }
            return juce::Decibels::gainToDecibels ((float) last);
        };
        const float freeDb = render (0.0f), duckedDb = render (0.8f);
        check (layoutOk && sc.meters.sidechainActive.load() && std::abs (freeDb + 23.0f) < 0.5f && duckedDb < freeDb - 15.0f,
               "external side-chain input ducks the channel (" + juce::String (freeDb, 1) + " -> " + juce::String (duckedDb, 1) + " dB)");
    }

    // ---- Mono in, stereo out -------------------------------------------------------------------------------------------
    {
        StripProcessor mono;
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (juce::AudioChannelSet::mono());
        l.inputBuses.add (juce::AudioChannelSet::disabled());
        l.outputBuses.add (juce::AudioChannelSet::stereo());
        const bool ok = mono.setBusesLayout (l);
        mono.setRateAndBufferSizeDetails (sr, 512);
        mono.prepareToPlay (sr, 512);
        setParam (mono, ParamID::pan, -100.0f);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        float lvlL = 0.0f, lvlR = 0.0f;
        for (int b = 0; b < 20; ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                buf.setSample (0, i, 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (b * 512 + i) / sr));
                buf.setSample (1, i, 0.0f);
            }
            mono.processBlock (buf, midi);
            lvlL = buf.getRMSLevel (0, 0, 512);
            lvlR = buf.getRMSLevel (1, 0, 512);
        }
        check (ok && mono.isMonoSource() && lvlR < 1.0e-4f && std::abs (juce::Decibels::gainToDecibels (lvlL / 0.0707107f) - 3.01f) < 0.2f,
               "mono source pans with constant power (hard left = +3 dB)");
    }

    // ---- Editor ---------------------------------------------------------------------------------------------------------
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorIfNeeded());
    auto* editor = dynamic_cast<StripEditor*> (ed.get());
    check (editor != nullptr, "editor created");
    {
        proc->setCurrentProgram (1);
        proc->prepareToPlay (sr, 512);
        for (int page = 0; page < 2; ++page)
            for (int mode = 0; mode < 2; ++mode)
            {
                editor->getPanel().setPage (page);
                setParam (*proc, ParamID::mode, (float) mode);
                auto audio = makeVocal (1.5, 0.2f);
                run (*proc, audio, 480, editor);
            }
        check (true, "both pages render in both modes with audio running");
    }

    if (argc > 1 && editor != nullptr)
    {
        const int page = argc > 2 ? juce::String (argv[2]).getIntValue() : 0;
        const bool analogue = argc > 3 && juce::String (argv[3]).getIntValue() != 0;
        proc->setCurrentProgram (analogue ? 2 : 1);
        if (page == 0)
        {
            setParam (*proc, ParamID::gateOn, 1.0f);
            setParam (*proc, ParamID::gateMode, 1.0f);
            setParam (*proc, ParamID::gateThr, -48.0f);
            setParam (*proc, ParamID::gateRange, 20.0f);
        }
        else
        {
            setParam (*proc, ParamID::trOn, 1.0f);
            setParam (*proc, ParamID::trAttack, 25.0f);
            setParam (*proc, ParamID::drvOn, 1.0f);
            setParam (*proc, ParamID::drvDrive, 6.0f);
            setParam (*proc, ParamID::drvMix, 50.0f);
        }
        setParam (*proc, ParamID::inGain, 6.0f);
        setParam (*proc, ParamID::eq2Gain, -3.0f);
        setParam (*proc, ParamID::eq3Gain, 3.0f);
        proc->prepareToPlay (sr, 512);
        editor->getPanel().setPage (page);
        auto audio = makeVocal (7.0, 0.12f, 4);
        run (*proc, audio, 480, editor);
        editor->setSize (MainPanel::baseWidth, MainPanel::baseHeight);
        for (int i = 0; i < 10; ++i)
            editor->updateDisplays();
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat png;
        check (stream.openedOk() && png.writeImageToStream (image, stream), "screenshot written to " + out.getFullPathName());
    }

    ed.reset();
    proc.reset();
    std::printf (failures == 0 ? "All integration checks passed\n" : "%d integration check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
