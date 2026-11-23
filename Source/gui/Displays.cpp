#include "Displays.h"
#include "../PluginProcessor.h"

namespace lacs
{
namespace
{
void drawLegend (juce::Graphics& g, juce::Rectangle<int> area, std::initializer_list<std::pair<juce::String, juce::Colour>> items)
{
    g.setFont (uiFont (10.5f, true));
    int x = area.getRight();
    std::vector<std::pair<juce::String, juce::Colour>> list (items);
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), it->first) + 26;
        x -= w;
        g.setColour (it->second);
        g.fillRoundedRectangle ((float) x, (float) area.getCentreY() - 2.0f, 12.0f, 4.0f, 2.0f);
        g.setColour (Colours::textDim);
        g.drawText (it->first, x + 16, area.getY(), w - 16, area.getHeight(), juce::Justification::centredLeft);
    }
}

void drawFrame (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour (Colours::graphBg);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (Colours::edge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

void strokeDashed (juce::Graphics& g, const juce::Path& p, float thickness, float on, float off)
{
    juce::Path dashed;
    const float dashes[] = { on, off };
    juce::PathStrokeType (thickness).createDashedStroke (dashed, p, dashes, 2);
    g.fillPath (dashed);
}

float raw (juce::AudioProcessorValueTreeState& s, const char* id) { return s.getRawParameterValue (id)->load(); }

ConsoleParams consoleFromState (juce::AudioProcessorValueTreeState& s)
{
    ConsoleParams c;
    c.analogue = juce::roundToInt (raw (s, ParamID::mode)) == 1;
    c.type = (ConsoleType) juce::jlimit (0, 2, juce::roundToInt (raw (s, ParamID::consoleType)));
    c.channel = juce::roundToInt (raw (s, ParamID::channel));
    return c;
}

EqParams eqFromState (juce::AudioProcessorValueTreeState& s)
{
    EqParams p;
    p.enabled = raw (s, ParamID::eqOn) > 0.5f;
    p.lfBell = raw (s, ParamID::eqLfBell) > 0.5f;
    p.hfBell = raw (s, ParamID::eqHfBell) > 0.5f;
    for (int b = 0; b < kEqBands; ++b)
    {
        p.freq[(size_t) b] = raw (s, BandIDs::eq[b][0]);
        p.gain[(size_t) b] = raw (s, BandIDs::eq[b][1]);
        p.q[(size_t) b] = raw (s, BandIDs::eq[b][2]);
    }
    return p;
}

const char* bandLabels[kEqBands] = { "LF", "LM", "HM", "HF" };
} // namespace

juce::Colour moduleColour (ModuleId m)
{
    switch (m)
    {
        case ModuleId::gate:       return Colours::accent;
        case ModuleId::transient:  return Colours::blue;
        case ModuleId::comp:       return Colours::danger;
        case ModuleId::eq:         return Colours::threshold;
        case ModuleId::deesser:    return Colours::pink;
        case ModuleId::drive:      return Colours::detection;
        case ModuleId::numModules: break;
    }
    return Colours::accent;
}

//==============================================================================
SpectrumDisplay::SpectrumDisplay (StripProcessor& p) : processor (p), state (p.getState())
{
    fifoIn.assign ((size_t) fftSize, 0.0f);
    fifoOut.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);
    window.resize ((size_t) fftSize);
    specIn.assign ((size_t) fftSize / 2, -120.0f);
    specOut.assign ((size_t) fftSize / 2, -120.0f);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (fftSize - 1));
    setOpaque (true);
    setTooltip ("Drag the EQ nodes to set frequency and gain, mouse wheel for Q, double-click to reset. "
                "Drag HP / LP sideways; double-click switches a filter off");
}

float SpectrumDisplay::value (const char* id) const { return state.getRawParameterValue (id)->load(); }

void SpectrumDisplay::analyse (std::vector<float>& fifo, int& fifoPos, std::vector<float>& smoothed)
{
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = fifo[(size_t) ((fifoPos + i) % fftSize)] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);
    for (int i = 0; i < fftSize / 2; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] / (float) (fftSize / 4), -140.0f);
        auto& s = smoothed[(size_t) i];
        s = db > s ? s + (db - s) * 0.4f : s + (db - s) * 0.1f;
    }
}

void SpectrumDisplay::pullSpectrum()
{
    float tmp[2048];
    int got;
    while ((got = processor.spectrumIn.pop (tmp, 2048)) > 0)
        for (int i = 0; i < got; ++i) { fifoIn[(size_t) posIn] = tmp[i]; posIn = (posIn + 1) % fftSize; }
    while ((got = processor.spectrumOut.pop (tmp, 2048)) > 0)
        for (int i = 0; i < got; ++i) { fifoOut[(size_t) posOut] = tmp[i]; posOut = (posOut + 1) % fftSize; }
    analyse (fifoIn, posIn, specIn);
    analyse (fifoOut, posOut, specOut);
    repaint();
}

juce::Rectangle<float> SpectrumDisplay::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromTop (22.0f);
    r.removeFromBottom (18.0f);
    r.removeFromLeft (30.0f);
    r.removeFromRight (6.0f);
    return r;
}

float SpectrumDisplay::freqToX (float f, juce::Rectangle<float> a) const
{
    return a.getX() + a.getWidth() * std::log (juce::jmax (1.0f, f) / 20.0f) / std::log (1000.0f);
}

float SpectrumDisplay::xToFreq (float x, juce::Rectangle<float> a) const
{
    return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - a.getX()) / a.getWidth()));
}

float SpectrumDisplay::dbToY (float db, juce::Rectangle<float> a) const { return juce::jmap (db, maxDb, minDb, a.getY(), a.getBottom()); }

float SpectrumDisplay::yToDb (float y, juce::Rectangle<float> a) const
{
    return juce::jmap (juce::jlimit (a.getY(), a.getBottom(), y), a.getY(), a.getBottom(), maxDb, minDb);
}

juce::Point<float> SpectrumDisplay::nodePosition (int node, juce::Rectangle<float> a) const
{
    if (node < kEqBands)
        return { freqToX (value (BandIDs::eq[node][0]), a), dbToY (value (BandIDs::eq[node][1]), a) };
    const float f = node == kEqBands ? value (ParamID::hpf) : value (ParamID::lpf);
    const float x = juce::jlimit (a.getX() + 8.0f, a.getRight() - 8.0f, freqToX (f, a));
    return { x, dbToY (-3.0f, a) };
}

int SpectrumDisplay::hitNode (juce::Point<float> p) const
{
    const auto a = plotArea();
    for (int n = 0; n < kEqBands + 2; ++n)
        if (nodePosition (n, a).getDistanceFrom (p) < 10.0f)
            return n;
    return -1;
}

void SpectrumDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int h = hitNode (e.position);
    if (h != hover)
    {
        hover = h;
        setMouseCursor (h >= kEqBands ? juce::MouseCursor::LeftRightResizeCursor
                                      : (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor));
        repaint();
    }
}

void SpectrumDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragging = hitNode (e.position);
    if (dragging < 0)
        return;
    if (dragging < kEqBands)
    {
        selected = dragging;
        if (onBandSelected) onBandSelected (selected);
        if (auto* on = state.getParameter (ParamID::eqOn); on != nullptr && on->getValue() < 0.5f)
            on->setValueNotifyingHost (1.0f);
        state.getParameter (BandIDs::eq[dragging][0])->beginChangeGesture();
        state.getParameter (BandIDs::eq[dragging][1])->beginChangeGesture();
    }
    else
        state.getParameter (dragging == kEqBands ? ParamID::hpf : ParamID::lpf)->beginChangeGesture();
}

void SpectrumDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;
    const auto a = plotArea();
    const float f = xToFreq (e.position.x, a);
    if (dragging < kEqBands)
    {
        auto* fp = state.getParameter (BandIDs::eq[dragging][0]);
        auto* gp = state.getParameter (BandIDs::eq[dragging][1]);
        fp->setValueNotifyingHost (fp->convertTo0to1 (fp->getNormalisableRange().snapToLegalValue (f)));
        gp->setValueNotifyingHost (gp->convertTo0to1 (juce::jlimit (-18.0f, 18.0f, yToDb (e.position.y, a))));
    }
    else if (dragging == kEqBands)
    {
        auto* p = state.getParameter (ParamID::hpf);
        p->setValueNotifyingHost (p->convertTo0to1 (f <= 21.0f ? 16.0f : p->getNormalisableRange().snapToLegalValue (f)));
    }
    else
    {
        auto* p = state.getParameter (ParamID::lpf);
        p->setValueNotifyingHost (p->convertTo0to1 (f >= 19500.0f ? 22000.0f : p->getNormalisableRange().snapToLegalValue (f)));
    }
    repaint();
}

void SpectrumDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragging >= 0 && dragging < kEqBands)
    {
        state.getParameter (BandIDs::eq[dragging][0])->endChangeGesture();
        state.getParameter (BandIDs::eq[dragging][1])->endChangeGesture();
    }
    else if (dragging >= kEqBands)
        state.getParameter (dragging == kEqBands ? ParamID::hpf : ParamID::lpf)->endChangeGesture();
    dragging = -1;
}

void SpectrumDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int h = hitNode (e.position);
    if (h < 0)
        return;
    auto* p = state.getParameter (h < kEqBands ? BandIDs::eq[h][1] : (h == kEqBands ? ParamID::hpf : ParamID::lpf));
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->getDefaultValue());
    p->endChangeGesture();
}

void SpectrumDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    int h = hitNode (e.position);
    if (h < 0 || h >= kEqBands) h = selected;
    auto* q = state.getParameter (BandIDs::eq[h][2]);
    q->beginChangeGesture();
    q->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, q->getValue() + (w.deltaY > 0 ? 0.04f : -0.04f)));
    q->endChangeGesture();
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    drawFrame (g, getLocalBounds().toFloat());
    const auto a = plotArea();
    const double sr = std::max (8000.0, processor.getEngineSampleRate());

    g.setFont (uiFont (9.5f));
    for (float f : { 30.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = freqToX (f, a);
        g.setColour (Colours::grid);
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (Colours::textDim);
        g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f), (int) x - 20,
                    (int) a.getBottom() + 3, 40, 12, juce::Justification::centred);
    }
    for (float db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
    {
        const float y = dbToY (db, a);
        g.setColour (std::abs (db) < 0.1f ? Colours::edge : Colours::grid);
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        g.setColour (Colours::textDim);
        g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), 0, (int) y - 6, 26, 12, juce::Justification::centredRight);
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    // ---- Spectra (slope compensated +3 dB/oct so pink noise is flat) ------------------------------------
    const int bins = fftSize / 2;
    auto spectrumDbAt = [&] (const std::vector<float>& s, float f)
    {
        const float binWidth = (float) sr / (float) fftSize;
        const int b0 = juce::jlimit (1, bins - 1, (int) (f * 0.944f / binWidth));
        const int b1 = juce::jlimit (b0, bins - 1, (int) (f * 1.059f / binWidth));
        double p = 0.0;
        for (int b = b0; b <= b1; ++b) p += std::pow (10.0, s[(size_t) b] / 10.0);
        const float db = (float) (10.0 * std::log10 (p / (double) (b1 - b0 + 1) + 1.0e-20));
        return db + 3.0f * std::log2 (f / 1000.0f);
    };
    constexpr float specLo = -96.0f, specHi = -12.0f;
    auto specY = [&] (float db) { return juce::jmap (juce::jlimit (specLo, specHi, db), specLo, specHi, a.getBottom(), a.getY()); };

    juce::Path inPath, outPath;
    outPath.startNewSubPath (a.getX(), a.getBottom());
    for (int px = 0; px <= (int) a.getWidth(); px += 2)
    {
        const float f = xToFreq (a.getX() + (float) px, a);
        const float yi = specY (spectrumDbAt (specIn, f)), yo = specY (spectrumDbAt (specOut, f));
        if (px == 0) inPath.startNewSubPath (a.getX(), yi); else inPath.lineTo (a.getX() + (float) px, yi);
        outPath.lineTo (a.getX() + (float) px, yo);
    }
    outPath.lineTo (a.getRight(), a.getBottom());
    outPath.closeSubPath();
    g.setGradientFill (juce::ColourGradient (Colours::accent.withAlpha (0.26f), 0.0f, a.getY(), Colours::accent.withAlpha (0.03f), 0.0f, a.getBottom(), false));
    g.fillPath (outPath);
    g.setColour (Colours::input.brighter (0.5f).withAlpha (0.85f));
    g.strokePath (inPath, juce::PathStrokeType (1.3f));

    // ---- Response: EQ + filters ---------------------------------------------------------------------------
    const auto console = consoleFromState (state);
    const auto tol = processor.getTolerance();
    const auto eqp = eqFromState (state);
    const bool eqToKey = processor.meters.eqInSidechain.load();
    FilterModule filters;
    filters.prepare (sr);
    filters.setParams ({ value (ParamID::hpf), value (ParamID::lpf) }, console, tol);

    juce::Path eqPath, totalPath;
    for (int px = 0; px <= (int) a.getWidth(); px += 2)
    {
        const float f = xToFreq (a.getX() + (float) px, a);
        const double e = eqp.enabled ? EqModule::responseDb (sr, eqp, console, tol, f) : 0.0;
        const double fl = filters.magnitudeDb (f);
        const double total = (eqToKey ? 0.0 : e) + fl;
        const float ye = dbToY ((float) juce::jlimit (-60.0, 40.0, e), a);
        const float yt = dbToY ((float) juce::jlimit (-60.0, 40.0, total), a);
        if (px == 0) { eqPath.startNewSubPath (a.getX(), ye); totalPath.startNewSubPath (a.getX(), yt); }
        else { eqPath.lineTo (a.getX() + (float) px, ye); totalPath.lineTo (a.getX() + (float) px, yt); }
    }
    {
        juce::Path fill (totalPath);
        const float zero = dbToY (0.0f, a);
        fill.lineTo (a.getRight(), zero);
        fill.lineTo (a.getX(), zero);
        fill.closeSubPath();
        g.setColour (Colours::threshold.withAlpha (0.12f));
        g.fillPath (fill);
        g.setColour (console.analogue ? Colours::analogue : Colours::threshold);
        g.strokePath (totalPath, juce::PathStrokeType (2.0f));
    }
    if (eqToKey && eqp.enabled)
    {
        g.setColour (Colours::danger.withAlpha (0.9f));
        strokeDashed (g, eqPath, 1.6f, 5.0f, 4.0f);
        g.setFont (uiFont (10.0f, true));
        g.drawText ("EQ -> DYNAMICS KEY", a.withTrimmedTop (4.0f).withHeight (14.0f).withTrimmedRight (6.0f), juce::Justification::centredRight);
    }

    // ---- Nodes ------------------------------------------------------------------------------------------------
    auto drawNode = [&] (int n, juce::Colour c, const juce::String& label, bool sel, bool active)
    {
        const auto pos = nodePosition (n, a);
        const bool hov = n == hover || n == dragging;
        const float r = sel || hov ? 8.5f : 7.0f;
        g.setColour (sel ? c : Colours::panelLight);
        g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (pos));
        g.setColour (c.withAlpha (active ? 1.0f : 0.4f));
        g.drawEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (pos), 1.3f);
        g.setColour (sel ? Colours::background : (active ? Colours::text : Colours::textDim));
        g.setFont (uiFont (8.5f, true));
        g.drawText (label, juce::Rectangle<float> (20, 12).withCentre (pos), juce::Justification::centred);
    };
    drawNode (kEqBands, Colours::blue, "HP", false, value (ParamID::hpf) > 16.5f);
    drawNode (kEqBands + 1, Colours::blue, "LP", false, value (ParamID::lpf) < 21000.0f);
    for (int b = 0; b < kEqBands; ++b)
        drawNode (b, Colours::threshold, bandLabels[b], b == selected, eqp.enabled && std::abs (eqp.gain[(size_t) b]) > 0.01f);

    g.restoreState();
    g.setFont (uiFont (11.0f, true));
    g.setColour (Colours::text);
    g.drawText (console.analogue ? "EQ / SPECTRUM  -  " + juce::String (consoleTypeName (console.type)).toUpperCase() + " CH " + juce::String (console.channel)
                                 : juce::String ("EQ / SPECTRUM"),
                10, 3, 260, 18, juce::Justification::centredLeft);
    drawLegend (g, { 0, 3, getWidth() - 10, 18 },
                { { "IN", Colours::input }, { "OUT", Colours::accent }, { "EQ", console.analogue ? Colours::analogue : Colours::threshold },
                  { "FILTER", Colours::blue } });
}

//==============================================================================
DynamicsDisplay::DynamicsDisplay (StripProcessor& p) : processor (p)
{
    frames.resize ((size_t) capacity);
    setOpaque (true);
    setTooltip ("Left: the gate + compressor transfer curve with the live detector level. "
                "Right: input and output level, compressor gain reduction (red, hanging from 0 dB) and gate closing (shaded)");
}

void DynamicsDisplay::addFrame (const DynFrame& f)
{
    frames[(size_t) (count % capacity)] = f;
    ++count;
}

void DynamicsDisplay::paint (juce::Graphics& g)
{
    drawFrame (g, getLocalBounds().toFloat());
    auto& s = processor.getState();
    g.setFont (uiFont (11.0f, true));
    g.setColour (Colours::text);
    g.drawText ("DYNAMICS", 10, 3, 120, 18, juce::Justification::centredLeft);
    drawLegend (g, { 0, 3, getWidth() - 10, 18 },
                { { "IN", Colours::input }, { "OUT", Colours::accent }, { "GR", Colours::danger }, { "GATE", Colours::learn } });

    auto body = getLocalBounds().toFloat().withTrimmedTop (24.0f).withTrimmedBottom (16.0f).reduced (8.0f, 0.0f);
    const float side = juce::jmin (body.getHeight(), 230.0f);
    auto plot = body.removeFromLeft (side).withTrimmedLeft (22.0f);
    body.removeFromLeft (12.0f);
    auto hist = body;

    // ---- Transfer curve ------------------------------------------------------------------------------------
    constexpr float lo = -60.0f, hiIn = 0.0f, hiOut = 6.0f;
    auto xOf = [&] (float db) { return juce::jmap (juce::jlimit (lo, hiIn, db), lo, hiIn, plot.getX(), plot.getRight()); };
    auto yOf = [&] (float db) { return juce::jmap (juce::jlimit (lo, hiOut, db), lo, hiOut, plot.getBottom(), plot.getY()); };
    g.setFont (uiFont (9.0f));
    for (float db = lo; db <= 0.0f; db += 12.0f)
    {
        g.setColour (Colours::grid);
        g.drawVerticalLine ((int) xOf (db), plot.getY(), plot.getBottom());
        g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
        g.setColour (Colours::textDim);
        g.drawText (juce::String ((int) db), (int) plot.getX() - 24, (int) yOf (db) - 6, 20, 12, juce::Justification::centredRight);
        g.drawText (juce::String ((int) db), (int) xOf (db) - 12, (int) plot.getBottom() + 2, 24, 12, juce::Justification::centred);
    }
    g.setColour (Colours::edge);
    g.drawLine (xOf (lo), yOf (lo), xOf (0.0f), yOf (0.0f), 1.0f);

    GateParams gp;
    gp.enabled = raw (s, ParamID::gateOn) > 0.5f;
    gp.expander = juce::roundToInt (raw (s, ParamID::gateMode)) == 1;
    gp.thresholdDb = raw (s, ParamID::gateThr);
    gp.rangeDb = raw (s, ParamID::gateRange);
    const bool compOn = raw (s, ParamID::compOn) > 0.5f;
    const auto ch = (CompCharacter) juce::jlimit (0, 3, juce::roundToInt (raw (s, ParamID::compChar)));
    const float thr = raw (s, ParamID::compThr), ratio = raw (s, ParamID::compRatio);
    const float knee = CompressorModule::effectiveKnee (ch, raw (s, ParamID::compKnee));
    const float makeup = raw (s, ParamID::compMakeup) + processor.meters.autoMakeup.load();
    const float mix = raw (s, ParamID::compMix) * 0.01f;

    auto curve = [&] (float in)
    {
        float out = in;
        if (gp.enabled) out -= GateModule::staticReduction (gp, in);
        if (compOn)
        {
            const float red = CompressorModule::staticReduction (out, thr, CompressorModule::effectiveRatio (ch, ratio, out - thr), knee);
            const float wet = dbToGain (out - red + makeup), dry = dbToGain (out);
            out = gainToDb (dry + mix * (wet - dry));
        }
        return out;
    };
    juce::Path tp;
    for (int i = 0; i <= 120; ++i)
    {
        const float in = lo + (hiIn - lo) * (float) i / 120.0f;
        const float x = xOf (in), y = yOf (curve (in));
        if (i == 0) tp.startNewSubPath (x, y); else tp.lineTo (x, y);
    }
    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());
    if (compOn)
    {
        g.setColour (Colours::danger.withAlpha (0.5f));
        strokeDashed (g, juce::Path ([&] { juce::Path p; p.startNewSubPath (xOf (thr), plot.getY()); p.lineTo (xOf (thr), plot.getBottom()); return p; }()), 1.0f, 3.0f, 3.0f);
    }
    if (gp.enabled)
    {
        g.setColour (Colours::learn.withAlpha (0.6f));
        strokeDashed (g, juce::Path ([&] { juce::Path p; p.startNewSubPath (xOf (gp.thresholdDb), plot.getY()); p.lineTo (xOf (gp.thresholdDb), plot.getBottom()); return p; }()), 1.0f, 3.0f, 3.0f);
    }
    g.setColour (compOn || gp.enabled ? Colours::text : Colours::textDim);
    g.strokePath (tp, juce::PathStrokeType (2.0f));
    const float level = compOn ? processor.meters.compKey.load() : processor.meters.gateKey.load();
    if ((compOn || gp.enabled) && level > lo)
    {
        const juce::Point<float> dot (xOf (level), yOf (curve (level)));
        g.setColour (Colours::accent.withAlpha (0.3f));
        g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (dot));
        g.setColour (Colours::accent);
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (dot));
    }
    g.restoreState();
    g.setColour (Colours::edge);
    g.drawRect (plot, 1.0f);

    // ---- History ---------------------------------------------------------------------------------------------
    auto hy = [&] (float db) { return juce::jmap (juce::jlimit (lo, 0.0f, db), lo, 0.0f, hist.getBottom(), hist.getY()); };
    for (float db = lo; db <= 0.0f; db += 12.0f)
    {
        g.setColour (Colours::grid);
        g.drawHorizontalLine ((int) hy (db), hist.getX(), hist.getRight());
    }
    const int shown = (int) juce::jmin<int64_t> (count, (int64_t) capacity);
    if (shown > 1)
    {
        const float dx = hist.getWidth() / (float) (capacity - 1);
        juce::Path inP, outP, grP;
        outP.startNewSubPath (hist.getRight(), hist.getBottom());
        const float gateRange = juce::jmax (1.0f, gp.rangeDb);
        for (int k = 0; k < shown; ++k)
        {
            const auto& f = frames[(size_t) ((count - 1 - k) % capacity)];
            const float x = hist.getRight() - dx * (float) k;
            if (f.gateGrDb > 0.5f)
            {
                g.setColour (Colours::learn.withAlpha (0.25f * juce::jlimit (0.0f, 1.0f, f.gateGrDb / gateRange)));
                g.fillRect (x - dx, hist.getY(), dx + 0.5f, hist.getHeight());
            }
            if (k == 0) { inP.startNewSubPath (x, hy (f.inDb)); grP.startNewSubPath (x, hy (-f.compGrDb)); }
            else { inP.lineTo (x, hy (f.inDb)); grP.lineTo (x, hy (-f.compGrDb)); }
            outP.lineTo (x, hy (f.outDb));
        }
        outP.lineTo (hist.getRight() - dx * (float) (shown - 1), hist.getBottom());
        outP.closeSubPath();
        g.setColour (Colours::accent.withAlpha (0.35f));
        g.fillPath (outP);
        g.setColour (Colours::input.brighter (0.4f));
        g.strokePath (inP, juce::PathStrokeType (1.2f));
        g.setColour (Colours::danger);
        g.strokePath (grP, juce::PathStrokeType (1.6f));
    }
    g.setColour (Colours::textDim);
    g.setFont (uiFont (9.0f));
    g.drawText ("6 s", hist.withTop (hist.getBottom() + 2.0f).withHeight (12.0f), juce::Justification::centredLeft);
    g.drawText ("now", hist.withTop (hist.getBottom() + 2.0f).withHeight (12.0f), juce::Justification::centredRight);
    const float gr = processor.meters.comp.load();
    g.setColour (Colours::danger);
    g.setFont (uiFont (10.0f, true));
    g.drawText ("GR " + juce::String (gr, 1) + " dB", plot.reduced (6.0f, 4.0f).withHeight (14.0f), juce::Justification::centredLeft);
}

//==============================================================================
VuMeters::VuMeters (StripProcessor& p) : processor (p)
{
    setOpaque (true);
    setTooltip ("VU meters: input (after the trim) and output. 0 VU = -18 dBFS, the level an analogue desk is happiest at. "
                "The red light shows peaks above -1 dBFS");
}

void VuMeters::tick()
{
    auto toVu = [] (float rmsDb) { return rmsDb + 3.01f + 18.0f; };
    auto spring = [] (float& pos, float& vel, float target)
    {
        // light mechanical damping on top of the 300 ms VU integration
        vel = 0.55f * vel + 0.35f * (target - pos);
        pos += vel;
    };
    spring (needleIn, velIn, juce::jlimit (-25.0f, 6.0f, toVu (processor.meters.vuIn.load())));
    spring (needleOut, velOut, juce::jlimit (-25.0f, 6.0f, toVu (processor.meters.vuOut.load())));
    const float ip = processor.meters.inPeak.load(), op = processor.meters.outPeak.load();
    peakIn = ip > 0.891f ? 1.0f : juce::jmax (0.0f, peakIn - 0.04f);
    peakOut = op > 0.891f ? 1.0f : juce::jmax (0.0f, peakOut - 0.04f);
    repaint();
}

void VuMeters::drawMeter (juce::Graphics& g, juce::Rectangle<float> r, float vu, float peak, const juce::String& label, bool analogue)
{
    const auto face = analogue ? Colours::vuFace : Colours::panelLight.darker (0.25f);
    const auto ink = analogue ? juce::Colour (0xff2b2118) : Colours::textDim;
    const auto red = analogue ? juce::Colour (0xffc0392b) : Colours::danger;
    g.setColour (Colours::edge);
    g.fillRoundedRectangle (r, 5.0f);
    auto f = r.reduced (3.0f);
    if (analogue)
    {
        juce::ColourGradient glow (face.brighter (0.1f), f.getCentreX(), f.getBottom(), face.darker (0.18f), f.getCentreX(), f.getY(), true);
        g.setGradientFill (glow);
    }
    else
        g.setColour (face);
    g.fillRoundedRectangle (f, 4.0f);

    const juce::Point<float> pivot (f.getCentreX(), f.getBottom() + f.getHeight() * 0.25f);
    const float radius = f.getHeight() * 1.05f;
    const float span = 0.92f;
    auto angleOf = [&] (float v)
    {
        const float pos = juce::jlimit (-0.04f, 1.04f, std::pow (10.0f, v / 20.0f) / 1.41254f); // linear voltage scale, +3 VU = full
        return -span + 2.0f * span * pos;
    };
    auto pointAt = [&] (float angle, float rad) { return pivot + juce::Point<float> (std::sin (angle) * rad, -std::cos (angle) * rad); };

    g.saveState();
    g.reduceClipRegion (f.toNearestInt());
    // red zone
    juce::Path redArc;
    redArc.addCentredArc (pivot.x, pivot.y, radius * 0.86f, radius * 0.86f, 0.0f, angleOf (0.0f), angleOf (3.0f), true);
    g.setColour (red.withAlpha (0.85f));
    g.strokePath (redArc, juce::PathStrokeType (4.0f));
    juce::Path arc;
    arc.addCentredArc (pivot.x, pivot.y, radius * 0.86f, radius * 0.86f, 0.0f, angleOf (-20.0f), angleOf (0.0f), true);
    g.setColour (ink);
    g.strokePath (arc, juce::PathStrokeType (1.2f));

    g.setFont (uiFont (9.5f, true));
    for (int iv : { -20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3 })
    {
        const float v = (float) iv;
        const float ang = angleOf (v);
        const bool major = iv == -20 || iv == -10 || iv == -7 || iv == -5 || iv == -3 || iv == 0 || iv == 3;
        g.setColour (v > 0.0f ? red : ink);
        g.drawLine ({ pointAt (ang, radius * 0.86f), pointAt (ang, radius * (major ? 0.95f : 0.91f)) }, major ? 1.6f : 1.0f);
        if (major)
        {
            const auto t = pointAt (ang, radius * 1.03f);
            g.drawText (v > 0.0f ? "+" + juce::String ((int) v) : juce::String ((int) v), juce::Rectangle<float> (28.0f, 12.0f).withCentre (t),
                        juce::Justification::centred);
        }
    }
    g.setColour (ink);
    g.setFont (uiFont (15.0f, true));
    g.drawText ("VU", juce::Rectangle<float> (60.0f, 18.0f).withCentre ({ f.getCentreX() - radius * 0.32f, f.getBottom() - radius * 0.2f }), juce::Justification::centred);
    g.setFont (uiFont (10.0f, true));
    g.drawText (label, f.reduced (8.0f, 5.0f), juce::Justification::bottomLeft);
    g.setFont (uiFont (9.5f));
    g.drawText (juce::String (vu, 1), f.reduced (8.0f, 5.0f), juce::Justification::bottomRight);

    // needle
    const float ang = angleOf (vu);
    g.setColour (analogue ? juce::Colour (0xff1a1410) : Colours::accent);
    g.drawLine ({ pointAt (ang, radius * 0.25f), pointAt (ang, radius * 0.97f) }, 1.8f);
    g.restoreState();

    // peak light
    const auto led = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ f.getRight() - 12.0f, f.getY() + 12.0f });
    g.setColour (red.withAlpha (0.2f + 0.8f * peak));
    g.fillEllipse (led);
    g.setColour (ink.withAlpha (0.5f));
    g.drawEllipse (led, 0.8f);
}

void VuMeters::paint (juce::Graphics& g)
{
    drawFrame (g, getLocalBounds().toFloat());
    const bool analogue = juce::roundToInt (raw (processor.getState(), ParamID::mode)) == 1;
    g.setFont (uiFont (11.0f, true));
    g.setColour (Colours::text);
    g.drawText ("VU", 10, 3, 60, 18, juce::Justification::centredLeft);
    g.setColour (analogue ? Colours::analogue : Colours::textDim);
    g.setFont (uiFont (10.0f, true));
    g.drawText (analogue ? "ANALOGUE   0 VU = -18 dBFS" : "DIGITAL   0 VU = -18 dBFS", 0, 3, getWidth() - 10, 18, juce::Justification::centredRight);

    auto r = getLocalBounds().toFloat().withTrimmedTop (24.0f).reduced (8.0f, 6.0f);
    const float h = (r.getHeight() - 8.0f) / 2.0f;
    drawMeter (g, r.removeFromTop (h), needleIn, peakIn, "IN", analogue);
    r.removeFromTop (8.0f);
    drawMeter (g, r, needleOut, peakOut, "OUT", analogue);
}

//==============================================================================
int ChainStrip::pageOf (ModuleId m)
{
    switch (m)
    {
        case ModuleId::gate: case ModuleId::comp: case ModuleId::eq: return pageMain;
        case ModuleId::transient: case ModuleId::deesser: case ModuleId::drive: return pageColour;
        case ModuleId::numModules: break;
    }
    return pageMain;
}

ChainStrip::ChainStrip (StripProcessor& p) : processor (p), state (p.getState())
{
    order = processor.getModuleOrder();
    rebuildTiles();
    setTooltip ("Click a stage to edit it, click its dot to switch it on/off, drag the modules to change their order");
}

void ChainStrip::rebuildTiles()
{
    tiles.clear();
    tiles.push_back ({ "input", "INPUT", {}, Colours::textDim, -1, -1, {} });
    for (int m : order)
        tiles.push_back ({ juce::String ("m") + juce::String (m), moduleShortName ((ModuleId) m), moduleEnableID ((ModuleId) m),
                           moduleColour ((ModuleId) m), pageOf ((ModuleId) m), m, {} });
    tiles.push_back ({ "output", "OUTPUT", {}, Colours::textDim, -1, -1, {} });
    layoutTiles();
}

void ChainStrip::layoutTiles()
{
    const float gap = 12.0f;
    const float w = ((float) getWidth() - gap * (float) (tiles.size() - 1)) / (float) tiles.size();
    float x = 0.0f;
    for (auto& t : tiles)
    {
        t.bounds = { x, 0.0f, w, (float) getHeight() };
        x += w + gap;
    }
}

juce::Rectangle<float> ChainStrip::powerRect (const Tile& t) const
{
    return { t.bounds.getX() + 7.0f, t.bounds.getY() + 8.0f, 11.0f, 11.0f };
}

bool ChainStrip::tileEnabled (const Tile& t) const
{
    return t.enableId.isEmpty() || state.getRawParameterValue (t.enableId)->load() > 0.5f;
}

juce::String ChainStrip::tileValue (const Tile& t) const
{
    auto gr = [] (float v) { return (v > 0.05f ? "-" : "") + juce::String (std::abs (v), 1) + " dB"; };
    auto value = [this] (const char* id) { return state.getRawParameterValue (id)->load(); };
    auto signedDb = [] (float v) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; };
    auto& m = processor.meters;
    if (t.key == "input")
    {
        juce::String s = signedDb (value (ParamID::inGain));
        if (value (ParamID::hpf) > 16.5f) s << "  HP " << juce::roundToInt (value (ParamID::hpf));
        return s;
    }
    if (t.key == "output")
    {
        const bool analogue = juce::roundToInt (value (ParamID::mode)) == 1;
        return signedDb (value (ParamID::outGain)) + (analogue ? "  ANA" : "");
    }
    if (t.module < 0)
        return {};
    switch ((ModuleId) t.module)
    {
        case ModuleId::gate:      return m.gateOpen.load() ? juce::String ("open") : gr (m.gate.load());
        case ModuleId::transient: return signedDb (m.transient.load());
        case ModuleId::comp:      return juce::String (compCharacterName ((CompCharacter) juce::jlimit (0, 3, juce::roundToInt (value (ParamID::compChar)))))
                                         + "  " + gr (m.comp.load());
        case ModuleId::eq:
        {
            if (m.eqInSidechain.load())
                return "-> KEY";
            int active = 0;
            for (int b = 0; b < kEqBands; ++b) active += std::abs (value (BandIDs::eq[b][1])) > 0.05f ? 1 : 0;
            return juce::String (active) + (active == 1 ? " band" : " bands");
        }
        case ModuleId::deesser:   return gr (m.deess.load());
        case ModuleId::drive:     return driveTypeName ((DriveType) juce::jlimit (0, 3, juce::roundToInt (value (ParamID::drvType))));
        case ModuleId::numModules: break;
    }
    return {};
}

void ChainStrip::tick()
{
    const auto o = processor.getModuleOrder();
    if (o != order && dragTile < 0)
    {
        order = o;
        rebuildTiles();
    }
    repaint();
}

void ChainStrip::paint (juce::Graphics& g)
{
    if (tiles.empty())
        return;
    if (tiles.front().bounds.getWidth() <= 0.0f)
        layoutTiles();

    for (size_t i = 0; i + 1 < tiles.size(); ++i)
    {
        const auto& a = tiles[i].bounds;
        const float x = a.getRight() + 6.0f, y = a.getCentreY();
        juce::Path arrow;
        arrow.startNewSubPath (x - 3.0f, y - 4.0f);
        arrow.lineTo (x + 1.5f, y);
        arrow.lineTo (x - 3.0f, y + 4.0f);
        g.setColour (Colours::textDim.withAlpha (0.6f));
        g.strokePath (arrow, juce::PathStrokeType (1.4f));
    }

    {
        float x0 = 1.0e9f, x1 = -1.0e9f;
        for (auto& t : tiles)
            if (t.module >= 0) { x0 = std::min (x0, t.bounds.getX()); x1 = std::max (x1, t.bounds.getRight()); }
        g.setColour (Colours::edge);
        g.drawHorizontalLine (getHeight() - 1, x0, x1);
    }

    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const auto& t = tiles[i];
        auto b = t.bounds;
        if ((int) i == dragTile)
            b = b.withX (dragX - dragOffset);
        const bool on = tileEnabled (t);
        const bool hov = (int) i == hoverTile;

        g.setColour ((int) i == dragTile ? Colours::panelLight.brighter (0.15f) : (hov ? Colours::panelLight.brighter (0.06f) : Colours::panelLight));
        g.fillRoundedRectangle (b.reduced (0.5f, 1.5f), 5.0f);
        g.setColour (on ? t.colour.withAlpha (0.8f) : Colours::edge);
        g.drawRoundedRectangle (b.reduced (0.5f, 1.5f), 5.0f, on ? 1.2f : 1.0f);
        if (on)
        {
            g.setColour (t.colour);
            g.fillRoundedRectangle (b.getX() + 6.0f, b.getBottom() - 5.0f, b.getWidth() - 12.0f, 2.0f, 1.0f);
        }
        if (t.enableId.isNotEmpty())
        {
            auto pr = powerRect (t).withX (b.getX() + 7.0f);
            g.setColour (on ? t.colour : Colours::graphBg);
            g.fillEllipse (pr);
            g.setColour (on ? t.colour.brighter (0.3f) : Colours::textDim);
            g.drawEllipse (pr, 1.0f);
        }
        g.setFont (uiFont (11.0f, true));
        g.setColour (on ? Colours::text : Colours::textDim);
        g.drawText (t.name, b.withTrimmedLeft (t.enableId.isNotEmpty() ? 20.0f : 6.0f).withHeight (26.0f).translated (0.0f, 2.0f),
                    t.enableId.isNotEmpty() ? juce::Justification::centredLeft : juce::Justification::centred);
        g.setFont (uiFont (10.0f));
        g.setColour (on ? t.colour.withAlpha (0.9f) : Colours::textDim.withAlpha (0.6f));
        g.drawText (on ? tileValue (t) : juce::String ("off"), b.withTrimmedTop (22.0f).withTrimmedBottom (6.0f).reduced (6.0f, 0.0f),
                    juce::Justification::centred);
    }

    if (dragTile >= 0 && dragTarget >= 0)
    {
        const auto& t = tiles[(size_t) dragTarget];
        const float x = dragTarget > dragTile ? t.bounds.getRight() + 6.0f : t.bounds.getX() - 6.0f;
        g.setColour (Colours::accent);
        g.fillRoundedRectangle (x - 1.5f, 2.0f, 3.0f, (float) getHeight() - 4.0f, 1.5f);
    }
}

void ChainStrip::mouseMove (const juce::MouseEvent& e)
{
    int h = -1;
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].bounds.contains (e.position)) h = (int) i;
    if (h != hoverTile)
    {
        hoverTile = h;
        setMouseCursor (h >= 0 && tiles[(size_t) h].module >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }
}

void ChainStrip::mouseDown (const juce::MouseEvent& e)
{
    dragTile = -1;
    dragMoved = false;
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const auto& t = tiles[i];
        if (! t.bounds.contains (e.position))
            continue;
        if (t.enableId.isNotEmpty() && powerRect (t).expanded (4.0f).contains (e.position))
        {
            if (auto* p = state.getParameter (t.enableId))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
                p->endChangeGesture();
            }
            return;
        }
        dragTile = (int) i;
        dragOffset = e.position.x - t.bounds.getX();
        dragX = e.position.x;
        return;
    }
}

void ChainStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (dragTile < 0 || tiles[(size_t) dragTile].module < 0)
        return;
    if (! dragMoved && std::abs (e.getDistanceFromDragStartX()) < 5)
        return;
    dragMoved = true;
    dragX = e.position.x;
    dragTarget = -1;
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].module >= 0 && e.position.x >= tiles[i].bounds.getX() - 6.0f && e.position.x <= tiles[i].bounds.getRight() + 6.0f)
            dragTarget = (int) i;
    if (dragTarget < 0)
    {
        int firstM = -1, lastM = -1;
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i].module >= 0) { if (firstM < 0) firstM = (int) i; lastM = (int) i; }
        dragTarget = e.position.x < tiles[(size_t) firstM].bounds.getX() ? firstM : lastM;
    }
    repaint();
}

void ChainStrip::mouseUp (const juce::MouseEvent&)
{
    if (dragTile >= 0 && ! dragMoved)
    {
        if (onTileClicked)
            onTileClicked (tiles[(size_t) dragTile].page, tiles[(size_t) dragTile].key);
    }
    else if (dragTile >= 0 && dragTarget >= 0 && dragTarget != dragTile)
    {
        int firstM = -1;
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i].module >= 0) { firstM = (int) i; break; }
        const int from = dragTile - firstM, to = dragTarget - firstM;
        std::vector<int> o (order.begin(), order.end());
        const int moved = o[(size_t) from];
        o.erase (o.begin() + from);
        o.insert (o.begin() + to, moved);
        for (int i = 0; i < kNumOrderable; ++i) order[(size_t) i] = o[(size_t) i];
        processor.setModuleOrder (order);
        rebuildTiles();
        if (onOrderChanged)
            onOrderChanged();
    }
    dragTile = dragTarget = -1;
    dragMoved = false;
    repaint();
}
} // namespace lacs
