#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "Displays.h"
#include "Theme.h"
#include "Widgets.h"

namespace lacs
{
/** A titled column of controls inside a section (used for the EQ bands). */
class BandStrip : public juce::Component
{
public:
    BandStrip (const juce::String& title, juce::Colour colour);
    void paint (juce::Graphics&) override;
    void resized() override;
    void setItems (std::vector<juce::Component*> items, int columns) { content = std::move (items); cols = columns; resized(); }
    /** Optional small control in the header (e.g. a BELL switch). */
    void setHeaderItem (juce::Component* c) { header = c; resized(); }

private:
    juce::String title;
    juce::Colour colour;
    std::vector<juce::Component*> content;
    juce::Component* header = nullptr;
    int cols = 1;
};

/** All UI content, laid out at a fixed base size and scaled by the editor. */
class MainPanel : public juce::Component
{
public:
    static constexpr int baseWidth = 1440, baseHeight = 780;

    explicit MainPanel (StripProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();

    void setPage (int page);
    int getPage() const { return currentPage; }
    void showToast (const juce::String& text);
    static void grid (std::vector<juce::Component*> items, int columns, juce::Rectangle<int> area);

private:
    Knob* knob (juce::Component& parent, const char* id, const juce::String& name, juce::Colour c, bool bipolar = false);
    Choice* choice (juce::Component& parent, const char* id, const juce::String& caption = {});
    ParamPill* pill (juce::Component& parent, const char* id, const juce::String& label, juce::Colour c);
    void refreshPresetList();
    void focusSection (const juce::String& key);
    void setMode (bool analogue);

    StripProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    std::vector<std::unique_ptr<juce::Component>> owned;

    // Header
    juce::ComboBox presetBox;
    PillButton saveButton { "SAVE" }, assistButton { "ASSIST", Colours::learn };
    PillButton digitalButton { "DIGITAL", Colours::accent }, analogueButton { "ANALOGUE", Colours::analogue };
    PillButton slotA { "A", Colours::detection }, slotB { "B", Colours::detection }, copyButton { "A > B" };
    juce::File userPresetFiles[256];
    int numUserPresets = 0;

    // Displays
    ChainStrip chain;
    SpectrumDisplay spectrum;
    DynamicsDisplay dynamics;
    VuMeters vu;
    LevelMeters meters;

    // Pages
    PillButton tabMain { "EQ & DYNAMICS" }, tabColour { "COLOUR & CONSOLE" };
    int currentPage = 0;

    Section inputSection, outputSection;
    Section eqSection, gateSection, compSection;
    Section trSection, dsSection, drvSection, consoleSection;
    std::map<juce::String, Section*> sectionByKey;

    std::vector<Knob*> inputKnobs, outputKnobs, gateKnobs, compKnobs, trKnobs, dsKnobs, drvKnobs, consoleKnobs;
    ParamPill* polarityPill = nullptr;
    Choice *gateMode = nullptr, *gateKey = nullptr, *compChar = nullptr, *compKey = nullptr;
    ParamPill *gateListen = nullptr, *compAutoRel = nullptr, *compAutoGain = nullptr, *dsListen = nullptr;
    Choice *dsMode = nullptr, *drvType = nullptr, *consoleType = nullptr;
    std::array<std::unique_ptr<BandStrip>, kEqBands> eqStrips;
    juce::Label consoleInfo;

    int blink = 0, toastTicks = 0;
    juce::String infoText, toastText;
    juce::String highlightKey;
    float highlightAmount = 0.0f;
};

class StripEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit StripEditor (StripProcessor&);
    ~StripEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Pulls new data from the audio thread and refreshes the displays (called at 30 Hz). */
    void updateDisplays() { panel.tick(); }
    MainPanel& getPanel() { return panel; }

private:
    void timerCallback() override { updateDisplays(); }

    StripLookAndFeel lookAndFeel;
    MainPanel panel;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StripEditor)
};
} // namespace lacs
