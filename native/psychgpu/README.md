# PsychGPU native backend

`MappedNoteBuffer` maps Component 1's `notes.bin` directly into the process
address space and provides allocation-free indexed access and binary searches.

## Properties

- Win32: `CreateFileW`, `CreateFileMappingW`, and `MapViewOfFile`.
- POSIX: `open`, `fstat`, and read-only private `mmap`.
- No note copies or per-query heap allocations.
- `findFirstAtOrAfter()` returns the lower timestamp bound.
- `findFirstAfter()` returns the upper timestamp bound.
- `findVisibleRange(start, end)` returns a half-open index interval for the
  inclusive timestamp interval `[start, end]`.
- An empty but valid file is represented by `isOpen() == true`, `count() == 0`,
  and `data() == nullptr`.
- Paths passed on Windows are interpreted as UTF-8 and converted with
  `MultiByteToWideChar`.

The mapped representation assumes a little-endian runtime, matching the
compiler's binary output and supported hxcpp desktop targets.

## Standalone CMake build

```sh
cmake -S native/psychgpu -B build/psychgpu-native
cmake --build build/psychgpu-native --config Release
```

The Haxe/hxcpp extern and game build wiring intentionally belong to Component
3. Until that integration lands, this target can be compiled independently for
native validation.
