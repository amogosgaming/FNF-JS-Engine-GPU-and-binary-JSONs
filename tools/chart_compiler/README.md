# Offline Psych chart compiler

This standalone C++17 tool converts a Psych Engine JSON chart into sparse
`song_info.json` metadata/events and a memory-mappable `notes.bin` stream.
It does not load `song.notes` into one managed collection.

## Build

```sh
cmake -S tools/chart_compiler -B build/chart_compiler
cmake --build build/chart_compiler --config Release
```

## Usage

```sh
build/chart_compiler/chart_compiler \
  --input assets/preload/data/bopeebo/bopeebo-hard.json \
  --events assets/preload/data/bopeebo/events.json \
  --output compiled/bopeebo-hard
```

On a multi-config Windows build, the executable is normally under
`build/chart_compiler/Release/`.

The default run size is 1,000,000 records. A temporary sort record occupies
20 serialized bytes, so the principal run buffer is about 20 MB. Sorting is
followed by a bounded 64-way, multi-pass merge; it does not attempt to open an
unbounded number of chunks. Override these values with `--chunk-records` and
`--merge-fan-in`.

## `notes.bin` schema

The file has no header. Its length must be divisible by 12. Every record is
little-endian and has this exact layout:

| Byte offset | Type | Description |
|---:|---|---|
| 0 | `uint64` | Absolute timestamp in milliseconds |
| 8 | `uint16` | Sustain length for this segment |
| 10 | `uint8` | Encoded normalized global lane |
| 11 | `uint8` | Note-type ID |

Records are ordered by timestamp, normalized lane, then stable source order.
Fractional milliseconds use `std::round`: nearest integer with halfway cases
away from zero.

### Lane byte

- Bits `0..6` are the normalized global lane (`lane_id & 0x7f`).
- Bit `7` (`0x80`) repeats the must-hit/player ownership state.
- With four keys per side, player lanes are `0..3` and opponent lanes are
  `4..7` before the ownership flag is added.
- Psych's `mustHitSection` and raw-lane side toggle are resolved offline.

### Note-type byte

- `0x00`: default/normal note.
- `0x01..0xfe`: custom string IDs listed in `song_info.json.note_types`.
- `0xff`: generated sustain continuation; it must not create another hit
  judgement or note head.

A hold longer than 65,535 ms produces a warning and is represented as
continuous chained records. The first record retains the original type. Each
later record starts where the preceding segment ends and uses type `0xff`.

## `song_info.json` schema

Top-level fields include:

- `format`: `"psych-gpu-chart"`
- `version`: currently `1`
- `notes_file`, `endianness`, and `record_size`
- `note_count` and `source_note_count`
- `timestamp_unit` and `timestamp_rounding`
- `lanes_per_side` and `lane_encoding`
- `note_types`: custom string-to-byte dictionary
- `events`: sparse events with pre-rounded absolute `timestamp_ms`
- `metadata`: original song properties except heavy `notes` and `events`

Integers through JavaScript's exact integer ceiling are emitted as JSON
numbers. Larger `uint64` values are emitted as decimal strings and must be
parsed as 64-bit integers by the runtime bridge.

The compiler also emits `__bpm_change` sparse events at absolute section-start
timestamps. Runtime note rendering and hit detection do not need beat-to-time
conversion.

## Failure and temporary-file behavior

Output is first written to `.partial` files and renamed into place only after a
successful write. By default, merge chunks and section spools are removed.
Use `--keep-temp` while diagnosing malformed or exceptionally large charts.
