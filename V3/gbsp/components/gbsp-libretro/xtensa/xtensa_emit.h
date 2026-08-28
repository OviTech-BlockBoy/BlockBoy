/* Xtensa LX7 (ESP32-S3) machine-code emitter for the gpSP JIT.
 *
 * Every encoding below is verified against xtensa-esp-elf-as 14.2.0 —
 * bytes are in memory order.
 *
 * Conventions of the generated code:
 * - Each block is a plain windowed function: starts with ENTRY a1,frame and
 *   ends with RETW.N. Callable as a C function pointer; C functions are called
 *   from the block with CALLX8 (args a10/a11, result a10).
 * - Literal pool at the head of the block (L32R only reaches backwards);
 *   fixed slots, filled on demand at emit time.
 * - Only wide (24-bit) instructions + retw.n/nop.n. */

#ifndef XTENSA_EMIT_H
#define XTENSA_EMIT_H

#include "common.h"

typedef struct
{
  u8 *block_base;   // write view: start of the block (= literal pool)
  u8 *code_start;   // write view: first instruction byte (after the pool)
  u8 *ptr;          // write view: current emit position
  u8 *limit;        // write view: end of available space
  u32 *lits;        // write view: literal slots (= block_base)
  u32 lit_count;
  u32 lit_max;
  s32 exec_delta;   // exec_view_addr - write_view_addr (constant offset)
  int overflow;     // 1 once space/literals run out → reject the block
} xemit_t;

/* Set when a bcc patch falls outside the ±127-byte range. translate_block then
 * rejects the block → classic fallback instead of a silent misjump (the imm8 was
 * previously truncated without a check). Reset per block in xe_init.
 * TU-local: cpu_jit.c and jit_selftest.c each have their own copy. */
static int xe_bcc_overflow __attribute__((unused));

/* ------------------------------------------------------------------ */
/* Basis                                                               */
/* ------------------------------------------------------------------ */

static inline void xe_init(xemit_t *e, u8 *buf, u32 size, u32 lit_slots,
                           s32 exec_delta)
{
  e->block_base = buf;
  e->lits = (u32 *)buf;
  e->lit_max = lit_slots;
  e->lit_count = 0;
  e->code_start = buf + lit_slots * 4;
  e->ptr = e->code_start;
  e->limit = buf + size;
  e->exec_delta = exec_delta;
  e->overflow = 0;
  xe_bcc_overflow = 0;   // reset bcc range overflow per block
}

static inline void xe_byte3(xemit_t *e, u8 b0, u8 b1, u8 b2)
{
  if (e->ptr + 3 > e->limit) { e->overflow = 1; return; }
  e->ptr[0] = b0; e->ptr[1] = b1; e->ptr[2] = b2;
  e->ptr += 3;
}

static inline void xe_byte2(xemit_t *e, u8 b0, u8 b1)
{
  if (e->ptr + 2 > e->limit) { e->overflow = 1; return; }
  e->ptr[0] = b0; e->ptr[1] = b1;
  e->ptr += 2;
}

/* Allocate a literal slot (reuses an existing value if present) */
static inline int xe_lit(xemit_t *e, u32 value)
{
  u32 i;
  for (i = 0; i < e->lit_count; i++)
    if (e->lits[i] == value)
      return (int)i;
  if (e->lit_count >= e->lit_max) { e->overflow = 1; return 0; }
  e->lits[e->lit_count] = value;
  return (int)e->lit_count++;
}

/* ------------------------------------------------------------------ */
/* Instructions (encodings verified against the assembler)             */
/* ------------------------------------------------------------------ */

/* entry a1, frame   — "36 41 00" for frame=32 */
static inline void xe_entry(xemit_t *e, u32 frame)
{
  u32 imm12 = frame >> 3;
  xe_byte3(e, 0x36, (u8)(((imm12 & 0xF) << 4) | 1), (u8)(imm12 >> 4));
}

/* retw.n — "1d f0" */
static inline void xe_retw(xemit_t *e) { xe_byte2(e, 0x1d, 0xf0); }
/* ret.n — "0d f0"  (call0 ABI: jump to a0, NO window rotation) */
static inline void xe_ret(xemit_t *e) { xe_byte2(e, 0x0d, 0xf0); }
/* nop.n — "3d f0" */
static inline void xe_nop(xemit_t *e) { xe_byte2(e, 0x3d, 0xf0); }

/* movi t, imm12 (signed -2048..2047) — "a2 a7 ff" = movi a10, 0x7ff */
static inline void xe_movi(xemit_t *e, u32 t, s32 imm)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0xA0 | ((imm >> 8) & 0xF)),
           (u8)(imm & 0xFF));
}

/* l32i t, s, off (off = veelvoud van 4, 0..1020) — "a2 2b ff" */
static inline void xe_l32i(xemit_t *e, u32 t, u32 s, u32 off)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0x20 | s), (u8)(off >> 2));
}

/* s32i t, s, off — "a2 6b ff" */
static inline void xe_s32i(xemit_t *e, u32 t, u32 s, u32 off)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0x60 | s), (u8)(off >> 2));
}

/* l8ui t, s, off (0..255) — "22 03 07" */
static inline void xe_l8ui(xemit_t *e, u32 t, u32 s, u32 off)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0x00 | s), (u8)off);
}

/* s8i t, s, off — "22 43 07" */
static inline void xe_s8i(xemit_t *e, u32 t, u32 s, u32 off)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0x40 | s), (u8)off);
}

/* l16ui t, s, off (even, 0..510) — "22 13 03" */
static inline void xe_l16ui(xemit_t *e, u32 t, u32 s, u32 off)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0x10 | s), (u8)(off >> 1));
}

/* s16i t, s, off — "22 53 03" */
static inline void xe_s16i(xemit_t *e, u32 t, u32 s, u32 off)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0x50 | s), (u8)(off >> 1));
}

/* RRR forms: r = dest, s/t = sources.
 * sub a2,a3,a4 = "40 23 c0"; and=0x10 or=0x20 xor=0x30 add=0x80 mull=0x82 */
static inline void xe_rrr(xemit_t *e, u8 op2, u32 r, u32 s, u32 t)
{
  xe_byte3(e, (u8)(t << 4), (u8)((r << 4) | s), op2);
}
#define xe_add(e, r, s, t)  xe_rrr(e, 0x80, r, s, t)
#define xe_sub(e, r, s, t)  xe_rrr(e, 0xC0, r, s, t)
#define xe_and(e, r, s, t)  xe_rrr(e, 0x10, r, s, t)
#define xe_or(e, r, s, t)   xe_rrr(e, 0x20, r, s, t)
#define xe_xor(e, r, s, t)  xe_rrr(e, 0x30, r, s, t)
#define xe_mull(e, r, s, t) xe_rrr(e, 0x82, r, s, t)
#define xe_mov(e, r, s)     xe_or(e, r, s, s)

/* neg r, t — "30 20 60" = neg a2, a3 (t = source) */
static inline void xe_neg(xemit_t *e, u32 r, u32 t)
{
  xe_byte3(e, (u8)(t << 4), (u8)(r << 4), 0x60);
}

/* addi t, s, imm8 (signed) — "22 c3 fb" = addi a2, a3, -5 */
static inline void xe_addi(xemit_t *e, u32 t, u32 s, s32 imm)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0xC0 | s), (u8)(imm & 0xFF));
}

/* addmi t, s, imm8<<8 — "22 d3 01" = addmi a2, a3, 0x100 */
static inline void xe_addmi(xemit_t *e, u32 t, u32 s, s32 imm)
{
  xe_byte3(e, (u8)((t << 4) | 0x02), (u8)(0xD0 | s), (u8)((imm >> 8) & 0xFF));
}

/* slli r, s, sa (1..31) — sa=12: "40 23 11"; sa=31: "10 23 01" */
static inline void xe_slli(xemit_t *e, u32 r, u32 s, u32 sa)
{
  u32 n = 32 - sa;
  xe_byte3(e, (u8)((n & 0xF) << 4), (u8)((r << 4) | s),
           (u8)((n & 0x10) ? 0x11 : 0x01));
}

/* srli r, t, sa (0..15) — sa=15: "30 2f 41" (t = source) */
static inline void xe_srli(xemit_t *e, u32 r, u32 t, u32 sa)
{
  xe_byte3(e, (u8)(t << 4), (u8)((r << 4) | sa), 0x41);
}

/* srai r, t, sa (0..31) — sa=31: "30 2f 31" */
static inline void xe_srai(xemit_t *e, u32 r, u32 t, u32 sa)
{
  xe_byte3(e, (u8)(t << 4), (u8)((r << 4) | (sa & 0xF)),
           (u8)(0x21 | ((sa >> 4) << 4)));
}

/* extui r, t, shift(0..15), bits(1..16) — "30 2c 34" = extui a2,a3,12,4 */
static inline void xe_extui(xemit_t *e, u32 r, u32 t, u32 shift, u32 bits)
{
  /* EXTUI: shiftimm is 5 bits — sa3:0 in the s field (inst[11:8]), sa4 in op1 bit0
   * (inst[16] = byte2 bit0). Without sa4 extui only works for shift<16. */
  xe_byte3(e, (u8)(t << 4), (u8)((r << 4) | (shift & 0xF)),
           (u8)(((bits - 1) << 4) | 0x04 | ((shift >> 4) & 1)));
}

/* sext r, s, imm (imm 7..22) — sign-extend from bit imm. Emit order (mem):
 * "80 34 23" = sext a3,a4,15 (objdump shows it reversed "233480"). imm=15 = (s16). */
static inline void xe_sext(xemit_t *e, u32 r, u32 s, u32 imm)
{
  xe_byte3(e, (u8)((imm - 7) << 4), (u8)((r << 4) | s), 0x23);
}

/* Variable shifts via SAR: ssl/ssr s, then sll/srl/sra */
static inline void xe_ssl(xemit_t *e, u32 s) { xe_byte3(e, 0x00, (u8)(0x10 | s), 0x40); }
static inline void xe_ssr(xemit_t *e, u32 s) { xe_byte3(e, 0x00, (u8)(0x00 | s), 0x40); }
static inline void xe_sll(xemit_t *e, u32 r, u32 s) { xe_byte3(e, 0x00, (u8)((r << 4) | s), 0xA1); }
static inline void xe_srl(xemit_t *e, u32 r, u32 t) { xe_byte3(e, (u8)(t << 4), (u8)(r << 4), 0x91); }
static inline void xe_sra(xemit_t *e, u32 r, u32 t) { xe_byte3(e, (u8)(t << 4), (u8)(r << 4), 0xB1); }

/* jx s — "a0 03 00" */
static inline void xe_jx(xemit_t *e, u32 s) { xe_byte3(e, 0xA0, (u8)s, 0x00); }

/* callx8 s — "e0 0a 00" = callx8 a10 */
static inline void xe_callx8(xemit_t *e, u32 s) { xe_byte3(e, 0xE0, (u8)s, 0x00); }

/* callx0 s — "c0 08 00" = callx0 a8 (call0 ABI: a0 = link, NO window rotation).
 * Used for the mem stubs: the block saves its own windowed retw link (a0)
 * around this call. */
static inline void xe_callx0(xemit_t *e, u32 s) { xe_byte3(e, 0xC0, (u8)s, 0x00); }

/* ------------------------------------------------------------------ */
/* Branches with fixups (offsets within the block; both views linear) */
/* ------------------------------------------------------------------ */

/* Conditions (r field): beq=1 blt=2 bltu=3 bne=9 bge=A bgeu=B */
#define XE_EQ  0x1
#define XE_LT  0x2
#define XE_LTU 0x3
#define XE_NE  0x9
#define XE_GE  0xA
#define XE_GEU 0xB

/* bcc s, t, <target later> — "37 12 01" = beq a2, a3, +5.
 * Returns a fixup pointer (write view) for xe_patch_bcc. */
static inline u8 *xe_bcc(xemit_t *e, u32 cond, u32 s, u32 t)
{
  u8 *fix = e->ptr;
  xe_byte3(e, (u8)((t << 4) | 0x07), (u8)((cond << 4) | s), 0x00);
  return fix;
}

static inline void xe_patch_bcc(u8 *fix, const u8 *target)
{
  s32 off = (s32)(target - (fix + 4)); // imm8 = target - (pc + 4)
  if (off < -128 || off > 127) xe_bcc_overflow = 1;  // out of range → reject block
  fix[2] = (u8)off;
}

/* j <target later> — "46 d6 ff"; imm18 = target - pc - 4 */
static inline u8 *xe_j(xemit_t *e)
{
  u8 *fix = e->ptr;
  xe_byte3(e, 0x06, 0x00, 0x00);
  return fix;
}

static inline void xe_patch_j(u8 *fix, const u8 *target)
{
  s32 imm18 = (s32)(target - (fix + 4));
  fix[0] = (u8)(0x06 | ((imm18 & 0x3) << 6));
  fix[1] = (u8)((imm18 >> 2) & 0xFF);
  fix[2] = (u8)((imm18 >> 10) & 0xFF);
}

/* l32r t, <literal slot> — "21 d4 ff"; word offset backwards from
 * ((pc+3) & ~3), computed in exec-view addresses. */
static inline void xe_l32r(xemit_t *e, u32 t, int lit_index)
{
  u32 pc_exec = (u32)(uintptr_t)(e->ptr + e->exec_delta);
  u32 lit_exec = (u32)(uintptr_t)((u8 *)&e->lits[lit_index] + e->exec_delta);
  s32 woff = (s32)(lit_exec - ((pc_exec + 3) & ~3u)) >> 2;
  xe_byte3(e, (u8)((t << 4) | 0x01), (u8)(woff & 0xFF), (u8)((woff >> 8) & 0xFF));
}

/* Convenience: load an arbitrary 32-bit constant (via movi or literal) */
static inline void xe_load_const(xemit_t *e, u32 t, u32 value)
{
  if ((s32)value >= -2048 && (s32)value <= 2047)
    xe_movi(e, t, (s32)value);
  else
    xe_l32r(e, t, xe_lit(e, value));
}

#endif /* XTENSA_EMIT_H */
