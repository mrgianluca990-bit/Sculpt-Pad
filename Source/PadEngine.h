#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace sculptpad {

struct Sample {
    std::vector<float> left, right;
    double sampleRate = 44100.0;
    int rootNote = 60, roundRobin = 0;
    std::size_t loopStart = 0, loopEnd = 0, loopCrossfade = 0;
};

// Real soft strings -> warm ensemble -> analogue pad -> airy harmonic pad.
// Every layer keeps advancing while inaudible, including sample sustain loops.
// Four modelled variations cycle the actual recorded takes that are available;
// the factory bank has one recorded soft take per root, not four recordings.
// All allocation and sample filtering is confined to setSamples()/prepare().
class PadEngine {
public:
    PadEngine() { prepare(44100.0); }

    void setSamples(std::vector<Sample> samples) {
        allSoundOff();
        bank_.clear();
        bank_.reserve(samples.size());
        for (auto& source : samples) {
            if (!validSample(source)) continue;
            BankSample destination;
            destination.sampleRate = source.sampleRate;
            destination.rootNote = source.rootNote;
            destination.roundRobin = source.roundRobin;
            destination.loopStart = source.loopStart;
            destination.loopEnd = source.loopEnd;
            destination.loopCrossfade = source.loopCrossfade;
            destination.levels[0].left = std::move(source.left);
            destination.levels[0].right = std::move(source.right);
            destination.levelCount = 1;
            while (destination.levelCount < destination.levels.size()) {
                const auto& previous = destination.levels[destination.levelCount - 1];
                if (previous.left.size() < 16) break;
                auto& next = destination.levels[destination.levelCount];
                next.left = downsample(previous.left);
                if (!previous.right.empty()) next.right = downsample(previous.right);
                ++destination.levelCount;
            }
            bank_.push_back(std::move(destination));
        }
        for (int note = 0; note < 128; ++note) buildChoices(note);
        reset();
    }

    bool hasSamples() const { return !bank_.empty(); }
    std::size_t sampleCount() const { return bank_.size(); }

    void prepare(double rate) {
        sampleRate_ = std::isfinite(rate) && rate >= 8000.0 && rate <= 768000.0 ? rate : 44100.0;
        for (std::size_t i = 0; i <= tableSize_; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(tableSize_);
            sine_[i] = static_cast<float>(std::sin(2.0 * pi_ * t));
            filterSteps_[i] = static_cast<float>(1.0 - std::exp(-2.0 * pi_ * 0.45 * t));
        }
        for (std::size_t i = 0; i < noteHz_.size(); ++i)
            noteHz_[i] = 440.0 * std::exp2((static_cast<double>(i) - 69.0) / 12.0);
        static constexpr std::array<double, 4> cents{3.7, -3.7, 1.1, -1.1};
        for (std::size_t i = 0; i < cents.size(); ++i) detune_[i] = std::exp2(cents[i] / 1200.0);
        parameterStep_ = coefficient(0.025);
        bendStep_ = coefficient(0.006);
        releaseStep_ = static_cast<float>(std::exp(-6.907755278982137 / (2.4 * sampleRate_)));
        dcPole_ = static_cast<float>(std::exp(-2.0 * pi_ * 12.0 / sampleRate_));
        stealSamples_ = std::max(4, static_cast<int>(sampleRate_ * 0.012));
        reset();
    }

    void reset() {
        allSoundOff();
        for (auto& channel : channels_) channel = Channel{};
        for (auto& sequence : noteOnSequence_) sequence.fill(0);
        for (auto& sequence : noteOffSequence_) sequence.fill(0);
        rrCounters_.fill(0);
        lastRoundRobin_ = -1;
        order_ = 0;
        motionA_ = motionB_ = 0.0;
        morphCurrent_ = morphTarget_;
        gainCurrent_ = gainTarget_;
    }

    void setMorph(float value) {
        if (std::isfinite(value)) morphTarget_ = std::clamp(value, 0.0f, 1.0f);
    }
    void setOutputGain(float value) {
        if (std::isfinite(value)) gainTarget_ = std::clamp(value, 0.0f, 4.0f);
    }

    void noteOn(int note, float velocity, int channel = 1) {
        if (!valid(note, channel) || !std::isfinite(velocity)) return;
        if (velocity <= 0.0f) { noteOff(note, channel); return; }
        if (!hasSamples()) return;
        velocity = std::min(velocity, 1.0f);
        const int variation = static_cast<int>(rrCounters_[static_cast<std::size_t>(note)]++ % 4);
        lastRoundRobin_ = variation;
        Voice* slot = nullptr;
        for (auto& voice : voices_) if (!voice.active) { slot = &voice; break; }
        if (slot == nullptr) {
            slot = &voices_[0];
            for (auto& voice : voices_) if (stealBefore(voice, *slot)) slot = &voice;
            saveTail(*slot);
        }
        *slot = Voice{};
        auto& voice = *slot;
        voice.active = voice.keyDown = true;
        voice.channel = channel - 1;
        voice.note = note;
        voice.order = ++order_;
        voice.keySequence = ++noteOnSequence_[static_cast<std::size_t>(voice.channel)][static_cast<std::size_t>(note)];
        voice.velocity = velocity;
        voice.velocityGain = std::pow(velocity, 0.8f);
        voice.frequency = noteHz_[static_cast<std::size_t>(note)];
        voice.variation = variation;
        static constexpr std::array<float, 4> gains{1.0f, 0.995f, 1.005f, 0.998f};
        static constexpr std::array<float, 4> tones{1.0f, 0.975f, 1.025f, 0.987f};
        static constexpr std::array<float, 4> attacks{1.0f, 1.035f, 0.972f, 1.016f};
        const auto rr = static_cast<std::size_t>(variation);
        voice.rrGain = gains[rr];
        voice.naturalFilter = filterStep((4800.0f + 1600.0f * velocity) * tones[rr]);
        const std::array<double, 4> attackSeconds{
            0.18 + 0.06 * (1.0 - static_cast<double>(velocity)), 0.38, 0.29, 0.54};
        for (std::size_t i = 0; i < attackSeconds.size(); ++i)
            voice.attackStep[i] = static_cast<float>(1.0 / (sampleRate_ * attackSeconds[i] * attacks[rr]));
        const double initialPhase = wrap(0.071 * static_cast<double>(note) + 0.19 * variation);
        for (std::size_t i = 0; i < voice.analoguePhase.size(); ++i)
            voice.analoguePhase[i] = wrap(initialPhase + 0.137 * static_cast<double>(i));
        voice.bodyPhase = wrap(initialPhase + 0.31);
        voice.digitalPhaseA = wrap(initialPhase + 0.23);
        voice.digitalPhaseB = wrap(initialPhase + 0.61);
        voice.noise = 0x9e3779b9u ^ (static_cast<std::uint32_t>(note + 1) * 0x85ebca6bu)
            ^ (static_cast<std::uint32_t>(variation + 1) * 0xc2b2ae35u)
            ^ static_cast<std::uint32_t>(channel * 97);
        const auto& choices = choices_[static_cast<std::size_t>(note)];
        if (choices.count != 0) {
            voice.playhead.sample = choices.indices[rr % choices.count];
            const auto& sample = bank_[static_cast<std::size_t>(voice.playhead.sample)];
            voice.playhead.increment = sample.sampleRate / sampleRate_ * voice.frequency
                / noteHz_[static_cast<std::size_t>(sample.rootNote)];
        }
    }

    void noteOff(int note, int channel = 1) {
        if (!valid(note, channel)) return;
        const auto ch = static_cast<std::size_t>(channel - 1);
        const auto key = static_cast<std::size_t>(note);
        auto& off = noteOffSequence_[ch][key];
        if (off >= noteOnSequence_[ch][key]) return;
        const auto sequence = ++off;
        for (auto& voice : voices_)
            if (voice.active && voice.channel == channel - 1 && voice.note == note
                && voice.keySequence == sequence) {
                voice.keyDown = false;
                if (channels_[ch].sustain) voice.pedalHeld = true;
                else beginRelease(voice);
                break;
            }
    }

    void sustainPedal(bool down, int channel = 1) {
        if (channel < 1 || channel > 16) return;
        channels_[static_cast<std::size_t>(channel - 1)].sustain = down;
        if (!down) for (auto& voice : voices_)
            if (voice.active && voice.channel == channel - 1 && !voice.keyDown && voice.pedalHeld)
                beginRelease(voice);
    }

    void pitchBend(float semitones, int channel = 1) {
        if (channel < 1 || channel > 16 || !std::isfinite(semitones)) return;
        channels_[static_cast<std::size_t>(channel - 1)].bendTarget = static_cast<float>(
            std::exp2(static_cast<double>(std::clamp(semitones, -48.0f, 48.0f)) / 12.0));
    }

    void allNotesOff(int channel = 0) {
        if (channel < 0 || channel > 16) return;
        for (auto& voice : voices_) if (voice.active && matches(voice.channel, channel)) {
            voice.keyDown = false;
            if (channels_[static_cast<std::size_t>(voice.channel)].sustain) voice.pedalHeld = true;
            else beginRelease(voice);
        }
        consumePendingNotes(channel);
    }

    void allSoundOff(int channel = 0) {
        if (channel < 0 || channel > 16) return;
        for (auto& voice : voices_) if (matches(voice.channel, channel)) voice = Voice{};
        for (auto& voice : tails_) if (matches(voice.channel, channel)) voice = Voice{};
        consumePendingNotes(channel);
        peak_ = 0.0f;
    }

    void resetControllers(int channel = 1) {
        if (channel < 0 || channel > 16) return;
        for (int ch = 1; ch <= 16; ++ch) if (channel == 0 || channel == ch) {
            sustainPedal(false, ch);
            channels_[static_cast<std::size_t>(ch - 1)].bendTarget = 1.0f;
        }
    }

    void render(float* left, float* right, int count) {
        peak_ = 0.0f;
        for (int i = 0; i < count; ++i) {
            smooth(morphCurrent_, morphTarget_, parameterStep_);
            smooth(gainCurrent_, gainTarget_, parameterStep_);
            for (auto& channel : channels_) smooth(channel.bendCurrent, channel.bendTarget, bendStep_);
            const auto weights = morphWeights();
            const float motionA = sine(motionA_), motionB = sine(motionB_);
            Bus bus;
            for (auto& voice : voices_) if (voice.active) renderVoice(voice, bus, motionA, motionB);
            for (auto& voice : tails_) if (voice.active) renderVoice(voice, bus, motionA, motionB);
            float l = 0.0f, r = 0.0f;
            for (std::size_t anchor = 0; anchor < 4; ++anchor) {
                l += bus.left[anchor] * weights[anchor];
                r += bus.right[anchor] * weights[anchor];
            }
            l = safetyKnee(l * gainCurrent_);
            r = safetyKnee(r * gainCurrent_);
            if (left != nullptr) left[i] = l;
            if (right != nullptr) right[i] = r;
            peak_ = std::max(peak_, std::max(std::abs(l), std::abs(r)));
            motionA_ = wrap(motionA_ + 0.083 / sampleRate_);
            motionB_ = wrap(motionB_ + 0.117 / sampleRate_);
        }
    }

    float peak() const { return peak_; }
    int lastRoundRobin() const { return lastRoundRobin_; }
    int getActiveVoiceCount() const {
        int result = 0;
        for (const auto& voice : voices_) result += voice.active ? 1 : 0;
        for (const auto& voice : tails_) result += voice.active ? 1 : 0;
        return result;
    }
    int activeVoiceCount() const { return getActiveVoiceCount(); }

private:
    static constexpr double pi_ = 3.1415926535897932384626433832795;
    static constexpr std::size_t tableSize_ = 2048;
    static constexpr float naturalGain_ = 0.36f, analogueGain_ = 0.195f, digitalGain_ = 0.113f;
    struct Level { std::vector<float> left, right; };
    struct BankSample {
        std::array<Level, 16> levels{};
        std::size_t levelCount = 0, loopStart = 0, loopEnd = 0, loopCrossfade = 0;
        double sampleRate = 44100.0;
        int rootNote = 60, roundRobin = 0;
    };
    struct Choices { std::array<int, 16> indices{}; std::size_t count = 0; };
    struct Channel { bool sustain = false; float bendCurrent = 1.0f, bendTarget = 1.0f; };
    struct Playhead { int sample = -1; double position = 0.0, increment = 1.0; };
    struct Voice {
        bool active = false, keyDown = false, pedalHeld = false, releasing = false;
        int channel = 0, note = 60, variation = 0, tailRemaining = 0;
        std::uint64_t order = 0, keySequence = 0;
        std::uint32_t noise = 1;
        Playhead playhead{};
        double frequency = 440.0, bodyPhase = 0.0, digitalPhaseA = 0.0, digitalPhaseB = 0.0;
        std::array<double, 4> analoguePhase{};
        std::array<float, 4> attack{}, attackStep{};
        float velocity = 0.0f, velocityGain = 0.0f, rrGain = 1.0f, gate = 1.0f, energy = 0.0f;
        float naturalFilter = 1.0f, naturalL = 0.0f, naturalR = 0.0f;
        float dcInputL = 0.0f, dcInputR = 0.0f, dcL = 0.0f, dcR = 0.0f;
        float filterL1 = 0.0f, filterL2 = 0.0f, filterR1 = 0.0f, filterR2 = 0.0f;
        float airLow = 0.0f, airHigh = 0.0f;
    };
    struct Bus { std::array<float, 4> left{}, right{}; };

    static bool valid(int note, int channel) { return note >= 0 && note < 128 && channel >= 1 && channel <= 16; }
    static bool matches(int internalChannel, int channel) { return channel == 0 || internalChannel == channel - 1; }
    static double wrap(double x) { return x - std::floor(x); }
    static float smoothstep(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
    static void smooth(float& value, float target, float step) {
        const float difference = target - value;
        value = std::abs(difference) < 1.0e-8f ? target : value + step * difference;
    }
    float coefficient(double seconds) const { return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate_))); }
    float sine(double phase) const {
        const double position = wrap(phase) * static_cast<double>(tableSize_);
        const auto index = static_cast<std::size_t>(position);
        const float t = static_cast<float>(position - static_cast<double>(index));
        return sine_[index] + (sine_[index + 1] - sine_[index]) * t;
    }
    float filterStep(float hz) const {
        const float position = std::clamp(hz / static_cast<float>(sampleRate_ * 0.45), 0.0f, 1.0f)
            * static_cast<float>(tableSize_);
        const auto index = static_cast<std::size_t>(position);
        const auto next = std::min(index + 1, tableSize_);
        return filterSteps_[index] + (filterSteps_[next] - filterSteps_[index])
            * (position - static_cast<float>(index));
    }
    float partialGain(double hz) const {
        return smoothstep(static_cast<float>((sampleRate_ * 0.45 - hz) / (sampleRate_ * 0.05)));
    }
    std::array<float, 4> morphWeights() const {
        std::array<float, 4> result{};
        const float position = std::clamp(morphCurrent_, 0.0f, 1.0f) * 3.0f;
        const int segment = std::min(2, static_cast<int>(position));
        const float t = smoothstep(position - static_cast<float>(segment));
        if (t <= 0.0f) result[static_cast<std::size_t>(segment)] = 1.0f;
        else if (t >= 1.0f) result[static_cast<std::size_t>(segment + 1)] = 1.0f;
        else {
            float a = sine(0.25 + 0.25 * static_cast<double>(t));
            float b = sine(0.25 * static_cast<double>(t));
            // The first two anchors share the real strings. Compensate their
            // correlation so their overlap does not produce a 3 dB volume bump.
            static constexpr std::array<float, 3> correlation{0.78f, 0.12f, 0.0f};
            const float normalisation = 1.0f / std::sqrt(1.0f
                + 2.0f * correlation[static_cast<std::size_t>(segment)] * a * b);
            a *= normalisation; b *= normalisation;
            result[static_cast<std::size_t>(segment)] = a;
            result[static_cast<std::size_t>(segment + 1)] = b;
        }
        return result;
    }
    static float safetyKnee(float x) {
        if (!std::isfinite(x)) return 0.0f;
        const float absolute = std::abs(x);
        if (absolute <= 0.90f) return x;
        const float excess = (absolute - 0.90f) * 10.0f;
        return std::copysign(0.90f + 0.10f * (excess / (1.0f + excess)), x);
    }
    static float blep(double phase, double step) {
        if (step <= 0.0) return 0.0f;
        if (phase < step) { const double t = phase / step; return static_cast<float>(t + t - t * t - 1.0); }
        if (phase > 1.0 - step) { const double t = (phase - 1.0) / step; return static_cast<float>(t * t + t + t + 1.0); }
        return 0.0f;
    }
    static float saw(double phase, double step) { return static_cast<float>(2.0 * phase - 1.0) - blep(phase, step); }
    static float noise(Voice& voice) {
        voice.noise ^= voice.noise << 13; voice.noise ^= voice.noise >> 17; voice.noise ^= voice.noise << 5;
        return static_cast<float>(voice.noise >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
    static bool validSample(const Sample& sample) {
        if (sample.left.size() < 4 || (!sample.right.empty() && sample.right.size() != sample.left.size())
            || !std::isfinite(sample.sampleRate) || sample.sampleRate < 1000.0 || sample.sampleRate > 768000.0
            || sample.rootNote < 0 || sample.rootNote > 127 || sample.roundRobin < 0 || sample.roundRobin > 127
            || sample.loopStart >= sample.loopEnd || sample.loopEnd > sample.left.size()
            || sample.loopEnd - sample.loopStart < 4 || sample.loopCrossfade < 2
            || sample.loopCrossfade > (sample.loopEnd - sample.loopStart) / 2) return false;
        bool nonzero = false;
        for (float value : sample.left) {
            if (!std::isfinite(value)) return false;
            nonzero = nonzero || std::abs(value) > 1.0e-9f;
        }
        for (float value : sample.right) {
            if (!std::isfinite(value)) return false;
            nonzero = nonzero || std::abs(value) > 1.0e-9f;
        }
        return nonzero;
    }
    static std::vector<float> downsample(const std::vector<float>& source) {
        // A windowed-sinc half-rate low-pass makes octave mip levels offline.
        // Playback blends levels continuously, including through pitch bends.
        constexpr int taps = 49;
        std::array<double, taps> kernel{};
        double sum = 0.0;
        for (int i = 0; i < taps; ++i) {
            const double t = static_cast<double>(i) - static_cast<double>(taps - 1) * 0.5;
            const double window = 0.42 - 0.5 * std::cos(2.0 * pi_ * i / (taps - 1))
                + 0.08 * std::cos(4.0 * pi_ * i / (taps - 1));
            const double sinc = std::abs(t) < 1.0e-12 ? 0.45
                : std::sin(2.0 * pi_ * 0.225 * t) / (pi_ * t);
            kernel[static_cast<std::size_t>(i)] = sinc * window;
            sum += kernel[static_cast<std::size_t>(i)];
        }
        for (auto& value : kernel) value /= sum;
        std::vector<float> output((source.size() + 1) / 2);
        for (std::size_t i = 0; i < output.size(); ++i) {
            double value = 0.0;
            for (int tap = 0; tap < taps; ++tap) {
                const auto index = std::clamp(static_cast<std::ptrdiff_t>(i * 2)
                    + tap - taps / 2, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(source.size() - 1));
                value += static_cast<double>(source[static_cast<std::size_t>(index)]) * kernel[static_cast<std::size_t>(tap)];
            }
            output[i] = static_cast<float>(value);
        }
        return output;
    }
    static float readSample(const std::vector<float>& source, double position) {
        position = std::clamp(position, 0.0, static_cast<double>(source.size() - 1));
        const auto i = static_cast<std::size_t>(position);
        const float t = static_cast<float>(position - static_cast<double>(i));
        const float a = source[i == 0 ? 0 : i - 1];
        const float b = source[i], c = source[std::min(i + 1, source.size() - 1)];
        const float d = source[std::min(i + 2, source.size() - 1)];
        const float c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * t + c2) * t + c1) * t + b;
    }
    static void readLevel(const Level& level, double position, float& left, float& right) {
        left = readSample(level.left, position);
        right = level.right.empty() ? left : readSample(level.right, position);
    }
    void renderStrings(Voice& voice, double bend, float& left, float& right) {
        left = right = 0.0f;
        auto& playhead = voice.playhead;
        if (playhead.sample < 0) return;
        const auto& sample = bank_[static_cast<std::size_t>(playhead.sample)];
        const double increment = playhead.increment * bend;
        double adjustedIncrement = increment, divisor = 1.0;
        std::size_t levelIndex = 0;
        // Each selected level is already band-limited for this playback rate.
        // Start the blend an octave early to avoid switching filters at a bend.
        while (adjustedIncrement >= 1.0 && levelIndex + 1 < sample.levelCount) {
            ++levelIndex; divisor *= 2.0; adjustedIncrement *= 0.5;
        }
        const float levelBlend = static_cast<float>(std::clamp(adjustedIncrement * 2.0 - 1.0, 0.0, 1.0));
        auto read = [&](double position, float& l, float& r) {
            readLevel(sample.levels[levelIndex], position / divisor, l, r);
            if (levelBlend > 0.0f && levelIndex + 1 < sample.levelCount) {
                float nextL = 0.0f, nextR = 0.0f;
                readLevel(sample.levels[levelIndex + 1], position / (divisor * 2.0), nextL, nextR);
                l += (nextL - l) * levelBlend; r += (nextR - r) * levelBlend;
            }
        };
        read(playhead.position, left, right);
        const double crossfadeStart = static_cast<double>(sample.loopEnd - sample.loopCrossfade);
        if (playhead.position >= crossfadeStart) {
            const double offset = playhead.position - crossfadeStart;
            const double t = std::clamp(offset / static_cast<double>(sample.loopCrossfade), 0.0, 1.0);
            float headL = 0.0f, headR = 0.0f;
            read(static_cast<double>(sample.loopStart) + offset, headL, headR);
            const float tailWeight = sine(0.25 + 0.25 * t), headWeight = sine(0.25 * t);
            left = tailWeight * left + headWeight * headL;
            right = tailWeight * right + headWeight * headR;
        }
        playhead.position += increment;
        const double loopEnd = static_cast<double>(sample.loopEnd);
        if (playhead.position >= loopEnd) {
            const double restart = static_cast<double>(sample.loopStart + sample.loopCrossfade);
            const double length = loopEnd - restart;
            playhead.position = restart + std::fmod(playhead.position - loopEnd, length);
        }
        voice.dcL = left - voice.dcInputL + dcPole_ * voice.dcL;
        voice.dcR = right - voice.dcInputR + dcPole_ * voice.dcR;
        voice.dcInputL = left; voice.dcInputR = right;
        smooth(voice.naturalL, voice.dcL, voice.naturalFilter);
        smooth(voice.naturalR, voice.dcR, voice.naturalFilter);
        left = voice.naturalL; right = voice.naturalR;
    }
    void consumePendingNotes(int channel) {
        for (int ch = 0; ch < 16; ++ch) if (matches(ch, channel))
            noteOffSequence_[static_cast<std::size_t>(ch)] = noteOnSequence_[static_cast<std::size_t>(ch)];
    }
    static void beginRelease(Voice& voice) {
        voice.keyDown = false; voice.pedalHeld = false; voice.releasing = true;
    }
    static bool stealBefore(const Voice& a, const Voice& b) {
        const int aClass = a.releasing ? 0 : (a.keyDown ? 2 : 1);
        const int bClass = b.releasing ? 0 : (b.keyDown ? 2 : 1);
        if (aClass != bClass) return aClass < bClass;
        if (std::abs(a.energy - b.energy) > 1.0e-6f) return a.energy < b.energy;
        return a.order < b.order;
    }
    void saveTail(const Voice& source) {
        if (!source.active || source.attack[0] <= 0.0f) return;
        auto* tail = &tails_[0];
        for (auto& candidate : tails_) {
            if (!candidate.active) { tail = &candidate; break; }
            if (candidate.energy * static_cast<float>(candidate.tailRemaining)
                < tail->energy * static_cast<float>(tail->tailRemaining)) tail = &candidate;
        }
        *tail = source;
        beginRelease(*tail);
        tail->tailRemaining = stealSamples_;
    }
    void buildChoices(int note) {
        auto& choices = choices_[static_cast<std::size_t>(note)];
        choices.count = 0;
        int bestRoot = -1, bestScore = std::numeric_limits<int>::max();
        for (const auto& sample : bank_) {
            const int score = std::abs(sample.rootNote - note);
            if (score < bestScore || (score == bestScore && sample.rootNote < bestRoot)) {
                bestRoot = sample.rootNote; bestScore = score;
            }
        }
        for (std::size_t i = 0; i < bank_.size() && choices.count < choices.indices.size(); ++i)
            if (bank_[i].rootNote == bestRoot) choices.indices[choices.count++] = static_cast<int>(i);
        std::sort(choices.indices.begin(), choices.indices.begin() + static_cast<std::ptrdiff_t>(choices.count),
            [this](int a, int b) { return bank_[static_cast<std::size_t>(a)].roundRobin < bank_[static_cast<std::size_t>(b)].roundRobin; });
    }

    void renderVoice(Voice& voice, Bus& bus, float motionA, float motionB) {
        float tailGain = 1.0f;
        if (voice.tailRemaining > 0) {
            tailGain = static_cast<float>(voice.tailRemaining) / static_cast<float>(stealSamples_);
            if (--voice.tailRemaining == 0) { voice.active = false; return; }
        }
        if (voice.releasing) {
            voice.gate *= releaseStep_;
            if (voice.gate < 1.0e-6f) { voice.active = false; return; }
        } else for (std::size_t i = 0; i < voice.attack.size(); ++i)
            voice.attack[i] = std::min(1.0f, voice.attack[i] + voice.attackStep[i]);
        const double bend = channels_[static_cast<std::size_t>(voice.channel)].bendCurrent;
        const double hz = voice.frequency * bend;
        const double step = std::min(0.49, hz / sampleRate_);
        const float fundamental = partialGain(hz);
        float stringL = 0.0f, stringR = 0.0f;
        renderStrings(voice, bend, stringL, stringR);
        stringL *= fundamental; stringR *= fundamental;

        std::array<float, 4> oscillators{};
        for (std::size_t i = 0; i < oscillators.size(); ++i) {
            const double delta = std::min(0.49, step * detune_[i]);
            oscillators[i] = saw(voice.analoguePhase[i], delta) * partialGain(hz * detune_[i]);
            voice.analoguePhase[i] = wrap(voice.analoguePhase[i] + delta);
        }
        const float body = sine(voice.bodyPhase) * fundamental;
        const float analogueL = 0.39f * oscillators[0] + 0.19f * oscillators[1]
            + 0.12f * oscillators[2] + 0.10f * oscillators[3] + 0.14f * body;
        const float analogueR = 0.19f * oscillators[0] + 0.39f * oscillators[1]
            + 0.10f * oscillators[2] + 0.12f * oscillators[3] + 0.14f * body;
        const float cutoff = 650.0f + static_cast<float>(hz) * 3.8f + 950.0f * voice.velocity
            + 200.0f * motionA;
        const float filter = filterStep(cutoff);
        smooth(voice.filterL1, analogueL, filter); smooth(voice.filterL2, voice.filterL1, filter);
        smooth(voice.filterR1, analogueR, filter); smooth(voice.filterR2, voice.filterR1, filter);

        // Harmonic additive spectrum: no sharp waveshaper and no inharmonic
        // bell attacks. Partials smoothly disappear below the Nyquist boundary.
        static constexpr std::array<int, 10> ratios{1, 2, 3, 4, 5, 6, 8, 10, 12, 16};
        static constexpr std::array<float, 10> levels{
            0.68f, 0.27f, 0.22f, 0.18f, 0.15f, 0.13f, 0.12f, 0.105f, 0.09f, 0.07f};
        float digitalA = 0.0f, digitalB = 0.0f;
        for (std::size_t i = 0; i < ratios.size(); ++i) {
            const double ratio = static_cast<double>(ratios[i]);
            const float evolution = i == 0 ? 1.0f : 1.0f + (i % 2 == 0 ? 0.20f * motionA : 0.20f * motionB);
            const float level = levels[i] * evolution;
            digitalA += level * sine(voice.digitalPhaseA * ratio) * partialGain(hz * ratio);
            digitalB += level * sine(voice.digitalPhaseB * ratio) * partialGain(hz * ratio * 1.0007);
        }
        const float n = noise(voice);
        smooth(voice.airLow, n, filterStep(6200.0f));
        smooth(voice.airHigh, voice.airLow, filterStep(1800.0f));
        const float air = 0.013f * (voice.airLow - voice.airHigh) * fundamental;
        const float digitalL = 0.80f * digitalA + 0.20f * digitalB + air;
        const float digitalR = 0.20f * digitalA + 0.80f * digitalB + air;
        voice.bodyPhase = wrap(voice.bodyPhase + step);
        voice.digitalPhaseA = wrap(voice.digitalPhaseA + step);
        voice.digitalPhaseB = wrap(voice.digitalPhaseB + std::min(0.49, step * 1.0007));

        const float gain = voice.gate * voice.velocityGain * voice.rrGain * tailGain;
        const std::array<float, 4> layerL{
            stringL * naturalGain_,
            0.27f * stringL + 0.060f * voice.filterL2 + 0.032f * body,
            voice.filterL2 * analogueGain_, digitalL * digitalGain_};
        const std::array<float, 4> layerR{
            stringR * naturalGain_,
            0.27f * stringR + 0.060f * voice.filterR2 + 0.032f * body,
            voice.filterR2 * analogueGain_, digitalR * digitalGain_};
        float energy = 0.0f;
        for (std::size_t i = 0; i < layerL.size(); ++i) {
            const float envelope = smoothstep(voice.attack[i]) * gain;
            const float l = layerL[i] * envelope, r = layerR[i] * envelope;
            bus.left[i] += l; bus.right[i] += r;
            energy = std::max(energy, std::max(std::abs(l), std::abs(r)));
        }
        voice.energy += 0.002f * (energy - voice.energy);
    }

    std::vector<BankSample> bank_;
    std::array<Choices, 128> choices_{};
    std::array<Voice, 32> voices_{};
    std::array<Voice, 8> tails_{};
    std::array<Channel, 16> channels_{};
    std::array<double, 128> noteHz_{};
    std::array<double, 4> detune_{};
    std::array<std::array<std::uint64_t, 128>, 16> noteOnSequence_{}, noteOffSequence_{};
    std::array<std::uint32_t, 128> rrCounters_{};
    std::array<float, tableSize_ + 1> sine_{}, filterSteps_{};
    double sampleRate_ = 44100.0, motionA_ = 0.0, motionB_ = 0.0;
    std::uint64_t order_ = 0;
    float morphTarget_ = 0.0f, morphCurrent_ = 0.0f, gainTarget_ = 1.0f, gainCurrent_ = 1.0f;
    float parameterStep_ = 0.01f, bendStep_ = 0.01f, releaseStep_ = 0.9999f, dcPole_ = 0.999f;
    float peak_ = 0.0f;
    int lastRoundRobin_ = -1, stealSamples_ = 512;
};

} // namespace sculptpad
