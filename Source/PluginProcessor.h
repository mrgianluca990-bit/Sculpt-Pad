#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "PadEngine.h"

class SculptPadProcessor final : public juce::AudioProcessor
{
public:
    SculptPadProcessor();
    ~SculptPadProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Sculpt Pad"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 6.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    bool areSamplesReady() const noexcept { return samplesReady; }
    const juce::String& getSampleMessage() const noexcept { return sampleMessage; }
    int getRoundRobinCount() const noexcept { return roundRobinCount; }

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> meterLevel { 0.0f };
    std::atomic<int> roundRobinStep { -1 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void loadBundledSamples();
    void handleMidi (const juce::MidiMessage&);
    void renderSegment (juce::AudioBuffer<float>&, int start, int length);

    sculptpad::PadEngine engine;
    std::atomic<float>* morphParameter = nullptr;
    std::atomic<float>* outputParameter = nullptr;

    // These are written only during construction, before audio or UI access.
    bool samplesReady = false;
    int roundRobinCount = 0;
    juce::String sampleMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptPadProcessor)
};
