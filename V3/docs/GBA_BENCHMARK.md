# GBA benchmark

The benchmark is a diagnostic tool, hidden by default in customer firmware.
Enable it through **Service > Shop Menu > In game > GBA benchmark**, then open
the GBA game menu. Turning it off or restoring Shop defaults hides the entry.
Shop **All On** also enables it; use **Reset** or switch it off before delivery.

No measurement starts automatically. Before the first run, the benchmark has
no large measurement buffer and writes no CSV files. Small inactive checks
remain in the emulator. After a run, its PSRAM buffer is retained until restart.

## Running a measurement

1. Load a repeatable scene or save state and let the game finish booting.
2. Open **GBA benchmark**, select 30, 60, 90 or 120 seconds, then select a mode.
3. The measurement starts after a two-second warmup. The optional FPS overlay
   displays `REC` during recording. Use the same overlay setting for comparisons.
4. Wait for automatic completion. Opening Menu or Options stops and saves a
   partial measurement. Results are saved under `/BlockBoy/benchmarks/` on SD.

**Clean FPS only** is the low-overhead reference mode. **Clean + timings** adds
detailed scope measurements. Both temporarily enable JIT and audio, use normal
speed, disable frameskip and disable scaling. Previous settings are restored
afterwards. **Current + timings** keeps the current settings.

The file is opened and metadata is written before recording. Measurement rows
are buffered in PSRAM and written after recording stops, avoiding benchmark
SD writes during ROM access measurements. Other emulator IO is unaffected.

## Interpreting results

- Emulated FPS counts completed emulation frames over the actual interval.
- Drawn FPS counts frames drawn live or queued for render offload. It does not
  measure physical LCD presentation.
- Frame maximum, threshold counts and a 1 ms p95 histogram describe stalls.
- Detailed timings cover interpreter calls, JIT calls, compilation, ROM page
  loads, CpuSet, CpuFastSet, LZ77, native m4a mixer attempts and rendering.
- Scope times are **inclusive host elapsed times**. Nested scopes overlap,
  offload runs in parallel, and scheduling delays are included. Do not add
  these columns to derive CPU utilization.
- Cycle-counter scopes assume the fixed 240 MHz configuration. An individual
  scope lasting about 17.9 seconds or longer can wrap the counter. Run duration
  and frame timing use the 64-bit microsecond clock.
- ROM page counts record misses for 32 KiB pages, not cache hits or access order.

Repeat each scene at least three times with the same ROM, BIOS, SD card and
settings. Separate first-visit tests after reboot from warm-cache repeat tests.
Loading a save state does not guarantee identical ROM or JIT cache state.
Compare FPS-only and detailed runs to assess measurement overhead.

```powershell
python tools/gba_benchmark_report.py "D:/measurements/*.csv"
```

The report uses total frames divided by total time, including partial intervals.
Builds being compared should contain the same benchmark instrumentation.
Host tests are documented in [GBA tests](../tools/tests/README_GBA.md); hardware
timing, SD export and task scheduling still require testing on the device.
