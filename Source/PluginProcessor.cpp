#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PadSampleData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
const char* findBundledResource (const juce::String& fileName, int& size)
{
    // Resolve original filenames instead of guessing JUCE's C++ identifier rules.
    const auto basename = fileName.fromLastOccurrenceOf ("/", false, false);
    for (int i = 0; i < PadSamples::namedResourceListSize; ++i)
    {
        if (basename == PadSamples::originalFilenames[i])
            return PadSamples::getNamedResource (PadSamples::namedResourceList[i], size);
    }

    size = 0;
    return nullptr;
}
}

juce::AudioProcessorValueTreeState::ParameterLayout SculptPadProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout parameters;
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "morph", 1 }, "Morph",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("%")));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    return parameters;
}

SculptPadProcessor::SculptPadProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SCULPT_PAD_STATE", createLayout())
{
    morphParameter = apvts.getRawParameterValue ("morph");
    outputParameter = apvts.getRawParameterValue ("output");
    loadBundledSamples();
}

void SculptPadProcessor::loadBundledSamples()
{
    // All decoding and allocation happens before playback. The engine's bank is
    // immutable afterwards; the audio callback never loads files or takes a lock.
    int manifestSize = 0;
    const auto* manifestData = findBundledResource ("manifest.json", manifestSize);
    if (manifestData == nullptr || manifestSize <= 0)
    {
        sampleMessage = "Libreria campioni mancante";
        return;
    }

    juce::var manifest;
    const auto parsed = juce::JSON::parse (
        juce::String::fromUTF8 (manifestData, manifestSize), manifest);
    const auto sampleFiles = manifest.getProperty ("sampleFiles", {});
    const auto* entries = sampleFiles.getArray();
    if (parsed.failed() || entries == nullptr || entries->isEmpty())
    {
        sampleMessage = "Manifest della libreria non valido";
        return;
    }

    juce::WavAudioFormat wav;
    std::vector<sculptpad::Sample> samples;
    samples.reserve (static_cast<std::size_t> (entries->size()));
    constexpr int modelledRoundRobinCount = 4;

    for (const auto& entry : *entries)
    {
        if (! entry.hasProperty ("file") || ! entry.hasProperty ("rootNote")
            || ! entry.hasProperty ("roundRobin") || ! entry.hasProperty ("loopStart")
            || ! entry.hasProperty ("loopEnd") || ! entry.hasProperty ("loopCrossfade"))
        {
            sampleMessage = "Mappatura campioni incompleta";
            return;
        }

        // Refuse malformed metadata before conversion to unsigned frame offsets.
        // JSON integral fields are int/int64, including every factory manifest.
        const auto isInteger = [] (const juce::var& value)
        {
            return value.isInt() || value.isInt64();
        };
        if (! entry["file"].isString() || ! isInteger (entry["rootNote"])
            || ! isInteger (entry["roundRobin"]) || ! isInteger (entry["loopStart"])
            || ! isInteger (entry["loopEnd"]) || ! isInteger (entry["loopCrossfade"]))
        {
            sampleMessage = "Formato della mappatura non valido";
            return;
        }

        const auto filename = entry["file"].toString();
        int dataSize = 0;
        const auto* data = findBundledResource (filename, dataSize);
        if (data == nullptr || dataSize <= 0)
        {
            sampleMessage = "Campione mancante: " + filename;
            return;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::MemoryInputStream (
                data, static_cast<std::size_t> (dataSize), false), true));
        if (reader == nullptr || reader->lengthInSamples < 4
            || reader->lengthInSamples > 20000000 || reader->numChannels == 0
            || reader->numChannels > 2
            || ! std::isfinite (reader->sampleRate)
            || reader->sampleRate < 1000.0 || reader->sampleRate > 768000.0)
        {
            sampleMessage = "Campione non valido: " + filename;
            return;
        }

        const int length = static_cast<int> (reader->lengthInSamples);
        juce::AudioBuffer<float> decoded (static_cast<int> (reader->numChannels), length);
        if (! reader->read (&decoded, 0, length, 0, true, true))
        {
            sampleMessage = "Lettura campione fallita: " + filename;
            return;
        }

        sculptpad::Sample sample;
        const auto rootNote = static_cast<juce::int64> (entry["rootNote"]);
        const auto roundRobin = static_cast<juce::int64> (entry["roundRobin"]);
        const auto loopStart = static_cast<juce::int64> (entry["loopStart"]);
        const auto loopEnd = static_cast<juce::int64> (entry["loopEnd"]);
        const auto loopCrossfade = static_cast<juce::int64> (entry["loopCrossfade"]);
        if (rootNote < 0 || rootNote > 127 || roundRobin < 0 || roundRobin > 127
            || loopStart < 0 || loopStart >= length
            || loopEnd <= loopStart + 3 || loopEnd > length
            || loopCrossfade < 2 || loopCrossfade > (loopEnd - loopStart) / 2)
        {
            sampleMessage = "Valori di mappatura non validi: " + filename;
            return;
        }
        sample.rootNote = static_cast<int> (rootNote);
        sample.roundRobin = static_cast<int> (roundRobin);
        sample.sampleRate = reader->sampleRate;
        sample.loopStart = static_cast<std::size_t> (loopStart);
        sample.loopEnd = static_cast<std::size_t> (loopEnd);
        sample.loopCrossfade = static_cast<std::size_t> (loopCrossfade);

        const auto* left = decoded.getReadPointer (0);
        const auto* right = decoded.getNumChannels() > 1 ? decoded.getReadPointer (1) : left;
        bool nonzero = false;
        for (int frame = 0; frame < length; ++frame)
        {
            if (! std::isfinite (left[frame]) || ! std::isfinite (right[frame]))
            {
                sampleMessage = "Dati audio non validi: " + filename;
                return;
            }
            nonzero = nonzero || std::abs (left[frame]) > 1.0e-9f
                              || std::abs (right[frame]) > 1.0e-9f;
        }
        if (! nonzero)
        {
            sampleMessage = "Campione silenzioso: " + filename;
            return;
        }
        sample.left.assign (left, left + length);
        sample.right.assign (right, right + length);
        samples.push_back (std::move (sample));
    }

    const auto expectedSampleCount = samples.size();
    engine.setSamples (std::move (samples));
    samplesReady = engine.hasSamples() && engine.sampleCount() == expectedSampleCount;
    roundRobinCount = modelledRoundRobinCount;
    sampleMessage = samplesReady ? juce::String() : juce::String ("Libreria campioni incompleta o non valida");
}

bool SculptPadProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SculptPadProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    // Install restored targets before prepare() snaps its smoothing state.
    // The first note therefore starts at the saved timbre and output level.
    engine.setMorph (morphParameter->load (std::memory_order_relaxed) * 0.01f);
    engine.setOutputGain (juce::Decibels::decibelsToGain (
        outputParameter->load (std::memory_order_relaxed), -60.0f));
    engine.prepare (sampleRate);
    meterLevel.store (0.0f, std::memory_order_relaxed);
    roundRobinStep.store (-1, std::memory_order_relaxed);
}

void SculptPadProcessor::releaseResources()
{
    engine.reset();
    meterLevel.store (0.0f, std::memory_order_relaxed);
    roundRobinStep.store (-1, std::memory_order_relaxed);
}

void SculptPadProcessor::handleMidi (const juce::MidiMessage& message)
{
    if (message.isNoteOn())
        engine.noteOn (message.getNoteNumber(), message.getFloatVelocity(), message.getChannel());
    else if (message.isNoteOff())
        engine.noteOff (message.getNoteNumber(), message.getChannel());
    else if (message.isAllSoundOff())
        engine.allSoundOff (message.getChannel());
    else if (message.isAllNotesOff())
        engine.allNotesOff (message.getChannel());
    else if (message.isPitchWheel())
    {
        constexpr float rangeInSemitones = 2.0f;
        const int value = message.getPitchWheelValue() - 8192;
        engine.pitchBend (rangeInSemitones * static_cast<float> (value)
                          / (value >= 0 ? 8191.0f : 8192.0f), message.getChannel());
    }
    else if (message.isController() && message.getControllerNumber() == 64)
        engine.sustainPedal (message.getControllerValue() >= 64, message.getChannel());
    else if (message.isController() && message.getControllerNumber() == 121)
        engine.resetControllers (message.getChannel());
}

void SculptPadProcessor::renderSegment (juce::AudioBuffer<float>& audio, int start, int length)
{
    if (length > 0)
        engine.render (audio.getWritePointer (0, start), audio.getWritePointer (1, start), length);
}

void SculptPadProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();
    if (audio.getNumChannels() < 2 || ! samplesReady)
    {
        midi.clear();
        meterLevel.store (0.0f, std::memory_order_relaxed);
        return;
    }

    engine.setMorph (morphParameter->load (std::memory_order_relaxed) * 0.01f);
    engine.setOutputGain (juce::Decibels::decibelsToGain (
        outputParameter->load (std::memory_order_relaxed), -60.0f));

    int cursor = 0;
    const int blockLength = audio.getNumSamples();
    for (const auto metadata : midi)
    {
        const int position = juce::jlimit (cursor, blockLength, metadata.samplePosition);
        renderSegment (audio, cursor, position - cursor);
        // Channel messages fit in MidiMessage's inline storage. Ignore SysEx
        // before constructing a message so it cannot allocate on the audio thread.
        if (metadata.numBytes <= 3)
            handleMidi (metadata.getMessage());
        cursor = position;
    }
    renderSegment (audio, cursor, blockLength - cursor);
    midi.clear();

    const float peak = blockLength > 0 ? audio.getMagnitude (0, blockLength) : 0.0f;
    meterLevel.store (peak, std::memory_order_relaxed);
    roundRobinStep.store (engine.lastRoundRobin(), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* SculptPadProcessor::createEditor()
{
    return new SculptPadEditor (*this);
}

void SculptPadProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    state.setProperty ("schemaVersion", 1, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void SculptPadProcessor::setStateInformation (const void* data, int size)
{
    if (const auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SculptPadProcessor();
}
