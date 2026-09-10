#pragma once

#include "note_format.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace psychgpu {

class MappedNoteBuffer final {
public:
    MappedNoteBuffer() noexcept = default;
    ~MappedNoteBuffer();

    MappedNoteBuffer(const MappedNoteBuffer&) = delete;
    MappedNoteBuffer& operator=(const MappedNoteBuffer&) = delete;

    MappedNoteBuffer(MappedNoteBuffer&& other) noexcept;
    MappedNoteBuffer& operator=(MappedNoteBuffer&& other) noexcept;

    // Maps a little-endian notes.bin into virtual address space. An empty,
    // valid file is considered open even though data() is null.
    bool open(const std::string& filepath, std::string& outError);
    void close() noexcept;

    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] bool empty() const noexcept { return noteCount_ == 0; }
    [[nodiscard]] std::size_t count() const noexcept { return noteCount_; }
    [[nodiscard]] std::size_t sizeBytes() const noexcept { return fileSize_; }

    [[nodiscard]] const NoteData* data() const noexcept { return data_; }
    [[nodiscard]] const NoteData& operator[](std::size_t index) const noexcept {
        return data_[index];
    }

    // First timestamp >= targetMs, or count() when no such record exists.
    [[nodiscard]] std::size_t findFirstAtOrAfter(
        std::uint64_t targetMs) const noexcept;

    // First timestamp > targetMs, or count() when no such record exists.
    [[nodiscard]] std::size_t findFirstAfter(
        std::uint64_t targetMs) const noexcept;

    // Returns the half-open index range containing records whose timestamps
    // are in the inclusive time interval [startMs, endMs].
    [[nodiscard]] std::pair<std::size_t, std::size_t> findVisibleRange(
        std::uint64_t startMs,
        std::uint64_t endMs) const noexcept;

private:
    const NoteData* data_ = nullptr;
    std::size_t fileSize_ = 0;
    std::size_t noteCount_ = 0;
    bool open_ = false;

#if defined(_WIN32)
    void* fileHandle_ = nullptr;
    void* mappingHandle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace psychgpu
