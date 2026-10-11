#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Menu requests a run; main starts it after leaving the menu. */
bool gba_benchmark_active(void);
void gba_benchmark_begin_frame(bool boot_ready);
void gba_benchmark_end_frame(uint32_t frame_us, bool drawn, bool offloaded,
                             uint32_t execute_us, uint32_t output_us,
                             uint32_t pacing_us, uint32_t wait_us);
void gba_benchmark_stop(const char *reason);
void gba_benchmark_offload(uint32_t render_us, uint32_t submit_us);
bool gba_benchmark_detail(void);
/* Implemented by the emulator, only called outside the timed interval. */
void gba_benchmark_settings(bool apply);
bool gba_benchmark_drain(void);
int gba_benchmark_frameskip(void);
