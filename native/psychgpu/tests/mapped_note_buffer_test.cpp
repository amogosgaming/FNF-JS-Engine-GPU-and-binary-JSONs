#include "mapped_note_buffer.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using psychgpu::MappedNoteBuffer;
using psychgpu::NoteData;

namespace {

void writeU16(std::ostream& out, std::uint16_t value) {
    out.put(static_cast<char>(value & 0xff));
    out.put(static_cast<char>((value >> 8) & 0xff));
}

void writeU64(std::ostream& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 0xff));
    }
}

void writeNote(std::ostream& out, const NoteData& note) {
    writeU64(out, note.timestamp_ms);
    writeU16(out, note.sustain_ms);
    out.put(static_cast<char>(note.lane_id));
    out.put(static_cast<char>(note.note_type));
}

fs::path uniquePath(const char* suffix) {
    const auto token = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    return fs::temp_directory_path() /
           ("psychgpu-mapped-note-" + std::to_string(token) + suffix);
}

} // namespace

int main() {
    const fs::path valid = uniquePath(".bin");
    const fs::path empty = uniquePath("-empty.bin");
    const fs::path invalid = uniquePath("-invalid.bin");

    {
        std::ofstream out(valid, std::ios::binary | std::ios::trunc);
        const std::array<NoteData, 5> notes{{
            {10, 0, 0x80, 0},
            {20, 0, 0x81, 0},
            {20, 50, 0x82, 1},
            {30, 0, 4, 0},
            {40, 0, 5, 2},
        }};
        for (const auto& note : notes) writeNote(out, note);
    }
    std::ofstream(empty, std::ios::binary | std::ios::trunc).close();
    {
        std::ofstream out(invalid, std::ios::binary | std::ios::trunc);
        out << "bad";
    }

    std::string error;
    MappedNoteBuffer buffer;
    assert(buffer.open(valid.string(), error));
    assert(buffer.isOpen());
    assert(buffer.count() == 5);
    assert(buffer.sizeBytes() == 60);
    assert(buffer[2].timestamp_ms == 20);
    assert(buffer.findFirstAtOrAfter(0) == 0);
    assert(buffer.findFirstAtOrAfter(20) == 1);
    assert(buffer.findFirstAtOrAfter(21) == 3);
    assert(buffer.findFirstAtOrAfter(100) == 5);
    assert(buffer.findFirstAfter(20) == 3);
    assert(buffer.findVisibleRange(20, 30) == std::make_pair<std::size_t, std::size_t>(1, 4));
    assert(buffer.findVisibleRange(31, 39) == std::make_pair<std::size_t, std::size_t>(4, 4));
    assert(buffer.findVisibleRange(40, 20) == std::make_pair<std::size_t, std::size_t>(0, 0));

    MappedNoteBuffer moved(std::move(buffer));
    assert(!buffer.isOpen());
    assert(moved.isOpen());
    assert(moved.count() == 5);
    moved.close();
    assert(!moved.isOpen());

    error.clear();
    assert(moved.open(empty.string(), error));
    assert(moved.isOpen());
    assert(moved.empty());
    assert(moved.data() == nullptr);
    moved.close();

    error.clear();
    assert(!moved.open(invalid.string(), error));
    assert(!error.empty());
    assert(!moved.isOpen());

    std::error_code ignored;
    fs::remove(valid, ignored);
    fs::remove(empty, ignored);
    fs::remove(invalid, ignored);
    return 0;
}
