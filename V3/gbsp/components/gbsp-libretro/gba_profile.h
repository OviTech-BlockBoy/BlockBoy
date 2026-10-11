#ifndef GBA_PROFILE_H
#define GBA_PROFILE_H
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Inclusive host elapsed time, not CPU utilization. Nested categories overlap.
 * Only the emulator task writes these counters. Offload has a separate hook. */
enum { GP_CLASSIC, GP_JIT, GP_COMPILE, GP_ROM, GP_CPUSET, GP_CPUFASTSET,
       GP_LZ77, GP_LIVE_RENDER, GP_M4A, GP_COUNT };
typedef struct { uint64_t cycles; uint32_t calls; } gba_profile_counter;
typedef struct {
    gba_profile_counter counter[GP_COUNT];
    uint32_t pages[1024];
} gba_profile_data;
extern gba_profile_data *gba_profile_current;
static inline uint32_t gba_profile_begin(void)
{
    if (!gba_profile_current) return 0;
    uint32_t cycles = 0;
#ifdef __XTENSA__
    __asm__ volatile("rsr.ccount %0" : "=a"(cycles) :: "memory");
#else
    extern uint32_t gba_profile_test_clock(void);
    cycles = gba_profile_test_clock();
#endif
    return cycles;
}
static inline void gba_profile_end(unsigned kind, uint32_t start)
{
    if (!gba_profile_current) return;
    uint32_t elapsed = gba_profile_begin() - start;
    gba_profile_current->counter[kind].cycles += elapsed;
    gba_profile_current->counter[kind].calls++;
}
#ifdef __cplusplus
}
#endif
#endif
