#include "../Source/PadEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int sampleRate = 44100;
std::uint16_t read16(std::istream& in) {
    unsigned char b[2]{}; in.read(reinterpret_cast<char*>(b), 2);
    return static_cast<std::uint16_t>(b[0] | (static_cast<unsigned>(b[1]) << 8));
}
std::uint32_t read32(std::istream& in) {
    unsigned char b[4]{}; in.read(reinterpret_cast<char*>(b), 4);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8)
         | (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}
void write16(std::ostream& out, std::uint16_t value) {
    const char b[2] = {static_cast<char>(value), static_cast<char>(value >> 8)}; out.write(b, 2);
}
void write32(std::ostream& out, std::uint32_t value) {
    const char b[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                      static_cast<char>(value >> 16), static_cast<char>(value >> 24)}; out.write(b, 4);
}
sculptpad::Sample loadWav(const std::filesystem::path& path, int root, int rr, std::size_t loopStart,
                          std::size_t loopEnd, std::size_t loopCrossfade) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open sample: " + path.string());
    char magic[4]{}; in.read(magic, 4);
    if (std::memcmp(magic, "RIFF", 4)) throw std::runtime_error("Expected RIFF WAV");
    (void)read32(in); in.read(magic, 4);
    if (std::memcmp(magic, "WAVE", 4)) throw std::runtime_error("Expected WAVE data");
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0, dataBytes = 0;
    std::streampos dataPosition{};
    while (in.read(magic, 4)) {
        const auto size = read32(in);
        const auto start = in.tellg();
        if (!std::memcmp(magic, "fmt ", 4)) {
            format = read16(in); channels = read16(in); rate = read32(in);
            (void)read32(in); (void)read16(in); bits = read16(in);
        } else if (!std::memcmp(magic, "data", 4)) {
            dataBytes = size; dataPosition = start;
        }
        in.seekg(start + static_cast<std::streamoff>(size + (size & 1U)));
    }
    if (format != 1 || bits != 16 || channels != 2 || rate < 8000 || dataBytes == 0)
        throw std::runtime_error("Preview requires stereo PCM16 WAV: " + path.string());
    in.clear(); in.seekg(dataPosition);
    sculptpad::Sample s;
    s.rootNote = root; s.roundRobin = rr; s.sampleRate = rate;
    s.loopStart = loopStart; s.loopEnd = loopEnd; s.loopCrossfade = loopCrossfade;
    const auto frames = dataBytes / 4U;
    s.left.resize(frames); s.right.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        s.left[i] = static_cast<float>(static_cast<std::int16_t>(read16(in))) / 32768.0f;
        s.right[i] = static_cast<float>(static_cast<std::int16_t>(read16(in))) / 32768.0f;
    }
    if (!in) throw std::runtime_error("Truncated sample: " + path.string());
    return s;
}
std::vector<sculptpad::Sample> loadBank(const std::filesystem::path& directory) {
    std::ifstream index(directory / "preview-bank.tsv");
    if (!index) throw std::runtime_error("Missing Samples/preview-bank.tsv");
    std::vector<sculptpad::Sample> bank;
    std::string line;
    while (std::getline(index, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string name; int note = -1, rr = -1;
        std::size_t loopStart = 0, loopEnd = 0, loopCrossfade = 0;
        if (!(row >> name >> note >> rr >> loopStart >> loopEnd >> loopCrossfade)) throw std::runtime_error("Invalid preview bank row");
        bank.push_back(loadWav(directory / name, note, rr, loopStart, loopEnd, loopCrossfade));
    }
    if (bank.empty()) throw std::runtime_error("Empty sample bank");
    return bank;
}
enum class Kind { noteOn, noteOff, pedal };
struct Event { int frame; Kind kind; int note; float value; };
int frame(double seconds) { return static_cast<int>(std::lround(seconds * sampleRate)); }
std::vector<Event> phrase() {
    // Identical sustained chords at every anchor; no external effects or
    // per-section gain changes. A gentle spread of note-ons resembles playing.
    const std::array<std::array<int, 5>, 2> chords{{
        {{48, 55, 59, 62, 67}}, {{45, 52, 55, 59, 64}}
    }};
    std::vector<Event> events;
    for (std::size_t c = 0; c < chords.size(); ++c) {
        const double start = static_cast<double>(c) * 3.7;
        for (std::size_t n = 0; n < chords[c].size(); ++n) {
            events.push_back({frame(start + 0.012 * static_cast<double>(n)),
                              Kind::noteOn, chords[c][n], 0.72f});
            events.push_back({frame(start + 3.25), Kind::noteOff, chords[c][n], 0.0f});
        }
    }
    return events;
}
void renderSection(sculptpad::PadEngine& engine, std::vector<Event> events, int offset,
                   int length, std::vector<float>& left, std::vector<float>& right, bool sweep) {
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.frame < b.frame;
    });
    std::size_t eventIndex = 0;
    for (int position = 0; position < length;) {
        while (eventIndex < events.size() && events[eventIndex].frame <= position) {
            const auto& e = events[eventIndex++];
            if (e.kind == Kind::noteOn) engine.noteOn(e.note, e.value);
            else if (e.kind == Kind::noteOff) engine.noteOff(e.note);
            else engine.sustainPedal(e.value >= 0.5f);
        }
        if (sweep) engine.setMorph(std::clamp((static_cast<float>(position) / sampleRate - 2.0f) / 11.0f, 0.0f, 1.0f));
        int count = std::min(64, length - position);
        if (eventIndex < events.size()) count = std::min(count, events[eventIndex].frame - position);
        engine.render(left.data() + offset + position, right.data() + offset + position, count);
        position += count;
    }
}
void saveWav(const std::filesystem::path& path, const std::vector<float>& left,
             const std::vector<float>& right) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot write preview");
    const auto dataBytes = static_cast<std::uint32_t>(left.size() * 4);
    out.write("RIFF", 4); write32(out, dataBytes + 36); out.write("WAVEfmt ", 8);
    write32(out, 16); write16(out, 1); write16(out, 2); write32(out, sampleRate);
    write32(out, sampleRate * 4); write16(out, 4); write16(out, 16);
    out.write("data", 4); write32(out, dataBytes);
    std::size_t clipped = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        for (const float x : {left[i], right[i]}) {
            if (!std::isfinite(x)) throw std::runtime_error("Non-finite preview output");
            if (std::abs(x) >= 1.0f) ++clipped;
            const auto value = static_cast<std::int16_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
            write16(out, static_cast<std::uint16_t>(value));
        }
    }
    if (!out) throw std::runtime_error("Incomplete preview write");
    std::cout << "Clipped output samples: " << clipped << '\n';
    if (clipped != 0) throw std::runtime_error("Preview clips at PCM full scale");
}
}

int main(int argc, char** argv) {
    try {
        const std::filesystem::path samples = argc > 1 ? argv[1] : "Samples";
        const std::filesystem::path output = argc > 2 ? argv[2] : "Preview/SculptPad-demo.wav";
        sculptpad::PadEngine engine;
        auto bank = loadBank(samples);
        std::cout << "Loaded " << bank.size() << " stereo samples\n";
        engine.setSamples(std::move(bank));
        if (!engine.hasSamples()) throw std::runtime_error("The engine rejected the bank");
        std::vector<float> left(frame(68.0)), right(left.size());
        for (int anchor = 0; anchor < 4; ++anchor) {
            engine.setMorph(static_cast<float>(anchor) / 3.0f);
            engine.setOutputGain(1.0f); engine.prepare(sampleRate);
            renderSection(engine, phrase(), frame(anchor * 12.0), frame(12.0), left, right, false);
            std::cout << "Rendered anchor " << anchor << '\n';
        }
        engine.setMorph(0.0f); engine.prepare(sampleRate);
        std::vector<Event> held{{0, Kind::pedal, 0, 1.0f}};
        const std::array<int, 5> sweepChord{{48, 55, 58, 62, 67}};
        for (std::size_t i = 0; i < sweepChord.size(); ++i) {
            held.push_back({frame(0.008 * static_cast<double>(i)), Kind::noteOn, sweepChord[i], 0.72f});
            held.push_back({frame(1.0), Kind::noteOff, sweepChord[i], 0.0f});
        }
        held.push_back({frame(15.0), Kind::pedal, 0, 0.0f});
        renderSection(engine, held, frame(48.0), frame(20.0), left, right, true);
        saveWav(output, left, right);
        const auto peakL = *std::max_element(left.begin(), left.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
        const auto peakR = *std::max_element(right.begin(), right.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
        std::cout << "Peak dBFS: " << 20.0 * std::log10(std::max(std::abs(peakL), std::abs(peakR)))
                  << "\nSaved " << output << " (68 seconds, stereo, 44.1 kHz)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Preview failed: " << error.what() << '\n'; return 1;
    }
}
