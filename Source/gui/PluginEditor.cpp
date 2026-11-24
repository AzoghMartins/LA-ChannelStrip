#include "PluginEditor.h"

namespace lacs
{
namespace
{
const char* eqBandNames[kEqBands] = { "LF", "LMF", "HMF", "HF" };

const char* consoleDescription (int type)
{
    switch (type)
    {
        case 0:  return "British: crisp, mostly odd harmonics. Bells narrow as you boost (proportional Q); shelves dip just past the corner. "
                        "Resonant high-pass.";
        case 1:  return "American: punchy, 2nd harmonic forward, a touch of low-end weight. Strong proportional Q and deeper shelf dips.";
        case 2:  return "Vintage: transformer-heavy and warm, rich even harmonics, low-end bump and a soft top. Broad bells and gentle shelves.";
        default: break;
    }
    return "";
}
} // namespace

//==============================================================================
BandStrip::BandStrip (const juce::String& t, juce::Colour c) : title (t), colour (c) {}

void BandStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f, 0.0f);
    g.setColour (juce::Colours::black.withAlpha (0.28f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawLine (r.getX() + 5.0f, r.getBottom() - 0.5f, r.getRight() - 5.0f, r.getBottom() - 0.5f, 1.0f);
    Hardware::drawLed (g, { r.getX() + 7.0f, 10.0f, 6.0f, 6.0f }, colour, true);
    g.setColour (Colours::text);
    g.setFont (uiFont (11.0f, true));
    g.drawText (title, 20, 4, getWidth() - 24, 18, juce::Justification::centredLeft);
}

void BandStrip::resized()
{
    auto r = getLocalBounds().reduced (2, 0);
    auto head = r.removeFromTop (26);
    if (header != nullptr)
        header->setBounds (head.removeFromRight (48).reduced (3, 3));
    MainPanel::grid (content, cols, r);
}

//==============================================================================
MainPanel::MainPanel (StripProcessor& p)
    : processor (p),
      state (p.getState()),
      chain (p),
      spectrum (p),
      dynamics (p),
      vu (p),
      inputSection ("Input"),
      outputSection ("Output"),
      eqSection ("EQ", &state, ParamID::eqOn, moduleColour (ModuleId::eq)),
      gateSection ("Gate / Expander", &state, ParamID::gateOn, moduleColour (ModuleId::gate)),
      compSection ("Compressor", &state, ParamID::compOn, moduleColour (ModuleId::comp)),
      trSection ("Transient", &state, ParamID::trOn, moduleColour (ModuleId::transient)),
      dsSection ("De-Esser", &state, ParamID::dsOn, moduleColour (ModuleId::deesser)),
      drvSection ("Drive", &state, ParamID::drvOn, moduleColour (ModuleId::drive)),
      consoleSection ("Analogue Console", &state, ParamID::mode, Colours::analogue)
{
    // ---- Header ----------------------------------------------------------------------------------
    refreshPresetList();
    presetBox.setTooltip ("Factory and user presets");
    presetBox.onItemChosen = [this] (int id)
    {
        if (id > 0 && id < 1000)
            processor.setCurrentProgram (id - 1);
        else if (id >= 1000 && id - 1000 < numUserPresets)
            processor.loadUserPreset (userPresetFiles[id - 1000]);
    };
    addAndMakeVisible (presetBox);

    saveButton.setTooltip ("Save the current settings (and module order) as a user preset");
    saveButton.onClick = [this]
    {
        auto* w = new juce::AlertWindow ("Save preset", "Name for the new preset:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", processor.getCurrentPresetName());
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
        {
            if (r == 1)
            {
                const auto name = w->getTextEditorContents ("name");
                showToast (processor.saveUserPreset (name) ? "Saved preset \"" + name + "\"" : juce::String ("Could not save the preset"));
                refreshPresetList();
            }
        }), true);
    };
    addAndMakeVisible (saveButton);

    assistButton.setTooltip ("Assistant: press, play the track for 5-30 seconds, press again. Sets the input trim so the channel sits at "
                             "0 VU (-18 dBFS), and the gate, compressor and de-esser thresholds from what it heard");
    assistButton.onClick = [this]
    {
        if (processor.isAssistListening())
            showToast (processor.finishAssist());
        else
            processor.startAssist();
    };
    addAndMakeVisible (assistButton);

    digitalButton.setTooltip ("Digital: clean, exact and silent. Constant-Q EQ, feed-forward compressor, no noise or console colour");
    analogueButton.setTooltip ("Analogue: the sound of a console channel. Harmonics that grow with level, the console's EQ curves, "
                               "feedback compression, per-channel component tolerances and an optional noise floor");
    digitalButton.onClick = [this] { setMode (false); };
    analogueButton.onClick = [this] { setMode (true); };
    addAndMakeVisible (digitalButton);
    addAndMakeVisible (analogueButton);

    slotA.setTooltip ("Settings slot A");
    slotB.setTooltip ("Settings slot B (starts as a copy of A)");
    slotA.onClick = [this] { processor.selectSlot (0); showToast ("Slot A"); };
    slotB.onClick = [this] { processor.selectSlot (1); showToast ("Slot B"); };
    copyButton.setTooltip ("Copy the current slot to the other one");
    copyButton.onClick = [this]
    {
        processor.copyToOtherSlot();
        showToast (processor.getActiveSlot() == 0 ? "Copied A to B" : "Copied B to A");
    };
    for (auto* b : { &slotA, &slotB, &copyButton })
        addAndMakeVisible (*b);

    // ---- Displays -----------------------------------------------------------------------------------
    addAndMakeVisible (chain);
    chain.onTileClicked = [this] (int page, const juce::String& key)
    {
        if (page >= 0) setPage (page);
        focusSection (key);
    };
    addAndMakeVisible (spectrum);
    spectrum.onBandSelected = [this] (int) { setPage (0); focusSection ("m3"); };
    addAndMakeVisible (dynamics);
    addAndMakeVisible (vu);
    addAndMakeVisible (meters);

    // ---- Tabs ----------------------------------------------------------------------------------------
    int tabIndex = 0;
    for (auto* t : { &tabMain, &tabColour })
    {
        t->setRadioGroupId (77);
        t->setClickingTogglesState (true);
        const int page = tabIndex++;
        t->onClick = [this, page] { setPage (page); };
        addAndMakeVisible (*t);
    }

    for (auto* s : { &inputSection, &outputSection, &eqSection, &gateSection, &compSection, &trSection, &dsSection, &drvSection, &consoleSection })
        addChildComponent (*s);
    inputSection.setVisible (true);
    outputSection.setVisible (true);

    // ---- Input / output ----------------------------------------------------------------------------
    polarityPill = pill (inputSection, ParamID::polarity, juce::CharPointer_UTF8 ("\xc3\x98  POLARITY"), Colours::accent);
    inputKnobs = { knob (inputSection, ParamID::inGain, "Trim", Colours::accent, true), knob (inputSection, ParamID::hpf, "High-Pass", Colours::blue),
                   knob (inputSection, ParamID::lpf, "Low-Pass", Colours::blue) };
    inputKnobs[0]->slider.setTooltip ("Input trim: aim for about 0 VU on the input meter (-18 dBFS), where the analogue console sounds as intended");
    inputKnobs[1]->slider.setTooltip ("18 dB/oct high-pass (the drag handle HP on the EQ display)");
    inputKnobs[2]->slider.setTooltip ("12 dB/oct low-pass (the drag handle LP on the EQ display)");
    outputKnobs = { knob (outputSection, ParamID::outGain, "Gain", Colours::accent, true), knob (outputSection, ParamID::pan, "Pan", Colours::accent, true),
                    knob (outputSection, ParamID::width, "Width", Colours::accent), knob (outputSection, ParamID::mix, "Mix", Colours::accent) };
    outputKnobs[1]->slider.setTooltip ("Pan: constant power for mono sources, balance for stereo sources");
    outputKnobs[3]->slider.setTooltip ("Dry/wet of the whole strip, for parallel processing");

    // ---- EQ -----------------------------------------------------------------------------------------------
    const auto ce = moduleColour (ModuleId::eq);
    for (int b = 0; b < kEqBands; ++b)
    {
        auto strip = std::make_unique<BandStrip> (eqBandNames[b], ce);
        eqSection.addAndMakeVisible (*strip);
        const bool shelf = b == 0 || b == kEqBands - 1;
        auto* f = knob (*strip, BandIDs::eq[b][0], "Freq", ce);
        auto* g = knob (*strip, BandIDs::eq[b][1], "Gain", ce, true);
        auto* q = knob (*strip, BandIDs::eq[b][2], shelf ? "Q / Slope" : "Q", ce);
        strip->setItems ({ f, g, q }, 1);
        if (shelf)
        {
            auto* bell = pill (*strip, b == 0 ? ParamID::eqLfBell : ParamID::eqHfBell, "BELL", ce);
            bell->setTooltip ("Switch the band from shelf to bell");
            strip->setHeaderItem (bell);
        }
        eqStrips[(size_t) b] = std::move (strip);
    }

    // ---- Gate -----------------------------------------------------------------------------------------------
    const auto cg = moduleColour (ModuleId::gate);
    gateMode = choice (gateSection, ParamID::gateMode, "Mode");
    gateMode->box.setTooltip ("Gate: closes to the range below the threshold. Expander: turns down gently (1:2) below the threshold");
    gateKey = choice (gateSection, ParamID::gateKey, "Key");
    gateKey->box.setTooltip ("What the gate listens to: the channel itself, the EQ (EQ leaves the audio path and shapes the key) "
                             "or the external side-chain input");
    gateKnobs = { knob (gateSection, ParamID::gateThr, "Thresh", Colours::threshold), knob (gateSection, ParamID::gateRange, "Range", cg),
                  knob (gateSection, ParamID::gateHyst, "Hyst", cg), knob (gateSection, ParamID::gateAttack, "Attack", cg),
                  knob (gateSection, ParamID::gateHold, "Hold", cg), knob (gateSection, ParamID::gateRelease, "Release", cg),
                  knob (gateSection, ParamID::gateKeyHpf, "Key HP", Colours::blue), knob (gateSection, ParamID::gateKeyLpf, "Key LP", Colours::blue) };
    gateKnobs[2]->slider.setTooltip ("Hysteresis: the gate closes this much below the threshold, so it does not chatter");
    gateListen = pill (gateSection, ParamID::gateListen, "KEY LISTEN", Colours::detection);
    gateListen->setTooltip ("Hear the filtered key signal the gate listens to");

    // ---- Compressor ------------------------------------------------------------------------------------------
    const auto cc = moduleColour (ModuleId::comp);
    compChar = choice (compSection, ParamID::compChar, "Character");
    compChar->box.setTooltip ("VCA: clean and precise. FET: fast, grabby, adds grit. Opto: smooth, program-dependent release. "
                              "Vari-Mu: gentle, ratio grows with level");
    compKey = choice (compSection, ParamID::compKey, "Key");
    compKey->box.setTooltip ("What the compressor listens to: the channel itself, the EQ (EQ leaves the audio path and shapes the key) "
                             "or the external side-chain input");
    compKnobs = { knob (compSection, ParamID::compThr, "Thresh", Colours::threshold), knob (compSection, ParamID::compRatio, "Ratio", cc),
                  knob (compSection, ParamID::compAttack, "Attack", cc), knob (compSection, ParamID::compRelease, "Release", cc),
                  knob (compSection, ParamID::compKnee, "Knee", cc), knob (compSection, ParamID::compMakeup, "Makeup", Colours::accent, true),
                  knob (compSection, ParamID::compMix, "Mix", cc), knob (compSection, ParamID::compScHpf, "SC HPF", Colours::blue) };
    compKnobs[7]->slider.setTooltip ("Side-chain high-pass: keeps the low end from driving the compressor");
    compAutoRel = pill (compSection, ParamID::compAutoRel, "AUTO REL", cc);
    compAutoRel->setTooltip ("Program-dependent release: fast for peaks, slow for sustained compression");
    compAutoGain = pill (compSection, ParamID::compAutoGain, "AUTO GAIN", Colours::accent);
    compAutoGain->setTooltip ("Automatic makeup gain that follows the average gain reduction");

    // ---- Transient ---------------------------------------------------------------------------------------------
    const auto ct = moduleColour (ModuleId::transient);
    trKnobs = { knob (trSection, ParamID::trAttack, "Attack", ct, true), knob (trSection, ParamID::trSustain, "Sustain", ct, true),
                knob (trSection, ParamID::trSpeed, "Speed", ct), knob (trSection, ParamID::trMix, "Mix", ct) };
    trKnobs[0]->slider.setTooltip ("More (or less) attack on every hit, independent of level");
    trKnobs[1]->slider.setTooltip ("More (or less) body and ring after each hit");
    trKnobs[2]->slider.setTooltip ("How long a transient lasts: short for clicks, longer for punch");

    // ---- De-esser ------------------------------------------------------------------------------------------------
    const auto cd = moduleColour (ModuleId::deesser);
    dsMode = choice (dsSection, ParamID::dsMode, "Mode");
    dsMode->box.setTooltip ("Split: only the sibilance band is turned down. Wide: the whole signal dips on each 's'");
    dsListen = pill (dsSection, ParamID::dsListen, "LISTEN", Colours::detection);
    dsListen->setTooltip ("Hear the sibilance band only, to tune the frequency");
    dsKnobs = { knob (dsSection, ParamID::dsFreq, "Freq", cd), knob (dsSection, ParamID::dsThr, "Thresh", Colours::threshold),
                knob (dsSection, ParamID::dsRange, "Range", cd) };

    // ---- Drive ---------------------------------------------------------------------------------------------------
    const auto cv = moduleColour (ModuleId::drive);
    drvType = choice (drvSection, ParamID::drvType, "Type");
    drvType->box.setTooltip ("Tape: soft, slightly asymmetric, rounded top. Tube: warm even harmonics. "
                             "Transformer: the low end saturates first. Transistor: harder, brighter edge");
    drvKnobs = { knob (drvSection, ParamID::drvDrive, "Drive", cv), knob (drvSection, ParamID::drvTone, "Tone", cv, true),
                 knob (drvSection, ParamID::drvMix, "Mix", cv), knob (drvSection, ParamID::drvOut, "Output", Colours::accent, true) };
    drvKnobs[0]->slider.setTooltip ("Saturation amount; the level is compensated automatically");

    // ---- Console ---------------------------------------------------------------------------------------------------
    const auto ca = Colours::analogue;
    consoleType = choice (consoleSection, ParamID::consoleType, "Console");
    consoleType->box.setTooltip ("The analogue desk's voicing: EQ curves, harmonic balance and bandwidth");
    consoleKnobs = { knob (consoleSection, ParamID::consoleDrive, "Drive", ca), knob (consoleSection, ParamID::noise, "Noise", ca),
                     knob (consoleSection, ParamID::channel, "Channel", ca) };
    consoleKnobs[0]->slider.setTooltip ("How hot the desk runs: more harmonics at the same level");
    consoleKnobs[1]->slider.setTooltip ("The desk's noise floor (independent on each side). Off by default");
    consoleKnobs[2]->slider.setTooltip ("Each channel number has its own small component tolerances (EQ, filters, thresholds, left/right balance)");
    consoleInfo.setFont (uiFont (10.5f));
    consoleInfo.setColour (juce::Label::textColourId, Colours::textDim);
    consoleInfo.setJustificationType (juce::Justification::topLeft);
    consoleInfo.setMinimumHorizontalScale (1.0f);
    consoleSection.addAndMakeVisible (consoleInfo);
    consoleSection.power->setTooltip ("Analogue mode (also in the header)");

    sectionByKey = { { "input", &inputSection }, { "output", &outputSection }, { "console", &consoleSection },
                     { "m0", &gateSection }, { "m1", &trSection }, { "m2", &compSection }, { "m3", &eqSection },
                     { "m4", &dsSection }, { "m5", &drvSection } };

    // Dim the controls of switched-off sections
    for (auto* s : { &eqSection, &gateSection, &compSection, &trSection, &dsSection, &drvSection, &consoleSection })
    {
        s->onActiveChanged = [s]
        {
            for (auto* c : s->getChildren())
                if (dynamic_cast<Knob*> (c) != nullptr || dynamic_cast<Choice*> (c) != nullptr || dynamic_cast<BandStrip*> (c) != nullptr
                    || dynamic_cast<juce::Label*> (c) != nullptr)
                    c->setAlpha (s->isActive() ? 1.0f : 0.5f);
        };
        s->onActiveChanged();
    }

    setPage (0);
    setSize (baseWidth, baseHeight);
}

Knob* MainPanel::knob (juce::Component& parent, const char* id, const juce::String& name, juce::Colour c, bool bipolar)
{
    auto k = std::make_unique<Knob> (state, id, name, c, bipolar);
    auto* raw = k.get();
    parent.addAndMakeVisible (*raw);
    owned.push_back (std::move (k));
    return raw;
}

Choice* MainPanel::choice (juce::Component& parent, const char* id, const juce::String& caption)
{
    auto c = std::make_unique<Choice> (state, id, caption);
    auto* raw = c.get();
    parent.addAndMakeVisible (*raw);
    owned.push_back (std::move (c));
    return raw;
}

ParamPill* MainPanel::pill (juce::Component& parent, const char* id, const juce::String& label, juce::Colour c)
{
    auto p = std::make_unique<ParamPill> (state, id, label, c);
    auto* raw = p.get();
    parent.addAndMakeVisible (*raw);
    owned.push_back (std::move (p));
    return raw;
}

void MainPanel::grid (std::vector<juce::Component*> items, int columns, juce::Rectangle<int> area)
{
    const int rows = ((int) items.size() + columns - 1) / juce::jmax (1, columns);
    const int cw = area.getWidth() / juce::jmax (1, columns);
    const int rh = area.getHeight() / juce::jmax (1, rows);
    int i = 0;
    for (auto* c : items)
    {
        if (c != nullptr)
            c->setBounds (area.getX() + (i % columns) * cw, area.getY() + (i / columns) * rh, cw, rh);
        ++i;
    }
}

void MainPanel::refreshPresetList()
{
    presetBox.clear (juce::dontSendNotification);
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem (processor.getProgramName (i), i + 1);

    numUserPresets = 0;
    auto files = StripProcessor::getUserPresetFolder().findChildFiles (juce::File::findFiles, false, "*.lastrip");
    files.sort();
    if (! files.isEmpty())
    {
        presetBox.addSectionHeading ("User presets");
        for (auto& f : files)
        {
            if (numUserPresets >= 256) break;
            userPresetFiles[numUserPresets] = f;
            presetBox.addItem (f.getFileNameWithoutExtension(), 1000 + numUserPresets);
            ++numUserPresets;
        }
    }
    presetBox.setText (processor.getCurrentPresetName(), juce::dontSendNotification);
}

void MainPanel::showToast (const juce::String& text)
{
    toastText = text;
    toastTicks = 30 * 8;
    repaint (0, 0, baseWidth, 46);
}

void MainPanel::setMode (bool analogue)
{
    if (auto* prm = state.getParameter (ParamID::mode))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (analogue ? 1.0f : 0.0f);
        prm->endChangeGesture();
    }
}

void MainPanel::setPage (int page)
{
    currentPage = juce::jlimit (0, 1, page);
    tabMain.setToggleState (currentPage == 0, juce::dontSendNotification);
    tabColour.setToggleState (currentPage == 1, juce::dontSendNotification);
    for (auto* s : { &eqSection, &gateSection, &compSection })
        s->setVisible (currentPage == 0);
    for (auto* s : { &trSection, &dsSection, &drvSection, &consoleSection })
        s->setVisible (currentPage == 1);
}

void MainPanel::focusSection (const juce::String& key)
{
    highlightKey = key;
    highlightAmount = 1.0f;
}

//==============================================================================
void MainPanel::resized()
{
    const int margin = 12, gap = 8;

    // Header
    presetBox.setBounds (318, 11, 186, 26);
    saveButton.setBounds (508, 11, 48, 26);
    assistButton.setBounds (568, 11, 104, 26);
    digitalButton.setBounds (684, 11, 76, 26);
    analogueButton.setBounds (762, 11, 88, 26);
    slotA.setBounds (864, 11, 30, 26);
    slotB.setBounds (896, 11, 30, 26);
    copyButton.setBounds (930, 11, 52, 26);

    chain.setBounds (margin, 54, baseWidth - 2 * margin, 44);

    const int dispY = 106, dispH = 284;
    spectrum.setBounds (margin, dispY, 560, dispH);
    dynamics.setBounds (580, dispY, 440, dispH);
    vu.setBounds (1028, dispY, 330, dispH);
    meters.setBounds (1366, dispY + 4, 62, dispH - 8);

    const int tabsY = dispY + dispH + gap;
    const int sideW = 128, outW = 148;
    const int pageX = margin + sideW + gap;
    tabMain.setBounds (pageX, tabsY, 130, 24);
    tabColour.setBounds (pageX + 136, tabsY, 160, 24);

    const int bottomH = baseHeight - tabsY - margin;
    inputSection.setBounds (margin, tabsY, sideW, bottomH);
    outputSection.setBounds (baseWidth - margin - outW, tabsY, outW, bottomH);
    {
        auto c = inputSection.getContentArea().withTrimmedTop (2);
        polarityPill->setBounds (c.removeFromTop (24).reduced (6, 0));
        c.removeFromTop (6);
        grid ({ inputKnobs[0], inputKnobs[1], inputKnobs[2] }, 1, c);
    }
    grid ({ outputKnobs[0], outputKnobs[1], outputKnobs[2], outputKnobs[3] }, 2,
          outputSection.getContentArea().withTrimmedTop (8).withTrimmedBottom (40));

    const int botY = tabsY + 24 + gap;
    const auto area = juce::Rectangle<int> (pageX, botY, baseWidth - 2 * margin - sideW - outW - 2 * gap, baseHeight - botY - margin);

    // ---- EQ & DYNAMICS -------------------------------------------------------------------------------
    {
        const int eqW = 470, gateW = 290;
        eqSection.setBounds (area.withWidth (eqW));
        gateSection.setBounds (area.withTrimmedLeft (eqW + gap).withWidth (gateW));
        compSection.setBounds (area.withTrimmedLeft (eqW + gateW + 2 * gap));

        auto e = eqSection.getContentArea();
        const int sw = e.getWidth() / kEqBands;
        for (auto& s : eqStrips)
            s->setBounds (e.removeFromLeft (sw).reduced (2, 0));

        auto g = gateSection.getContentArea();
        auto top = g.removeFromTop (46);
        gateMode->setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (4, 0));
        gateKey->setBounds (top.reduced (4, 0));
        g.removeFromTop (6);
        grid ({ gateKnobs[0], gateKnobs[1], gateKnobs[2], gateKnobs[3], gateKnobs[4], gateKnobs[5], gateKnobs[6], gateKnobs[7] }, 3, g);
        {
            // KEY LISTEN sits in the free cell next to the key filters
            const int cw = g.getWidth() / 3, rh = g.getHeight() / 3;
            gateListen->setBounds (juce::Rectangle<int> (g.getX() + 2 * cw, g.getY() + 2 * rh, cw, rh).withSizeKeepingCentre (cw - 8, 24));
        }

        auto c = compSection.getContentArea();
        auto ctop = c.removeFromTop (46);
        compChar->setBounds (ctop.removeFromLeft (ctop.getWidth() / 2).reduced (4, 0));
        compKey->setBounds (ctop.reduced (4, 0));
        c.removeFromTop (6);
        auto pills = c.removeFromBottom (26);
        compAutoRel->setBounds (pills.removeFromLeft (pills.getWidth() / 2).withSizeKeepingCentre (100, 22));
        compAutoGain->setBounds (pills.withSizeKeepingCentre (100, 22));
        grid ({ compKnobs[0], compKnobs[1], compKnobs[2], compKnobs[3], compKnobs[4], compKnobs[5], compKnobs[6], compKnobs[7] }, 4, c);
    }

    // ---- COLOUR & CONSOLE --------------------------------------------------------------------------------
    {
        const int trW = 230, dsW = 290, drvW = 300;
        trSection.setBounds (area.withWidth (trW));
        dsSection.setBounds (area.withTrimmedLeft (trW + gap).withWidth (dsW));
        drvSection.setBounds (area.withTrimmedLeft (trW + dsW + 2 * gap).withWidth (drvW));
        consoleSection.setBounds (area.withTrimmedLeft (trW + dsW + drvW + 3 * gap));

        grid ({ trKnobs[0], trKnobs[1], trKnobs[2], trKnobs[3] }, 2, trSection.getContentArea().withTrimmedTop (6));

        auto d = dsSection.getContentArea();
        auto dtop = d.removeFromTop (46);
        dsMode->setBounds (dtop.removeFromLeft (dtop.getWidth() / 2).reduced (4, 0));
        dsListen->setBounds (dtop.withTrimmedTop (16).withSizeKeepingCentre (90, 24));
        d.removeFromTop (6);
        grid ({ dsKnobs[0], dsKnobs[1], dsKnobs[2] }, 3, d.withSizeKeepingCentre (d.getWidth(), juce::jmin (d.getHeight(), 130)));

        auto v = drvSection.getContentArea();
        drvType->setBounds (v.removeFromTop (46).withSizeKeepingCentre (v.getWidth() / 2, 46));
        v.removeFromTop (6);
        grid ({ drvKnobs[0], drvKnobs[1], drvKnobs[2], drvKnobs[3] }, 2, v);

        auto k = consoleSection.getContentArea();
        consoleType->setBounds (k.removeFromTop (46).reduced (4, 0));
        k.removeFromTop (4);
        grid ({ consoleKnobs[0], consoleKnobs[1], consoleKnobs[2] }, 3, k.removeFromTop (110));
        k.removeFromTop (6);
        consoleInfo.setBounds (k.reduced (4, 0));
    }
}

void MainPanel::paint (juce::Graphics& g)
{
    Hardware::drawChassis (g, { 0, 0, baseWidth, baseHeight }, 46);

    // Logo: three channel faders at different positions
    {
        const float x0 = 16.0f, top = 11.0f, bottom = 35.0f;
        const float caps[] = { 0.65f, 0.3f, 0.5f };
        for (int i = 0; i < 3; ++i)
        {
            const float x = x0 + (float) i * 11.0f;
            g.setColour (Colours::edge.brighter (0.4f));
            g.fillRoundedRectangle (x - 1.0f, top, 2.0f, bottom - top, 1.0f);
            const float y = top + (bottom - top) * caps[i];
            g.setColour (i == 1 ? Colours::text : Colours::signature);
            g.fillRoundedRectangle (x - 4.5f, y - 3.0f, 9.0f, 6.0f, 1.5f);
        }

        g.setFont (uiFont (21.0f, true));
        g.setColour (Colours::text);
        g.drawText ("LA", 50, 8, 30, 30, juce::Justification::centredLeft);
        g.setColour (Colours::signature);
        g.drawText ("CHANNELSTRIP", 78, 8, 170, 30, juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (uiFont (10.0f));
        g.drawText ("CONSOLE", 250, 9, 66, 30, juce::Justification::centredLeft);
    }

    g.setFont (uiFont (11.5f));
    const auto infoArea = juce::Rectangle<int> (994, 4, baseWidth - 994 - 12, 38);
    if (toastTicks > 0)
    {
        g.setColour (Colours::learn);
        g.drawFittedText (toastText, infoArea, juce::Justification::centredRight, 3);
    }
    else
    {
        g.setColour (Colours::textDim);
        g.drawFittedText (infoText, infoArea, juce::Justification::centredRight, 2);
    }
}

void MainPanel::tick()
{
    DynFrame f;
    while (processor.dynQueue.pop (f))
        dynamics.addFrame (f);
    dynamics.repaint();
    spectrum.pullSpectrum();
    vu.tick();
    chain.tick();

    const float inPeak = processor.meters.inPeak.exchange (0.0f);
    const float outPeak = processor.meters.outPeak.exchange (0.0f);
    const float gr = std::max (processor.meters.comp.load(), processor.meters.deess.load());
    meters.setLevels (inPeak, outPeak, gr);

    auto v = [this] (const char* id) { return state.getRawParameterValue (id)->load(); };
    const bool analogue = juce::roundToInt (v (ParamID::mode)) == 1;
    digitalButton.setToggleState (! analogue, juce::dontSendNotification);
    analogueButton.setToggleState (analogue, juce::dontSendNotification);
    slotA.setToggleState (processor.getActiveSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState (processor.getActiveSlot() == 1, juce::dontSendNotification);
    copyButton.setButtonText (processor.getActiveSlot() == 0 ? "A > B" : "B > A");
    consoleInfo.setText (analogue ? juce::String (consoleDescription (juce::roundToInt (v (ParamID::consoleType))))
                                  : juce::String ("Digital mode: switch on (power button or ANALOGUE in the header) for the console's sound."),
                         juce::dontSendNotification);

    // Assistant
    blink = (blink + 1) % 20;
    if (processor.isAssistListening())
    {
        assistButton.setButtonText ("LISTENING " + juce::String ((int) processor.getAssistSeconds()) + "s");
        assistButton.setToggleState (blink < 14, juce::dontSendNotification);
        if (processor.getAssistSeconds() >= 30.0)
            showToast (processor.finishAssist());
    }
    else
    {
        assistButton.setButtonText ("ASSIST");
        assistButton.setToggleState (false, juce::dontSendNotification);
    }

    if (highlightAmount > 0.0f)
    {
        highlightAmount = juce::jmax (0.0f, highlightAmount - 0.04f);
        if (auto it = sectionByKey.find (highlightKey); it != sectionByKey.end())
            it->second->setHighlight (highlightAmount);
    }

    if (toastTicks > 0 && --toastTicks == 0)
        repaint (0, 0, baseWidth, 46);

    const double sr = processor.getEngineSampleRate();
    juce::String info;
    const bool wantsSc = (v (ParamID::gateOn) > 0.5f && juce::roundToInt (v (ParamID::gateKey)) == 2)
                         || (v (ParamID::compOn) > 0.5f && juce::roundToInt (v (ParamID::compKey)) == 2);
    if (wantsSc && ! processor.meters.sidechainActive.load())
        info << "External key selected, but no side-chain is connected (using the channel)\n";
    else if (processor.meters.eqInSidechain.load())
        info << "EQ is keying the dynamics (not in the audio path)\n";
    info << (analogue ? juce::String (consoleTypeName ((ConsoleType) juce::roundToInt (v (ParamID::consoleType)))) + " console, ch "
                            + juce::String (juce::roundToInt (v (ParamID::channel))) + "    "
                      : juce::String ("Digital    "))
         << "Latency " << juce::String (1000.0 * processor.getLatencySamples() / juce::jmax (1.0, sr), 1) << " ms    "
         << juce::String (sr / 1000.0, 1) << " kHz";
    if (info != infoText)
    {
        infoText = info;
        repaint (0, 0, baseWidth, 46);
    }

    const auto name = processor.getCurrentPresetName();
    if (presetBox.getText() != name && ! presetBox.isPopupActive())
        presetBox.setText (name, juce::dontSendNotification);
}

//==============================================================================
StripEditor::StripEditor (StripProcessor& p)
    : AudioProcessorEditor (p), panel (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (panel);

    setResizable (true, true);
    setResizeLimits (MainPanel::baseWidth * 5 / 10, MainPanel::baseHeight * 5 / 10, MainPanel::baseWidth * 2, MainPanel::baseHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) MainPanel::baseWidth / MainPanel::baseHeight);
    setSize (MainPanel::baseWidth * 9 / 10, MainPanel::baseHeight * 9 / 10);

    startTimerHz (30);
}

StripEditor::~StripEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void StripEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void StripEditor::resized()
{
    const float scale = (float) getWidth() / (float) MainPanel::baseWidth;
    panel.setTransform (juce::AffineTransform::scale (scale));
    panel.setBounds (0, 0, MainPanel::baseWidth, MainPanel::baseHeight);
}
} // namespace lacs
