#pragma once
#include "PluginProcessor.h"

struct SculptPadLookAndFeel : public juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float a0, float a1, juce::Slider&) override;
};

class SculptPadEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SculptPadEditor (SculptPadProcessor&);
    ~SculptPadEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawVU (juce::Graphics&, juce::Rectangle<float>);

    SculptPadProcessor& proc;
    SculptPadLookAndFeel laf;
    juce::Slider morph, volume;
    juce::AudioProcessorValueTreeState::SliderAttachment morphAtt, volumeAtt;
    juce::TooltipWindow tooltip { this, 700 };
    float needle = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptPadEditor)
};
