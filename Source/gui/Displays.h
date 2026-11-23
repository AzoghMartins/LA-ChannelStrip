#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "../dsp/StripEngine.h"
#include "Theme.h"

namespace lacs
{
class StripProcessor;

/** Colour used for each orderable module. */
juce::Colour moduleColour (ModuleId m);

//==============================================================================
/** Input / output spectrum with the EQ and filter curves. EQ nodes and the HP / LP nodes can be dragged. */
class SpectrumDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SpectrumDisplay (StripProcessor&);

    void pullSpectrum();
    void setSelectedBand (int b) { selected = b; repaint(); }
    std::function<void (int)> onBandSelected;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    /** Node index: 0..3 = EQ bands, 4 = high-pass, 5 = low-pass, -1 = none. */
    juce::Rectangle<float> plotArea() const;
    float freqToX (float f, juce::Rectangle<float> a) const;
    float xToFreq (float x, juce::Rectangle<float> a) const;
    float dbToY (float db, juce::Rectangle<float> a) const;
    float yToDb (float y, juce::Rectangle<float> a) const;
    int hitNode (juce::Point<float> p) const;
    juce::Point<float> nodePosition (int node, juce::Rectangle<float> a) const;
    float value (const char* id) const;
    void analyse (std::vector<float>& fifo, int& fifoPos, std::vector<float>& smoothed);

    StripProcessor& processor;
    juce::AudioProcessorValueTreeState& state;

    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> fifoIn, fifoOut, fftData, window, specIn, specOut;
    int posIn = 0, posOut = 0;

    int selected = 1, dragging = -1, hover = -1;
    static constexpr float minDb = -20.0f, maxDb = 20.0f;
};

//==============================================================================
/** Gate + compressor transfer curve with the live detector level, and a scrolling level / gain-reduction history. */
class DynamicsDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DynamicsDisplay (StripProcessor&);
    void addFrame (const DynFrame& f);
    void paint (juce::Graphics&) override;

private:
    StripProcessor& processor;
    static constexpr int capacity = 600; // 6 s at 100 frames per second
    std::vector<DynFrame> frames;
    int64_t count = 0;
};

//==============================================================================
/** Two VU meters (input after trim, output). 0 VU = -18 dBFS. The face lights up warm in Analogue mode. */
class VuMeters : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit VuMeters (StripProcessor&);
    void tick();
    void paint (juce::Graphics&) override;

private:
    void drawMeter (juce::Graphics&, juce::Rectangle<float> area, float vu, float peakHold, const juce::String& label, bool analogue);

    StripProcessor& processor;
    float needleIn = -20.0f, needleOut = -20.0f, velIn = 0.0f, velOut = 0.0f;
    float peakIn = 0.0f, peakOut = 0.0f;
};

//==============================================================================
/** Signal-flow strip: every stage as a tile. The module tiles can be dragged to change their order. */
class ChainStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum Page { pageMain = 0, pageColour = 1 };

    explicit ChainStrip (StripProcessor& p);
    void tick();
    void paint (juce::Graphics&) override;
    void resized() override { layoutTiles(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** Called with (page, sectionKey) when a tile is clicked (page -1 = stay on the current page). */
    std::function<void (int, const juce::String&)> onTileClicked;
    std::function<void()> onOrderChanged;

    static int pageOf (ModuleId m);

private:
    struct Tile
    {
        juce::String key, name, enableId;
        juce::Colour colour;
        int page = 0;
        int module = -1; // orderable ModuleId, -1 = fixed
        juce::Rectangle<float> bounds;
    };

    void rebuildTiles();
    void layoutTiles();
    juce::String tileValue (const Tile& t) const;
    bool tileEnabled (const Tile& t) const;
    juce::Rectangle<float> powerRect (const Tile& t) const;

    StripProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    std::vector<Tile> tiles;
    ModuleOrder order;
    int dragTile = -1, dragTarget = -1, hoverTile = -1;
    float dragX = 0.0f, dragOffset = 0.0f;
    bool dragMoved = false;
};
} // namespace lacs
