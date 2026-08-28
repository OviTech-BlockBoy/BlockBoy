/* Xtensa JIT for gpSP on ESP32-S3: core Thumb subset native,
 * the rest via the classic-interpreter seam. */

#ifndef CPU_JIT_H
#define CPU_JIT_H

#include "common.h"

extern u32 jit_enabled;

typedef struct
{
  u32 blocks;
  u32 block_runs;
  u32 classic_calls;
  u32 flushes;
  u32 half_flushes;    // cheap half flushes (oldest arena half; hot half survives)
  u32 verify_ok;       // pure blocks where JIT == interpreter
  u32 verify_mismatch; // pure blocks that differed
  u32 bcc_overflow;    // blocks rejected on bcc range overflow → classic
} jit_stats_t;
extern jit_stats_t jit_stats;

// write_view/exec_view: dual-mapped PSRAM arena (same physical memory);
// hash_mem: plain (PSRAM) buffer of JIT_HASH_BYTES (8192 entries × sizeof).
#define JIT_HASH_BYTES (16384 * 24)   // MUST be (1<<JIT_HASH_BITS) * sizeof(jhash_t)
int jit_init(void *write_view, void *exec_view, u32 size, void *hash_mem);
void jit_invalidate_all(void);

// Hot-block IRAM pinning. iram_buf = executable internal SRAM (direct, no
// I-cache → no fetch stalls). Hot blocks are copied here.
void jit_set_iram(void *iram_buf, u32 iram_size);

// Link-inline cache: PSRAM data array of {gen,addr} slots (buf = nslots*8
// bytes, zero-init). NULL = off (fallback to per-boundary hash lookup).
void jit_set_link_cache(void *buf, u32 nslots);

// Drop-in replacement for execute_arm(); falls back to the interpreter when
// disabled / not initialised / cheats active.
void execute_jit(u32 cycles);

#endif
