/* Execute the actual before/after C++ templates on synthetic scanlines.
 * No Windows SDK/CRT is required. The palette getter models R_CTX selection
 * and counts selections; this is not a host timing benchmark for the S3. */
typedef unsigned int u32;
typedef unsigned short u16;
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define REG_BLDALPHA 0
#define REG_BLDY 1
static u16 palettes[2][512];
static u16 registers[2][2];
static u32 active_core, lookups;
static u16 *get_palette(void) { lookups++; return palettes[active_core]; }
#define palette_ram_converted get_palette()
#define read_ioreg(index) registers[active_core][index]
namespace reference {
#include "fixtures/render_blend_reference.inc"
}
namespace candidate {
#include "render_candidate.inc"
}

static u32 inputs[256];
static u16 original[256], revised[256];
static u32 rng = 0xB10C2026;
static u32 random_word(void)
{
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

#define RUN_MODE(ns, tag) \
  if (st) ns::merge_blend<ns::tag, true>(start, end, dst, inputs); \
  else ns::merge_blend<ns::tag, false>(start, end, dst, inputs)
#define RUN_BLEND(ns) \
  switch (mode) { \
    case 0: RUN_MODE(ns, OBJ_BLEND); break; \
    case 1: RUN_MODE(ns, BLEND_ONLY); break; \
    case 2: RUN_MODE(ns, BLEND_BRIGHT); break; \
    case 3: RUN_MODE(ns, BLEND_DARK); break; \
  }
static void blend(bool optimized, u32 mode, bool st, u32 start, u32 end, u16 *dst)
{
  if (optimized) { RUN_BLEND(candidate); }
  else { RUN_BLEND(reference); }
}
static u32 last_ref, last_new;
extern "C" __declspec(dllexport) int reference_lookups(void) { return (int)last_ref; }
extern "C" __declspec(dllexport) int candidate_lookups(void) { return (int)last_new; }
extern "C" __declspec(dllexport) int test_render(void)
{
  for (u32 core = 0; core < 2; core++)
    for (u32 i = 0; i < 512; i++) palettes[core][i] = (u16)random_word();
  /* All 32^3 combinations of alpha A, alpha B and brightness, including
   * out-of-range factors that clamp to 16. Alternate the two core palettes. */
  for (u32 test = 0; test < 32768; test++) {
    active_core = test & 1;
    registers[active_core][0] = (u16)((test & 31) | (((test >> 5) & 31) << 8));
    registers[active_core][1] = (u16)((test >> 10) & 31);
    // Distinct other-core registers detect accidental live/snapshot mixing.
    registers[1 - active_core][0] = (u16)random_word();
    registers[1 - active_core][1] = (u16)random_word();
    u32 start = test % 7 == 0 ? 0 : random_word() % 241;
    u32 end = test % 7 == 0 ? 240 : start + random_word() % (241 - start);
    for (u32 i = 0; i < 256; i++) inputs[i] = random_word();
    for (u32 mode = 0; mode < 4; mode++) {
      for (u32 st = 0; st < 2; st++) {
        for (u32 i = 0; i < 256; i++) original[i] = revised[i] = 0xA55A;
        blend(false, mode, st != 0, start, end, original);
        lookups = 0;
        blend(true, mode, st != 0, start, end, revised);
        CHECK(lookups == 1);
        for (u32 i = 0; i < 256; i++) {
          CHECK(original[i] == revised[i]);
          if (i < start || i >= end) CHECK(revised[i] == 0xA55A);
        }
      }
    }
    for (u32 dark = 0; dark < 2; dark++) {
      for (u32 i = 0; i < 256; i++) original[i] = revised[i] = (u16)inputs[i];
      if (dark) reference::merge_brightness<reference::BLEND_DARK>(start, end, original);
      else reference::merge_brightness<reference::BLEND_BRIGHT>(start, end, original);
      lookups = 0;
      if (dark) candidate::merge_brightness<candidate::BLEND_DARK>(start, end, revised);
      else candidate::merge_brightness<candidate::BLEND_BRIGHT>(start, end, revised);
      CHECK(lookups == 1);
      for (u32 i = 0; i < 256; i++) CHECK(original[i] == revised[i]);
    }
  }
  // Representative full blend: both palette lookups per pixel become one per span.
  for (u32 i = 0; i < 240; i++) inputs[i] = 0x04000200u | (i << 16) | i;
  lookups = 0;
  blend(false, 1, false, 0, 240, original);
  last_ref = lookups;
  lookups = 0;
  blend(true, 1, false, 0, 240, revised);
  last_new = lookups;
  CHECK(last_ref == 480 && last_new == 1);
  return 0;
}
