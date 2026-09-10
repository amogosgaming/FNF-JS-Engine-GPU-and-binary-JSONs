#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace psychgpu {

// notes.bin is a headerless sequence of these little-endian records.
// The packing directive describes the mapped representation on supported
// little-endian hxcpp targets; readers must still validate file_size % 12.
#pragma pack(push, 1)
struct NoteData final {
    std::uint64_t timestamp_ms;
    std::uint16_t sustain_ms;
    std::uint8_t lane_id;
    std::uint8_t note_type;
};
#pragma pack(pop)

inline constexpr std::size_t NOTE_RECORD_SIZE = 12;
inline constexpr std::uint16_t MAX_SUSTAIN_MS = 65535;

inline constexpr std::uint8_t LANE_INDEX_MASK = 0x7f;
inline constexpr std::uint8_t LANE_MUST_HIT_FLAG = 0x80;

inline constexpr std::uint8_t NOTE_TYPE_NORMAL = 0x00;
inline constexpr std::uint8_t NOTE_TYPE_CUSTOM_MAX = 0xfe;
inline constexpr std::uint8_t NOTE_TYPE_CONTINUATION = 0xff;

static_assert(sizeof(NoteData) == NOTE_RECORD_SIZE,
              "NoteData must remain exactly 12 bytes");
static_assert(alignof(NoteData) == 1, "NoteData must remain byte-packed");
static_assert(std::is_trivially_copyable_v<NoteData>);

[[nodiscard]] constexpr std::uint8_t laneIndex(std::uint8_t encoded) noexcept {
    return encoded & LANE_INDEX_MASK;
}

[[nodiscard]] constexpr bool isMustHit(std::uint8_t encoded) noexcept {
    return (encoded & LANE_MUST_HIT_FLAG) != 0;
}

[[nodiscard]] constexpr bool isContinuation(std::uint8_t type) noexcept {
    return type == NOTE_TYPE_CONTINUATION;
}

} // namespace psychgpu
