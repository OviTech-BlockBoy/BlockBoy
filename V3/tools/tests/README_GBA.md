# GBA host regressions

Run from the repository root on Windows. The tests extract the current C functions
from `cpu_jit.c` and `gba_memory.c` and execute them in a native DLL. The harness
mocks SD/stdio and memory-map publication; it does not emulate the S3 or test the
generated Xtensa instructions.

Prerequisites: Python 3, Zig 0.14.1 (C compiler), and an LLVM `lld.exe` linker.
The ESP-Clang package installed with ESP-IDF supplies the latter. ESP-Clang itself
was built without an x86 backend and cannot compile these host tests.

Local setup used for this round, with all downloaded files inside ignored `build/`:

```powershell
& 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' -m pip install --no-deps --target build\gba-test-toolchain ziglang==0.14.1
& 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools/tests/test_gba_hotpaths.py --zig build\gba-test-toolchain\ziglang\zig.exe --linker 'C:\Espressif\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin\lld.exe'
```

Expected: four PASS lines (JIT and ROM tests, each with ROM_RAW_READ=1 and =0).
Artifacts and compiler caches are written to `build/gba-host-tests/`.

Coverage:

- Hash collisions preserve probe chains when the first entry is removed.
- Tombstone reuse restores the live count; repeated half-flushes do not underflow it.
- Half-flush generations advance and surviving entries retain their metadata.
- A 16MiB ROM fills two full cache blocks and a partial block with correct page mappings.
- Every mixture of success/failure across three raw reads preserves byte-identical content.
- A partially failed raw read is completely overwritten by the stdio fallback.
- An unavailable rawrom handle and a build with rawrom disabled both use stdio.
- Aligned/unaligned final blocks are bounded by file size, padded deterministically,
  and do not overwrite bytes outside the final mapped page.
- Open, seek, size, allocation-capacity and short-read failures return errors and close handles.

The original `jhash_insert` failed the live-count check immediately after tombstone
reuse. Moving `jit_hash_used++` below `place:` made that test pass. This was verified
before adding the ROM tests; it was not inferred from a source-text assertion.

## Renderer palette lookup regression

```powershell
& 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools/tests/test_gba_render.py --zig build\gba-test-toolchain\ziglang\zig.exe --linker 'C:\Espressif\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin\lld.exe'
```

This extracts the current `merge_blend` and `merge_brightness` templates from
`video.cpp` and compares them to the unmodified routines saved in
`fixtures/render_blend_reference.inc`. It checks 32768 cases for each color format
(RGB565 and XBGR1555), all blend modes, semi-transparent objects, two distinct
palettes, all combinations of 5-bit blend/brightness factors, and full, clipped
and empty segments. Output outside each segment is also checked.

The palette getter counts context selections. A full blend of 240 pixels requires
480 selections in the reference and one in the candidate. This is a count of
avoided work, not a timing measurement or FPS multiplier. These tests do not
exercise task scheduling, snapshots, or the full scanline renderer.

## V3 benchmark

```powershell
& 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools/tests/test_gba_benchmark.py --zig build\gba-test-toolchain\ziglang\zig.exe --linker 'C:\Espressif\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin\lld.exe'
& 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools/tests/test_gba_benchmark_report.py
```

The native harness compiles the actual `gbsp/main/rg_benchmark.c` with mocked
platform services. It checks allocation/open/header-write failures, boot and
render-drain guards, cancellation during warmup, stopping and restoring Clean,
current-settings mode, automatic finish, partial intervals, buffer reuse,
120-second capacity, offload counters, and no CSV writes while recording.
`gba_profile.h` is tested with a fake cycle clock, including wrapping through
zero and a scope starting exactly at zero. The Python report test checks
weighted FPS over unequal intervals and rejects empty/legacy input.

These mocks do not verify actual SD files, screen menus, audio, multicore
scheduling, or measurement overhead. Use `docs/GBA_BENCHMARK.md` for device testing.
