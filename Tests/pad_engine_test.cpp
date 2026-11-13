#include "../Source/PadEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
std::atomic<bool> trackAllocations { false };
std::atomic<std::size_t> allocationCount { 0 };

void countAllocation() noexcept {
    if (trackAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
}

// Keep the malloc-backed hooks out of GCC's inliner to avoid a false
// mismatched-new-delete warning in this test harness.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void freeTestAllocation(void* memory) noexcept { std::free(memory); }
}

void* operator new(std::size_t bytes) {
    countAllocation();
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory) noexcept { freeTestAllocation(memory); }
void operator delete(void* memory, std::size_t) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, std::size_t) noexcept { freeTestAllocation(memory); }

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    countAllocation();
    void* memory = nullptr;
    const auto boundary = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    if (posix_memalign(&memory, boundary, bytes == 0 ? 1 : bytes) == 0) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return ::operator new(bytes, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { freeTestAllocation(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { freeTestAllocation(memory); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes); } catch (...) { return nullptr; }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { freeTestAllocation(memory); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    freeTestAllocation(memory);
}
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    freeTestAllocation(memory);
}

namespace {
using sculptpad::PadEngine;
using sculptpad::Sample;
constexpr double rate = 48000.0;
constexpr double pi = 3.14159265358979323846;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double frequency(int note) { return 440.0 * std::exp2((note - 69.0) / 12.0); }

// Artificial periodic fixtures exercise the sampler. They are never shipped
// as a replacement for the real string recordings in Samples/.
Sample fixture(int note = 60, int rr = 0) {
    Sample s;
    s.sampleRate = rate; s.rootNote = note; s.roundRobin = rr;
    s.loopStart = 4800; s.loopEnd = 24000; s.loopCrossfade = 4800;
    s.left.resize(s.loopEnd); s.right.resize(s.loopEnd);
    for (std::size_t i = 0; i < s.left.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double p = 2.0 * pi * frequency(note) * t + 0.11 * rr;
        const double env = std::min(1.0, t / 0.008);
        s.left[i] = static_cast<float>(env * (0.18 * std::sin(p) + 0.035 * std::sin(2.0 * p)));
        s.right[i] = static_cast<float>(env * (0.17 * std::sin(p + 0.09) + 0.031 * std::sin(2.0 * p + 0.15)));
    }
    return s;
}

void setup(PadEngine& engine, float morph = 0.0f, double sampleRate = rate) {
    std::vector<Sample> bank;
    for (int note : {48, 60, 72}) bank.push_back(fixture(note));
    engine.setSamples(std::move(bank));
    engine.setMorph(morph); engine.setOutputGain(1.0f); engine.prepare(sampleRate);
    require(engine.hasSamples(), "Valid looped bank rejected");
}

struct Audio {
    std::vector<float> l, r;
    explicit Audio(int frames) : l(static_cast<std::size_t>(frames), 7.0f),
                                 r(static_cast<std::size_t>(frames), -7.0f) {}
};

void finite(const Audio& audio) {
    for (std::size_t i = 0; i < audio.l.size(); ++i)
        require(std::isfinite(audio.l[i]) && std::isfinite(audio.r[i]), "Non-finite audio");
}

Audio render(PadEngine& engine, int frames) {
    Audio audio(frames);
    engine.render(audio.l.data(), audio.r.data(), frames);
    finite(audio);
    require(std::isfinite(engine.peak()), "Non-finite meter");
    return audio;
}

void advance(PadEngine& engine, double seconds, double sampleRate = rate) {
    std::array<float, 256> l {}, r {};
    int remaining = static_cast<int>(seconds * sampleRate);
    while (remaining > 0) {
        const int n = std::min(remaining, 256);
        engine.render(l.data(), r.data(), n);
        for (int i = 0; i < n; ++i)
            require(std::isfinite(l[static_cast<std::size_t>(i)])
                    && std::isfinite(r[static_cast<std::size_t>(i)]), "Non-finite sustained audio");
        remaining -= n;
    }
}

double rms(const Audio& a) {
    double e = 0.0;
    for (std::size_t i = 0; i < a.l.size(); ++i)
        e += static_cast<double>(a.l[i]) * a.l[i] + static_cast<double>(a.r[i]) * a.r[i];
    return std::sqrt(e / (2.0 * static_cast<double>(a.l.size())));
}

double peak(const Audio& a) {
    double p = 0.0;
    for (std::size_t i = 0; i < a.l.size(); ++i)
        p = std::max({p, std::abs(static_cast<double>(a.l[i])), std::abs(static_cast<double>(a.r[i]))});
    return p;
}

double difference(const Audio& a, const Audio& b) {
    require(a.l.size() == b.l.size(), "Different comparison lengths");
    double p = 0.0;
    for (std::size_t i = 0; i < a.l.size(); ++i)
        p = std::max({p, std::abs(static_cast<double>(a.l[i]) - b.l[i]),
                        std::abs(static_cast<double>(a.r[i]) - b.r[i])});
    return p;
}

void released(PadEngine& engine, int count) {
    advance(engine, 5.5);
    require(engine.getActiveVoiceCount() == count, "Unexpected voice count after release");
}

void testSilenceAndInvalidInput() {
    PadEngine engine;
    engine.noteOn(60, 0.8f);
    require(peak(render(engine, 128)) == 0.0, "Empty engine is not silent");
    setup(engine);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    engine.noteOn(-1, 0.8f); engine.noteOn(128, 0.8f);
    engine.noteOn(60, 0.8f, 0); engine.noteOn(60, 0.8f, 17);
    engine.noteOn(60, nan); engine.noteOn(60, std::numeric_limits<float>::infinity());
    engine.noteOff(-1); engine.noteOff(128); engine.setMorph(nan);
    engine.setOutputGain(nan); engine.pitchBend(nan);
    require(engine.getActiveVoiceCount() == 0, "Invalid MIDI created voices");
    require(peak(render(engine, 128)) == 0.0, "Invalid MIDI created audio");
    engine.noteOn(60, 0.8f); advance(engine, 0.6);
    require(rms(render(engine, 1024)) > 1.0e-4, "Valid note failed after invalid input");
    for (double sampleRate : {0.0, -48000.0, std::numeric_limits<double>::quiet_NaN()}) {
        engine.prepare(sampleRate); engine.noteOn(60, 0.8f); render(engine, 128);
        engine.allSoundOff();
    }
}

void testInvalidBank() {
    for (int kind = 0; kind < 6; ++kind) {
        PadEngine engine;
        auto s = fixture();
        if (kind == 0) s.loopEnd = s.left.size() + 1;
        if (kind == 1) s.loopStart = s.loopEnd;
        if (kind == 2) s.loopCrossfade = s.loopEnd;
        if (kind == 3) s.left[100] = std::numeric_limits<float>::quiet_NaN();
        if (kind == 4) s.right.resize(4);
        if (kind == 5) s.sampleRate = 0.0;
        engine.setSamples({std::move(s)});
        require(!engine.hasSamples(), "Invalid sample/loop bank accepted");
    }
}

void testHeldLoopsAndMorph() {
    PadEngine engine; setup(engine);
    for (int note : {48, 55, 60, 64, 67}) engine.noteOn(note, 0.72f);
    advance(engine, 3.0); // More than six source sample lengths.
    require(rms(render(engine, 4096)) > 0.001, "Natural sustain stopped at sample end");
    for (float morph : {1.0f / 3.0f, 2.0f / 3.0f, 1.0f, 0.0f}) {
        engine.setMorph(morph);
        const auto audio = render(engine, 48000);
        require(rms(audio) > 0.001, "Held chord disappeared during morph");
        require(peak(audio) < 1.0, "Held chord clipped during morph");
        double largestStep = 0.0;
        for (std::size_t i = 1; i < audio.l.size(); ++i)
            largestStep = std::max(largestStep, std::abs(static_cast<double>(audio.l[i]) - audio.l[i - 1]));
        require(largestStep < 0.15, "Discontinuous held-note morph/loop");
    }
    require(engine.getActiveVoiceCount() == 5, "Held morph changed polyphony");
    engine.allNotesOff(); released(engine, 0);
    require(peak(render(engine, 256)) == 0.0, "Release leaves nonzero output");
}

void testVelocityAndStereo() {
    for (float morph : {0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f}) {
        PadEngine engine; setup(engine, morph);
        engine.noteOn(60, 0.35f); advance(engine, 0.8);
        const auto soft = render(engine, 8192);
        engine.reset(); engine.noteOn(60, 0.85f); advance(engine, 0.8);
        const auto loud = render(engine, 8192);
        require(rms(loud) > rms(soft) * 1.3, "Velocity does not control musical dynamics");
        require(peak(loud) < 1.0, "Single voice clips");
        bool stereo = false;
        for (std::size_t i = 0; i < loud.l.size(); ++i) stereo = stereo || loud.l[i] != loud.r[i];
        require(stereo, "Pad lost stereo width");
    }
}

void testRoundRobinAndReset() {
    PadEngine engine; setup(engine);
    std::vector<Audio> takes;
    for (int i = 0; i < 4; ++i) {
        engine.allSoundOff(); engine.noteOn(60, 0.7f);
        require(engine.lastRoundRobin() == i, "Round robin order is not 0,1,2,3");
        takes.push_back(render(engine, 8192));
    }
    require(difference(takes[0], takes[1]) > 1.0e-5, "Round robin variations are identical");
    engine.allSoundOff(); engine.noteOn(60, 0.7f);
    require(engine.lastRoundRobin() == 0, "Round robin does not wrap after four");
    engine.reset(); engine.noteOn(60, 0.7f);
    require(difference(takes[0], render(engine, 8192)) == 0.0, "Reset is not deterministic");
}

void testSustainChannelsAndPanic() {
    PadEngine engine; setup(engine, 1.0f);
    engine.noteOn(60, 0.7f, 1); engine.noteOn(67, 0.7f, 2);
    engine.sustainPedal(true, 1); engine.noteOff(60, 1); engine.noteOff(67, 2);
    released(engine, 1);
    engine.sustainPedal(false, 1); released(engine, 0);
    engine.noteOn(60, 0.7f, 1); engine.noteOn(67, 0.7f, 2);
    advance(engine, 0.3); engine.allSoundOff(1);
    require(engine.getActiveVoiceCount() == 1, "Channel panic affected another channel");
    require(rms(render(engine, 1024)) > 1.0e-4, "Channel panic silenced another channel");
    engine.allSoundOff();
    require(peak(render(engine, 256)) == 0.0, "Global panic leaves a tail");
    engine.noteOn(60, 0.7f); engine.sustainPedal(true); engine.allNotesOff();
    released(engine, 1); engine.resetControllers(); released(engine, 0);
}

void testRepeatedNotesAndStolenFifo() {
    PadEngine engine; setup(engine);
    engine.noteOn(60, 0.7f); engine.noteOn(60, 0.8f); engine.noteOff(60);
    released(engine, 1); engine.noteOff(60); released(engine, 0);
    engine.reset(); engine.noteOn(60, 0.01f);
    for (int i = 0; i < 31; ++i) engine.noteOn(25 + i, 0.8f);
    advance(engine, 0.7);
    engine.noteOn(60, 0.8f); // The very quiet oldest instance is stolen.
    require(engine.getActiveVoiceCount() <= 40, "Voice stealing exceeded its bounded tail budget");
    advance(engine, 0.02); // Allow the 12 ms stealing crossfade to finish.
    require(engine.getActiveVoiceCount() == 32, "Main polyphony is not bounded to 32");
    engine.noteOff(60); advance(engine, 5.5);
    require(engine.getActiveVoiceCount() == 32, "Old stolen note-off released the replacement");
    engine.noteOff(60); released(engine, 31);
    engine.allNotesOff(); released(engine, 0);
    engine.noteOn(60, 0.8f); engine.noteOff(60); released(engine, 0);
}

void testBendChannelIsolationAndMute() {
    PadEngine a, b; setup(a, 2.0f / 3.0f); setup(b, 2.0f / 3.0f);
    a.pitchBend(12.0f, 1);
    a.noteOn(60, 0.7f, 2); b.noteOn(60, 0.7f, 2);
    require(difference(render(a, 16384), render(b, 16384)) == 0.0,
            "Pitch bend leaked to a different MIDI channel");
    a.pitchBend(12.0f, 2);
    require(difference(render(a, 16384), render(b, 16384)) > 0.001,
            "Pitch bend has no effect on its own channel");
    a.setOutputGain(0.0f); advance(a, 0.6);
    require(peak(render(a, 1024)) < 1.0e-7, "Output gain cannot mute the pad");
}

void testBlockIndependence() {
    PadEngine a, b; setup(a, 0.43f); setup(b, 0.43f);
    for (int note : {48, 55, 60, 64}) { a.noteOn(note, 0.7f); b.noteOn(note, 0.7f); }
    const int n = 32768;
    const auto continuous = render(a, n);
    Audio partitioned(n);
    for (int position = 0; position < n;) {
        const int count = std::min(1 + position % 509, n - position);
        b.render(partitioned.l.data() + position, partitioned.r.data() + position, count);
        position += count;
    }
    require(difference(continuous, partitioned) == 0.0, "Sound depends on host block size");
}

void testRatesAndExtremes() {
    for (double sampleRate : {8000.0, 44100.0, 96000.0, 192000.0}) {
        PadEngine engine; setup(engine, 0.5f, sampleRate);
        for (int note : {0, 12, 36, 60, 84, 108, 127}) engine.noteOn(note, 1.0f);
        engine.pitchBend(48.0f);
        for (float morph : {0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f}) {
            engine.setMorph(morph);
            const auto a = render(engine, static_cast<int>(sampleRate * 0.08));
            require(peak(a) < 1.0, "Extreme MIDI/rate output clips");
        }
        engine.allSoundOff(); require(peak(render(engine, 128)) == 0.0, "Panic failed at sample rate");
    }
}

void testNoAudioThreadAllocations() {
    PadEngine engine; setup(engine);
    std::array<float, 128> l {}, r {};
    allocationCount.store(0); trackAllocations.store(true);
    for (int i = 0; i < 96; ++i) {
        const int ch = 1 + i % 16;
        engine.noteOn(36 + i % 48, 0.7f, ch);
        engine.setMorph(static_cast<float>(i % 100) / 100.0f);
        engine.pitchBend(0.1f * (i % 16), ch); engine.sustainPedal(i % 3 == 0, ch);
        engine.render(l.data(), r.data(), 128);
        if (i % 3 == 0) engine.noteOff(36 + i % 48, ch);
        if (i % 13 == 0) engine.allNotesOff(ch);
        if (i % 17 == 0) engine.resetControllers(ch);
        if (i % 23 == 0) engine.allSoundOff(ch);
    }
    engine.allNotesOff(); engine.resetControllers(0); engine.allSoundOff();
    trackAllocations.store(false);
    require(allocationCount.load() == 0, "Allocation on MIDI/render path");
}

void benchmark() {
    PadEngine engine; setup(engine, 0.75f);
    for (int i = 0; i < 32; ++i) engine.noteOn(36 + i, 0.6f);
    const auto start = std::chrono::steady_clock::now();
    advance(engine, 2.0);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "32-voice CPU observation: " << std::fixed << std::setprecision(2)
              << 100.0 * elapsed / 2.0 << "% of audio duration (this host only)\n";
}
}

int main() {
    const std::array<std::pair<const char*, void(*)()>, 12> tests{{
        {"silence and invalid inputs", testSilenceAndInvalidInput},
        {"invalid sample and loop metadata", testInvalidBank},
        {"long held loops and continuous morph", testHeldLoopsAndMorph},
        {"velocity and stereo", testVelocityAndStereo},
        {"four RR variations and deterministic reset", testRoundRobinAndReset},
        {"sustain, channels and panic", testSustainChannelsAndPanic},
        {"repeated notes and stolen-note FIFO", testRepeatedNotesAndStolenFifo},
        {"pitch bend isolation and output mute", testBendChannelIsolationAndMute},
        {"host block independence", testBlockIndependence},
        {"sample rates and extreme MIDI", testRatesAndExtremes},
        {"no audio-thread C++ allocations", testNoAudioThreadAllocations},
        {"32-voice timing observation", benchmark}
    }};
    try {
        for (const auto& test : tests) {
            test.second(); std::cout << "PASS: " << test.first << '\n';
        }
        std::cout << "All 12 Sculpt Pad core checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        trackAllocations.store(false);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

