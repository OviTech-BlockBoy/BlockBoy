/* jit_selftest.c — proves on hardware that the whole Xtensa JIT pipeline works:
 * emitter encodings, windowed ABI (entry/retw), literal pool with l32r, memory
 * access to the ARM register file, callx8 back to C, and dual-mapped executable
 * PSRAM with cache sync.
 *
 * Five tests, report to /BlockBoy/jit_selftest.txt (no serial needed).
 * Call AFTER rg_system_init (storage mounted).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_mmu_map.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"

#include "../components/gbsp-libretro/common.h"
#include "../components/gbsp-libretro/xtensa/xtensa_emit.h"

#define JIT_PAGE_SIZE 0x10000
#define REPORT_PATH   "/sd/BlockBoy/jit_selftest.txt"
#define BLOCK_SPACE   256
#define LIT_SLOTS     8

typedef u32 (*jit_fn_t)(void);

static FILE *report_fp;

static void logline(const char *fmt, ...)
{
  va_list va;
  va_start(va, fmt);
  vprintf(fmt, va);
  va_end(va);
  if (report_fp)
  {
    va_start(va, fmt);
    vfprintf(report_fp, fmt, va);
    va_end(va);
  }
}

/* Testable C callee for the callx8 bridge */
static u32 __attribute__((noinline)) jit_callee_double(u32 x)
{
  return x * 2;
}

/* Finish a block: D-cache writeback + I-cache invalidate, return the exec pointer */
static jit_fn_t finish_block(xemit_t *e, void *exec_view, void *write_view)
{
  u32 len = (u32)(e->ptr - e->block_base);
  u32 len_al = (len + 63) & ~63u;
  u32 off = (u32)(e->block_base - (u8 *)write_view);

  esp_cache_msync(e->block_base, len,
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  esp_cache_msync((u8 *)exec_view + (off & ~63u), len_al + 64,
      ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);

  // Entrypoint = na de literal pool
  return (jit_fn_t)((u8 *)exec_view + off + e->lit_max * 4);
}

bool jit_selftest_run(void)
{
  void *heap_ptr = NULL;
  void *exec_ptr = NULL;
  esp_paddr_t paddr = 0;
  mmu_target_t target;
  int pass = 0, fail = 0;
  xemit_t e;

  report_fp = fopen(REPORT_PATH, "w");
  logline("\n========== XTENSA JIT SELFTEST ==========\n");

  heap_ptr = heap_caps_aligned_alloc(JIT_PAGE_SIZE, JIT_PAGE_SIZE, MALLOC_CAP_SPIRAM);
  if (!heap_ptr ||
      esp_mmu_vaddr_to_paddr(heap_ptr, &paddr, &target) != ESP_OK ||
      esp_mmu_map(paddr, JIT_PAGE_SIZE, MMU_TARGET_PSRAM0,
                  MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT,
                  ESP_MMU_MMAP_FLAG_PADDR_SHARED, &exec_ptr) != ESP_OK)
  {
    logline("[JIT] FAIL: dual-mapping setup failed\n");
    goto done;
  }
  logline("[JIT] write=%p exec=%p\n", heap_ptr, exec_ptr);

  s32 delta = (s32)((u8 *)exec_ptr - (u8 *)heap_ptr);

  /* ---- Test A: ABI — entry/movi/retw, return value ---- */
  {
    u8 *buf = (u8 *)heap_ptr;
    xe_init(&e, buf, BLOCK_SPACE, LIT_SLOTS, delta);
    xe_entry(&e, 32);
    xe_movi(&e, 2, 42);          // a2 = 42 (return value)
    xe_retw(&e);
    jit_fn_t fn = finish_block(&e, exec_ptr, heap_ptr);
    u32 r = e.overflow ? 0xDEAD : fn();
    logline("[JIT] A (ABI/return):       %s (r=%u)\n", r == 42 ? "PASS" : "FAIL", (unsigned)r);
    (r == 42) ? pass++ : fail++;
  }

  /* ---- Test B: literal pool + l32r ---- */
  {
    u8 *buf = (u8 *)heap_ptr + 512;
    xe_init(&e, buf, BLOCK_SPACE, LIT_SLOTS, delta);
    int li = xe_lit(&e, 0xDEADBEEF);
    xe_entry(&e, 32);
    xe_l32r(&e, 2, li);          // a2 = literal
    xe_retw(&e);
    jit_fn_t fn = finish_block(&e, exec_ptr, heap_ptr);
    u32 r = e.overflow ? 0 : fn();
    logline("[JIT] B (literal/l32r):     %s (r=0x%08x)\n", r == 0xDEADBEEF ? "PASS" : "FAIL", (unsigned)r);
    (r == 0xDEADBEEF) ? pass++ : fail++;
  }

  /* ---- Test C: read/write the ARM register file (reg[]) ---- */
  {
    u8 *buf = (u8 *)heap_ptr + 1024;
    xe_init(&e, buf, BLOCK_SPACE, LIT_SLOTS, delta);
    int lreg = xe_lit(&e, (u32)(uintptr_t)&reg[0]);
    reg[0] = 5; reg[1] = 7; reg[2] = 0;
    xe_entry(&e, 32);
    xe_l32r(&e, 3, lreg);        // a3 = &reg[0]
    xe_l32i(&e, 4, 3, 0);        // a4 = reg[0]
    xe_l32i(&e, 5, 3, 4);        // a5 = reg[1]
    xe_add(&e, 6, 4, 5);         // a6 = a4 + a5
    xe_s32i(&e, 6, 3, 8);        // reg[2] = a6
    xe_mov(&e, 2, 6);            // return = sum
    xe_retw(&e);
    jit_fn_t fn = finish_block(&e, exec_ptr, heap_ptr);
    u32 r = e.overflow ? 0 : fn();
    int ok = (r == 12) && (reg[2] == 12);
    logline("[JIT] C (reg[] load/store): %s (r=%u, reg[2]=%u)\n", ok ? "PASS" : "FAIL",
            (unsigned)r, (unsigned)reg[2]);
    ok ? pass++ : fail++;
    reg[0] = reg[1] = reg[2] = 0;
  }

  /* ---- Test D: callx8 naar windowed C-functie ---- */
  {
    u8 *buf = (u8 *)heap_ptr + 1536;
    xe_init(&e, buf, BLOCK_SPACE, LIT_SLOTS, delta);
    int lfn = xe_lit(&e, (u32)(uintptr_t)&jit_callee_double);
    xe_entry(&e, 32);
    xe_l32r(&e, 8, lfn);         // a8 = &callee
    xe_movi(&e, 10, 21);         // a10 = arg
    xe_callx8(&e, 8);            // a10 = callee(21)
    xe_mov(&e, 2, 10);           // return = result
    xe_retw(&e);
    jit_fn_t fn = finish_block(&e, exec_ptr, heap_ptr);
    u32 r = e.overflow ? 0 : fn();
    logline("[JIT] D (callx8 -> C):      %s (r=%u)\n", r == 42 ? "PASS" : "FAIL", (unsigned)r);
    (r == 42) ? pass++ : fail++;
  }

  /* ---- Test E: voorwaartse branch met fixup ---- */
  {
    u8 *buf = (u8 *)heap_ptr + 2048;
    xe_init(&e, buf, BLOCK_SPACE, LIT_SLOTS, delta);
    xe_entry(&e, 32);
    xe_movi(&e, 3, 1);           // a3 = 1
    xe_movi(&e, 4, 2);           // a4 = 2
    u8 *fix = xe_bcc(&e, XE_NE, 3, 4); // bne a3, a4 -> taken
    xe_movi(&e, 2, 99);          // (not-taken path)
    xe_retw(&e);
    xe_patch_bcc(fix, e.ptr);    // taken:
    xe_movi(&e, 2, 77);
    xe_retw(&e);
    jit_fn_t fn = finish_block(&e, exec_ptr, heap_ptr);
    u32 r = e.overflow ? 0 : fn();
    logline("[JIT] E (branch/fixup):     %s (r=%u)\n", r == 77 ? "PASS" : "FAIL", (unsigned)r);
    (r == 77) ? pass++ : fail++;
  }

done:
  logline("==================================================\n");
  logline("[JIT] RESULT: %d PASS, %d FAIL — pipeline %s\n", pass, fail,
          (fail == 0 && pass == 5) ? "FULLY WORKING" : "NOT yet OK");
  logline("==================================================\n\n");

  if (exec_ptr)
    esp_mmu_unmap(exec_ptr);
  if (heap_ptr)
    heap_caps_free(heap_ptr);
  if (report_fp)
  {
    fclose(report_fp);
    report_fp = NULL;
  }
  return fail == 0 && pass == 5;
}
