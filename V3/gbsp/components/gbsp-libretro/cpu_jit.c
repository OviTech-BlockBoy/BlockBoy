/* Xtensa JIT for gpSP: translator ("JIT-lite").
 *
 * Thumb code in ROM is translated per block into native Xtensa code in
 * dual-mapped executable PSRAM. Blocks are call0 functions invoked by a C
 * dispatcher. Each branch ends a block (block linking below).
 *
 * Register roles in generated code:
 *   a2 = exit code (return)   a3 = &reg[0]
 *   a4 = T0 (+ T1/T2/T3 = a5/a6/a7 without reg-alloc, a14/a15/a8 with)
 *   a8 = callx8 target/T3     a10/a11/a12 = call args   a9/a13 = flag scratch
 * (a0-a7 survive a windowed callx8; a8-a15 don't — flag scratch / T1-T3 are
 *  never used across a call.)
 *
 * The hottest ARM regs can live persistently in a5/a6/a7 (the only regs that
 * survive callx8 and the callx0 load stub and the chain jx); see JIT_REG_ALLOC.
 *
 * ARM registers and NZCV live in memory (reg[] + REG_N/Z/C/V_FLAG slots,
 * gpSP dynarec convention).
 *
 * Rare instructions (incl. SWI, ADC/SBC, shifts-by-register) have no emitter:
 * the block ends with JEXIT_CLASSIC_ONE and the classic interpreter runs that
 * one instruction via the thumb_loop seam (budget=2).
 *
 * Notes: REG_PC is synced before every memory helper (BIOS open-bus); the
 * pop{pc}/push{lr} bit is in opcode bit 8; the cycle pump stays in the dispatcher. */

#include "common.h"
#include "xtensa/xtensa_emit.h"
#include "cpu_jit.h"

u32 jit_enabled = 0;
jit_stats_t jit_stats;

// Seam-globals (cpu.cpp)
extern u32 cached_exit_budget;
extern u32 cached_exit_flag;
extern s32 cached_exit_cycles;

// Cache sync is provided by gbsp/main (esp_cache is ESP-specific)
extern void jit_cache_sync(void *wr_addr, void *ex_addr, u32 len);

// windowed→call0 block bridge (cpu_jit_stub.S). Calls the block via callx0
// (no windowed entry/retw per block) and returns the exit code.
extern u32 jit_call0(void *block_entry);

/* Exit codes of generated blocks (a2) */
#define JEXIT_CONTINUE     0  // reg[REG_PC] set; dispatcher continues
#define JEXIT_MODE         2  // to ARM mode (bx): dispatcher → classic
#define JEXIT_CLASSIC_ONE  3  // uncovered instruction at reg[REG_PC]
#define JEXIT_HALT         4  // HALT alert from a store (REG_PC already set)

typedef u32 (*jit_block_fn)(void);

/* ------------------------------------------------------------------ */
/* Cache: dual-mapped arena + hash                                     */
/* ------------------------------------------------------------------ */

// 14 bits (16384 entries) matches the 2MB cache; at 13 bits the hash filled every
// 1-2s in busy scenes -> full flush -> recompile storm. Keep JIT_HASH_BYTES in sync.
#define JIT_HASH_BITS 14
#define JIT_HASH_SIZE (1u << JIT_HASH_BITS)
#define BLOCK_RESERVE 3072
#define LIT_SLOTS     32

// Verify mode: run pure blocks (no memory helpers / mode switch) twice — JIT and
// interpreter — and compare the full register state. The first mismatch pinpoints
// the bad emitter. Slow; off in production.

#define JF_PURE     0x01
#define JF_REPORTED 0x02

/* Flag elision: per instruction compute only the flags a later instruction still
 * consumes. In this JIT no emitted instruction reads flags (adc/sbc/conditionals →
 * classic = terminators) and the dispatcher writes all 4 NZCV slots back to CPSR on
 * every block exit. So each flag only needs computing by its last writer in the block.
 * 0 = always compute all flags. */
#define JIT_FLAG_ELISION 1

/* sext/extui peephole fusion: a (s16)/(u16) cast is normally JIT'd as 2 shifts;
 * Xtensa does it in 1 (sext/extui). Fuse only if both ops' flags are dead, same Rd,
 * adjacent, 2nd source == 1st dest, imm5==16. 0 = no fusion. */

/* cross-block flag liveness: drop the flag materialisation at a block boundary if the
 * terminator doesn't read the flag and all successors overwrite it before reading
 * (flag dead-out). Compile-time, conservative (doubt = live). 0 = off. */

/* link-inline cache: cache the link target per branch site → avoid the
 * jit_link_lookup hash per boundary. Flush-safe via a generation counter. 0 = hash
 * every time. Requires jit_set_link_cache(PSRAM buf), else falls back to the hash path. */
#define JIT_LINK_CACHE 1

/* Block linking: direct branches jump straight to the next block instead of back to
 * the C dispatcher. Safe w.r.t. cache flush: no patched addresses, each link does a
 * fresh hash lookup (miss → back to the dispatcher). A linked chain stays in one
 * jit_call0 window: the jx skips the whole bridge round-trip. 0 = old per-branch retw. */
#define JIT_BLOCK_LINK 1

/* Generic idle detector: the JIT recognises pure memory-poll loops (backward
 * conditional self-loop, body only loads + low-reg ALU without side effects/counter)
 * and fast-forwards them, even in games without an idle_loop_target_pc. 0 = only the
 * curated idle_loop_target_pc. */
#define JIT_GENERIC_IDLE 1

/* Idle-as-HALT: a detected idle loop sets CPU_HALT instead of just jit_cycles=0. Then
 * update_gba's inner loop (main.c) fast-forwards all scanline events until an IRQ wakes
 * the CPU — 1 call instead of ~577k JIT round-trips/run. Mirrors gpSP's VBlankIntrWait
 * HLE + mGBA's idle skip. Risk: an idle waiting on a non-IRQ flag could soft-stall, but
 * update_gba also returns on frame_complete = visible freeze, not a hard hang.
 * 0 = old per-event jit_cycles=0 fast-forward. */
#define JIT_IDLE_HALT 1

/* Static global register allocation. The NPIN hottest ARM regs live persistently in
 * callx8-surviving Xtensa regs (a5/a6/a7). Background: blocks call C helpers via a
 * windowed callx8 (eg_call, and crucially eg_link → jit_link_lookup at every chain
 * boundary). On call8 the window shifts +8 → the caller's a8-a15 become the callee's
 * a0-a7 and get clobbered; a0-a7 are preserved. Pinning in a12-a15 would be wiped on
 * every link. a4 is also out: the callx0 load stub clobbers it. That leaves a5/a6/a7 —
 * they survive callx8 and the callx0 load stub and carry across the jx into the next
 * chained block (same window).
 *
 * Requires JIT_CALL0_BLOCKS (flat window) + a static map (every block uses the same
 * ARM→Xtensa assignment, else pins don't line up after a jx). Discipline:
 *  - prologue (chain head only, before POST_ENTRY): load pins from reg[];
 *  - eg_ret (every exit to C): flush pins to reg[] (intra-chain jx does not flush);
 *  - around jit_blockmem (reads/writes reg[] directly): explicit flush/reload.
 * eg_get/eg_put are pin-aware → the emitters themselves stay unchanged. 0 = revert. */
#define JIT_REG_ALLOC 1
#define NPIN 3           /* operand pins off → a5 free for nzr (flags) */

/* FLAGS Stage 1 (QEMU NF/ZF model): the N/Z flags live as a RESULT in nzr (a5).
 * N = nzr<0, Z = nzr==0 → both free from the result (instead of ~6 instr to compute).
 * CF/VF stay 0/1 in reg[]. The interpreter uses 0/1 → eager flush: reconstruct in the
 * prologue (chain head), materialise in eg_ret. 0 = old 0/1 NZ. */
#define R_NZR 5          /* a5: the NZ result (free because NPIN=0) */

/* POST_ENTRY = bytes a linked jump skips (the chain-head prologue).
 * windowed: xe_entry(3)+l32r R_BASE(3). call0: l32r(3)+s32i blink(3). Under reg-alloc
 * add NPIN pin loads (l32i, 3 bytes each). LAZY_NZ adds the nzr reconstruct = 18 bytes. */
#if JIT_REG_ALLOC
#define POST_ENTRY (6 + 3 * NPIN)
#else
#define POST_ENTRY 6
#endif

/* Hot-block IRAM pinning: copy a block that ran PIN_THRESHOLD times into executable
 * internal SRAM (no I-cache → no PSRAM fetch stalls). A byte copy suffices: l32r is
 * PC-relative and internal branches are relative, so a 16-aligned copy stays correct
 * (only ISYNC needed). */
#define JIT_IRAM_PIN   1
#define PIN_THRESHOLD  64

/* call0 block ABI. Blocks are call0 functions (no windowed entry/retw): the
 * dispatcher calls them via the asm bridge jit_call0 (callx0). The block saves its
 * call0 link (a0) at the prologue in reg[OFF_BLINK] and reloads it on every exit
 * before ret.n (callx0 to stubs overwrites a0). Foundation for block linking +
 * register allocation. 0 = old windowed-block behaviour. */
#define JIT_CALL0_BLOCKS 1
#define OFF_BLINK (38 * 4)   /* reg[38]: backend-private, the block's call0 link */

#define FN 1u
#define FZ 2u
#define FC 4u
#define FV 8u
#define MAXB 48      /* == max_instr; bound for the pre-scan arrays */

typedef struct {
  u32 pc; u32 offset; u16 n_instr; u16 flags;
  u16 wlen; u16 _pad;     // block byte length (block_base..end) for the IRAM copy
  u32 runcount;           // hotness (linking off → dispatcher counts per block)
  u32 iram_entry;         // 0 = not pinned; else exec address of the IRAM copy
} jhash_t;

static u8 *jit_wr;
static u8 *jit_ex;
static u32 jit_size;
static u32 jit_used;
static jhash_t *jit_hash;
static u32 jit_hash_used;

static u8 *jit_iram;          // executable internal SRAM for hot blocks (or NULL)
static u32 jit_iram_size;
static u32 jit_iram_used;

void jit_set_iram(void *iram_buf, u32 iram_size)
{
  jit_iram = (u8 *)iram_buf;
  jit_iram_size = iram_size;
  jit_iram_used = 0;
}

/* link-inline cache: eg_link does a callx8 jit_link_lookup (hash) on every
 * inter-block branch. Cache the looked-up post-entry address per branch site → fast
 * path = a couple of l32i + jx (no callx8/hash). Flush-safe via a generation counter:
 * every flush does code_gen++, a slot is valid only if slot.gen == code_gen. A stale
 * slot → slow path. The array lives in PSRAM data (coherent via d-cache). NULL/overflow
 * → fall back to the old path. */
typedef struct { u32 gen; void *addr; } jlc_t;
static jlc_t *jit_link_cache;       /* PSRAM array, set via jit_set_link_cache */
static u32 jit_link_cache_n;        /* number of slots */
static u32 jit_link_slot_next;      /* bump allocator, reset on flush */
static u32 jit_link_slot_lim;       /* half-flush: slots are partitioned per arena half
                                     * (half 0 = [0,n/2), half 1 = [n/2,n)) so the range
                                     * of a wiped half becomes reusable automatically */
static u32 jit_code_gen = 1;        /* +1 on every flush; slot valid if ==slot.gen */

void jit_set_link_cache(void *buf, u32 nslots)
{
  jit_link_cache = (jlc_t *)buf;
  jit_link_cache_n = buf ? nslots : 0;
  jit_link_slot_next = 0;
  jit_link_slot_lim = jit_link_cache_n / 2;   /* we start in arena half 0 */
}

s32 jit_cycles;
static u32 jit_alert;

static u32 jp_pinned;   // number of blocks pinned in IRAM

/* Purity tracking for verify mode: helpers/mode-switches/classic-exits make a
 * block "impure" (side effects → not double-executable). */
static int tb_pure;
static u32 tb_n;

/* Half-flush support: hash removal uses TOMBSTONES (pc==1, offset stays !=0) so
 * linear-probe chains stay intact; jhash_insert reuses them. */
static u32 jit_hash_tomb;

void jit_invalidate_all(void)
{
  if (!jit_hash)
    return;
  memset(jit_hash, 0, JIT_HASH_SIZE * sizeof(jhash_t));
  jit_used = 64;
  jit_hash_used = 0;
  jit_hash_tomb = 0;
  jit_iram_used = 0;     // pins belong to the wiped hash → re-pin
  jit_code_gen++;        // all link-cache slots invalid (gen mismatch → slow path)
  jit_link_slot_next = 0;
  jit_link_slot_lim = jit_link_cache_n / 2;   /* full wipe = back to arena half 0 */
  jit_stats.flushes++;
}

int jit_init(void *write_view, void *exec_view, u32 size, void *hash_mem)
{
  if (!write_view || !exec_view || !hash_mem)
    return -1;
  jit_wr = (u8 *)write_view;
  jit_ex = (u8 *)exec_view;
  jit_size = size;
  jit_hash = (jhash_t *)hash_mem;
  jit_invalidate_all();
  return 0;
}

static inline u32 jhash_pc(u32 pc)
{
  return ((pc >> 1) * 2654435761u) >> (32 - JIT_HASH_BITS);
}

static inline jhash_t *jhash_lookup(u32 pc)
{
  u32 idx = jhash_pc(pc);
  while (jit_hash[idx].offset)
  {
    if (jit_hash[idx].pc == pc)
      return &jit_hash[idx];
    idx = (idx + 1) & (JIT_HASH_SIZE - 1);
  }
  return NULL;
}

static jhash_t *jhash_insert(u32 pc, u32 offset, u16 n_instr, u16 flags, u16 wlen)
{
  u32 idx = jhash_pc(pc);
  /* The first tombstone in the chain may be reused — pc is guaranteed absent here
   * (insert always follows a failed lookup). */
  while (jit_hash[idx].offset)
  {
    if (jit_hash[idx].pc == 1u) { jit_hash_tomb--; goto place; }
    idx = (idx + 1) & (JIT_HASH_SIZE - 1);
  }
  jit_hash_used++;
place:
  jit_hash[idx].pc = pc;
  jit_hash[idx].offset = offset;
  jit_hash[idx].n_instr = n_instr;
  jit_hash[idx].flags = flags;
  jit_hash[idx].wlen = wlen;
  jit_hash[idx].runcount = 0;
  jit_hash[idx].iram_entry = 0;
  return &jit_hash[idx];
}

/* ------------------------------------------------------------------ */
/* C helpers, called from generated code (callx8).                     */
/* REG_PC is already synced by the block. Cycle costs mirror            */
/* fast_read/write_memory (cpu.cpp).                                    */
/* ------------------------------------------------------------------ */

#define JALIGN8  0xF0000000u
#define JALIGN16 0xF0000001u
#define JALIGN32 0xF0000003u

#define JIT_READ(size, type, readfn)                                          \
{                                                                             \
  u8 *map;                                                                    \
  if (addr < 0x10000000)                                                      \
    jit_cycles -= ws_cyc_nseq[addr >> 24][(size - 8) / 16];                   \
  if (((addr >> 24) == 0) || (addr & JALIGN##size) ||                         \
      !(map = memory_map_read[addr >> 15]))                                   \
    return (u32)(type)(readfn)(addr);                                         \
  return (u32)(type)readaddress##size(map, (addr & 0x7FFF));                  \
}

u32 jit_ld32(u32 addr)  JIT_READ(32, u32, read_memory32)
u32 jit_ld16(u32 addr)  JIT_READ(16, u32, read_memory16)
u32 jit_ld8(u32 addr)   JIT_READ(8, u8, read_memory8)
// LDRSH: type MUST be s16 (not s32) — the fast path casts readaddress16()
// (unsigned u16) to `type`; only s16 sign-extends a negative halfword.
// With s32 the fast path zero-extended → thumb.gba test 210 (LDRSH neg) failed.
u32 jit_lds16(u32 addr) JIT_READ(16, s16, read_memory16_signed)
u32 jit_lds8(u32 addr)  JIT_READ(8, s8, read_memory8)

/* Stores: return 0 = continue, 1 = HALT (REG_PC = next_pc already set) */
#define JIT_WRITE(size, writefn)                                              \
{                                                                             \
  u32 a = addr & ~(JALIGN##size & 0x03);                                      \
  if (a < 0x10000000)                                                         \
    jit_cycles -= ws_cyc_nseq[a >> 24][(size - 8) / 16];                      \
  jit_alert |= writefn(a, val);                                               \
  if (jit_alert & CPU_ALERT_HALT)                                             \
  {                                                                           \
    reg[REG_PC] = next_pc;                                                    \
    return 1;                                                                 \
  }                                                                           \
  return 0;                                                                   \
}

u32 jit_st32(u32 addr, u32 val, u32 next_pc) JIT_WRITE(32, write_memory32)
u32 jit_st16(u32 addr, u32 val, u32 next_pc) JIT_WRITE(16, write_memory16)
u32 jit_st8(u32 addr, u32 val, u32 next_pc)  JIT_WRITE(8, write_memory8)

/* push/pop/stmia/ldmia — semantics 1:1 with exec_thumb_block_mem (cpu.cpp).
 * packed: rlist(9b, bit8=LR/PC) | rb<<12 | mode<<16 (0=stm 1=ldm 2=push 3=pop)
 * Return: 0=continue, 1=pop-pc (REG_PC set, exit CONTINUE), 5=HALT (PC set).
 * bit 8 counts — pop{pc}/push{lr} are NOT an empty rlist. */
u32 jit_blockmem(u32 packed, u32 next_pc)
{
  u32 rlist = packed & 0x1FF;
  u32 rb = (packed >> 12) & 0xF;
  u32 mode = (packed >> 16) & 3;
  u32 base = reg[rb];
  u32 numops = 0, i;
  int is_load = (mode & 1);
  int predec = (mode == 2);
  u32 ret = 0;

  for (i = 0; i < 9; i++)
    if ((rlist >> i) & 1) numops++;

  s32 addr_off = predec ? -4 : 4;
  u32 endaddr = base + addr_off * numops;
  u32 address = (predec ? endaddr : base) & ~3u;
  u32 wrbck_base = (rlist >> rb) & 1;
  u32 base_first = ((((1u << rb) - 1) & rlist) == 0);
  u32 writeback_first = is_load || !(wrbck_base && base_first);

  if (writeback_first)
    reg[rb] = endaddr;

  for (i = 0; i < 9; i++)
  {
    if (!((rlist >> i) & 1))
      continue;
    if (is_load)
    {
      u32 d;
      if (address < 0x10000000)
      {
        u8 *map = memory_map_read[address >> 15];
        jit_cycles -= ws_cyc_seq[address >> 24][1];
        d = map ? readaddress32(map, address & 0x7FFF) : read_memory32(address);
      }
      else
        d = read_memory32(address);
      if (i == 8) // pop pc
      {
        reg[REG_PC] = d & ~1u;
        if (reg[REG_PC] == idle_loop_target_pc && jit_cycles > 0)
          jit_cycles = 0;
        ret = 1;
      }
      else
        reg[i] = d;
    }
    else
    {
      u32 v = (i == 8) ? reg[REG_LR] : reg[i];
      if (address < 0x10000000)
        jit_cycles -= ws_cyc_seq[address >> 24][1];
      jit_alert |= write_memory32(address, v);
    }
    address += 4;
  }

  if (!writeback_first)
    reg[rb] = endaddr;

  if (!ret && (jit_alert & CPU_ALERT_HALT))
  {
    reg[REG_PC] = next_pc;
    ret = 5;
  }
  return ret;
}

/* ------------------------------------------------------------------ */
/* Emit-bouwstenen                                                     */
/* ------------------------------------------------------------------ */

#define OFF_REG(n)  ((n) * 4)
#define OFF_PC      (REG_PC * 4)
#define OFF_CPSR    (REG_CPSR * 4)
#define OFF_NF      (REG_N_FLAG * 4)
#define OFF_ZF      (REG_Z_FLAG * 4)
#define OFF_CF      (REG_C_FLAG * 4)
#define OFF_VF      (REG_V_FLAG * 4)

#define R_EXIT 2
#define R_BASE 3
#define R_T0   4   // operand A / result carrier to reg[] (clobbers load-stub: ok, dead after load)
#if JIT_REG_ALLOC
/* a5/a6/a7 are pin regs → T1/T2/T3 move. T1/T2 → a14/a15 (unused). T3 shares a8
 * with R_FN: never live at the same time (T3 is always consumed before a call, and
 * no emitter keeps T3 across a callx8/callx0). T1/T2/T3 are never used across a call,
 * so the a8-a15 clobber doesn't touch them. */
#define R_T1   14
#define R_T2   15
#define R_T3   8
#else
#define R_T1   5   // operand B
#define R_T2   6   // result (for 3-operand flag forms)
#define R_T3   7   // extra
#endif
#define R_FN   8
#define R_S0   9   // flag scratch (not call-surviving; never across a call)
#define R_A0   10
#define R_A1   11
#define R_A2   12
#define R_S1   13

#if JIT_REG_ALLOC
/* Static global ARM→Xtensa pin map. The NPIN hottest ARM regs (r0/r1/r2 = Thumb
 * workhorses; pin only "clean" GP regs, not SP/LR, which jit_blockmem reads as base).
 * pin_xt = a5/a6/a7 (survive callx8 + callx0 load stub + the jx). pin_of gives the
 * Xtensa pin of an ARM reg, or -1 (memory-backed). */
static const s8 pin_arm[NPIN] = { 0, 1, 2 };
static const u8 pin_xt[NPIN]  = { 5, 6, 7 };
static inline int pin_of(u32 armreg)
{
  for (int i = 0; i < NPIN; i++) if (pin_arm[i] == (s8)armreg) return pin_xt[i];
  return -1;
}
#else
static inline int pin_of(u32 armreg) { (void)armreg; return -1; }
#endif

/* Read/write an ARM reg. Pinned → register move (a5/a6/a7 is the canonical home,
 * t the working copy); else load/store to reg[]. Pin-awareness is centralised here
 * → the emitters below need not know. */
static inline void eg_get(xemit_t *e, u32 t, u32 armreg)
{
  int p = pin_of(armreg);
  if (p >= 0) { if ((u32)p != t) xe_mov(e, t, (u32)p); }
  else xe_l32i(e, t, R_BASE, OFF_REG(armreg));
}
static inline void eg_put(xemit_t *e, u32 t, u32 armreg)
{
  int p = pin_of(armreg);
  if (p >= 0) { if ((u32)p != t) xe_mov(e, (u32)p, t); }
  else xe_s32i(e, t, R_BASE, OFF_REG(armreg));
}

/* Direct-operand reg-alloc. The pin reg becomes the direct source/dest in the ALU
 * emitter instead of via a mov working copy. eg_src: gives the Xtensa reg holding
 * armreg's value (pinned = the pin, no emit; else load into `scratch`). eg_dst: gives
 * the destination reg (pinned = the pin; else `scratch`). eg_dst_put: commit (store
 * only if not pinned). Safe only for NZ-flag forms; CV forms read fixed T0/T1/T2. */
static inline u32 eg_src(xemit_t *e, u32 armreg, u32 scratch)
{
  int p = pin_of(armreg);
  if (p >= 0) return (u32)p;
  xe_l32i(e, scratch, R_BASE, OFF_REG(armreg));
  return scratch;
}
static inline u32 eg_dst(u32 armreg, u32 scratch)
{
  int p = pin_of(armreg);
  return p >= 0 ? (u32)p : scratch;
}
static inline void eg_dst_put(xemit_t *e, u32 armreg, u32 reg)
{
  if (pin_of(armreg) < 0) xe_s32i(e, reg, R_BASE, OFF_REG(armreg));
}

/* srli for sa 0..31 (xtensa srli max 15) — clobbers nothing extra.
 * Bugfix: sa>30 gave `srli r,r,16` → 16 wraps in the 4-bit field to 0 → wrong.
 * Now split into steps of ≤15 (e.g. sa=31 → 15+15+1). */
static void eg_srli32(xemit_t *e, u32 r, u32 src, u32 sa)
{
  if (sa == 0) { if (r != src) xe_mov(e, r, src); return; }
  u32 first = sa < 15 ? sa : 15;
  xe_srli(e, r, src, first);
  sa -= first;
  while (sa)
  {
    u32 step = sa < 15 ? sa : 15;
    xe_srli(e, r, r, step);
    sa -= step;
  }
}

/* N and Z from result res (clobbers R_S0/R_S1). gen = which flags to emit. */
static void eg_flags_nz(xemit_t *e, u32 res, u32 gen)
{
  if (gen & FN)
  {
    xe_extui(e, R_S0, res, 31, 1);           // N = bit31 as 0/1 (1 instr instead of srai+movi+and)
    xe_s32i(e, R_S0, R_BASE, OFF_NF);
  }
  if (gen & FZ)
  {
    xe_movi(e, R_S0, 0);
    u8 *fx = xe_bcc(e, XE_NE, res, R_S0);    // res != 0 → branch (Z stays 0)
    xe_movi(e, R_S0, 1);                     // not branching = res==0 → Z=1
    xe_patch_bcc(fx, e->ptr);
    xe_s32i(e, R_S0, R_BASE, OFF_ZF);
  }
}

/* C/V for addition: res=T2, sa=T0, sb=T1 (all three still live!).
 * C = (res < sb) unsigned; V = (~(sa^sb) & (sa^res)) >> 31. gen = C/V selection. */
static void eg_flags_add_cv(xemit_t *e, u32 gen)
{
  if (gen & FC)
  {
    xe_movi(e, R_S0, 1);
    u8 *fx = xe_bcc(e, XE_LTU, R_T2, R_T1);  // res < sb → C=1 (stays)
    xe_movi(e, R_S0, 0);
    xe_patch_bcc(fx, e->ptr);
    xe_s32i(e, R_S0, R_BASE, OFF_CF);
  }
  if (gen & FV)
  {
    xe_xor(e, R_S0, R_T0, R_T1);
    xe_movi(e, R_S1, -1);
    xe_xor(e, R_S0, R_S0, R_S1);             // ~(sa^sb)
    xe_xor(e, R_S1, R_T0, R_T2);             // sa^res
    xe_and(e, R_S0, R_S0, R_S1);
    xe_extui(e, R_S0, R_S0, 31, 1);          // V = bit31 as 0/1
    xe_s32i(e, R_S0, R_BASE, OFF_VF);
  }
}

/* C/V for subtraction (carry=1): res=T2, sa=T0, sb=T1.
 * C = (sb <= sa); V = ((sa^sb) & (~sb^res)) >> 31. gen = C/V selection. */
static void eg_flags_sub_cv(xemit_t *e, u32 gen)
{
  if (gen & FC)
  {
    xe_movi(e, R_S0, 1);
    u8 *fx = xe_bcc(e, XE_GEU, R_T0, R_T1);  // sa >= sb → C=1 (stays)
    xe_movi(e, R_S0, 0);
    xe_patch_bcc(fx, e->ptr);
    xe_s32i(e, R_S0, R_BASE, OFF_CF);
  }
  if (gen & FV)
  {
    xe_xor(e, R_S0, R_T0, R_T1);             // sa^sb
    xe_movi(e, R_S1, -1);
    xe_xor(e, R_S1, R_T1, R_S1);             // ~sb
    xe_xor(e, R_S1, R_S1, R_T2);             // ~sb^res
    xe_and(e, R_S0, R_S0, R_S1);
    xe_extui(e, R_S0, R_S0, 31, 1);          // V = bit31 as 0/1
    xe_s32i(e, R_S0, R_BASE, OFF_VF);
  }
}

/* cycles -= cyc (constant) — clobbers R_S0/R_S1 and for large cyc also R_T3 */
static void eg_cycles(xemit_t *e, s32 cyc)
{
  if (!cyc)
    return;
  xe_l32r(e, R_S0, xe_lit(e, (u32)(uintptr_t)&jit_cycles));
  xe_l32i(e, R_S1, R_S0, 0);
  if (cyc >= -128 && cyc <= 128)
    xe_addi(e, R_S1, R_S1, -cyc);
  else
  {
    xe_load_const(e, R_T3, (u32)cyc);
    xe_sub(e, R_S1, R_S1, R_T3);
  }
  xe_s32i(e, R_S1, R_S0, 0);
}

/* Block prologue: set R_BASE = &reg[0]. call0 mode also saves the call0 link (a0)
 * in reg[OFF_BLINK]. POST_ENTRY tracks the prologue length: windowed = entry(3)+
 * l32r(3); call0 = l32r(3)+s32i(3); + NPIN pin loads under reg-alloc. A linked jump
 * lands at POST_ENTRY = after the prologue, so pins are loaded only by the chain head. */
static void eg_prologue(xemit_t *e)
{
#if JIT_CALL0_BLOCKS
  xe_l32r(e, R_BASE, xe_lit(e, (u32)(uintptr_t)&reg[0]));
  xe_s32i(e, 0 /*a0*/, R_BASE, OFF_BLINK);   // stash the call0 link
#if JIT_REG_ALLOC
  // Load pins — only the chain head runs this; linked blocks enter at POST_ENTRY
  // (below) with the pins already live from the chain. Counts toward POST_ENTRY.
  for (int i = 0; i < NPIN; i++)
    xe_l32i(e, pin_xt[i], R_BASE, OFF_REG(pin_arm[i]));
#endif
#else
  xe_entry(e, 64);
  xe_l32r(e, R_BASE, xe_lit(e, (u32)(uintptr_t)&reg[0]));
#endif
}

/* Block return. call0: reload the link from reg[OFF_BLINK] (callx0 to stubs
 * overwrote a0) and ret.n. windowed: retw.n. */
static inline void eg_ret(xemit_t *e)
{
#if JIT_CALL0_BLOCKS
#if JIT_REG_ALLOC
  // Flush pins to reg[] before returning to C (interpreter/dispatcher/IRQ read
  // reg[]). Only this C-exit path flushes; the intra-chain jx (eg_link hit) does
  // NOT pass here → pins stay live across the chain.
  for (int i = 0; i < NPIN; i++)
    xe_s32i(e, pin_xt[i], R_BASE, OFF_REG(pin_arm[i]));
#endif
  xe_l32i(e, 0 /*a0*/, R_BASE, OFF_BLINK);
  xe_ret(e);
#else
  xe_retw(e);
#endif
}

/* Exit met constant PC */
static void eg_exit_pc(xemit_t *e, u32 pc_const, s32 cyc, u32 code)
{
  if (code != JEXIT_CONTINUE)
    tb_pure = 0;
  xe_load_const(e, R_T0, pc_const);
  xe_s32i(e, R_T0, R_BASE, OFF_PC);
  eg_cycles(e, cyc);
  xe_movi(e, R_EXIT, (s32)code);
  eg_ret(e);
}

/* Exit with PC in register vr (vr must not be R_T3/R_S0/R_S1) */
static void eg_exit_pcreg(xemit_t *e, u32 vr, s32 cyc, u32 code)
{
  tb_pure = 0; // bx/register target: not verifiable (PC/mode dynamic)
  xe_s32i(e, vr, R_BASE, OFF_PC);
  eg_cycles(e, cyc);
  xe_movi(e, R_EXIT, (s32)code);
  eg_ret(e);
}

static inline void eg_sync_pc(xemit_t *e, u32 pc)
{
  xe_load_const(e, R_T3, pc);
  xe_s32i(e, R_T3, R_BASE, OFF_PC);
}

/* Idle-loop fast-forward: the conditional branch is at idle_loop_target_pc and jumps
 * backward to itself (busy-wait on an IRQ flag). Instead of spinning in-window:
 * reg[PC]=tgt (loop start), jit_cycles=0 (budget spent → the dispatcher services the
 * pending events/IRQ that set the flag), exit. Mirrors the interpreter
 * (PC==idle_loop_target_pc → cycles_remaining=0). The loop now runs 1×/slice. */
static void eg_idle_exit(xemit_t *e, u32 tgt)
{
  tb_pure = 0;
  xe_load_const(e, R_T0, tgt);
  xe_s32i(e, R_T0, R_BASE, OFF_PC);
#if JIT_IDLE_HALT
  /* CPU_HALT → update_gba fast-forwards to the wake IRQ (no per-event round-trip). */
  xe_movi(e, R_T0, CPU_HALT);
  xe_s32i(e, R_T0, R_BASE, OFF_REG(CPU_HALT_STATE));
#endif
  xe_l32r(e, R_S0, xe_lit(e, (u32)(uintptr_t)&jit_cycles));
  xe_movi(e, R_T3, 0);
  xe_s32i(e, R_T3, R_S0, 0);               // jit_cycles = 0 (slice done)
  xe_movi(e, R_EXIT, JEXIT_CONTINUE);
  eg_ret(e);                                // flushes pins + ret
}

static inline void eg_call(xemit_t *e, void *fn)
{
  tb_pure = 0;
  xe_l32r(e, R_FN, xe_lit(e, (u32)(uintptr_t)fn));
  xe_callx8(e, R_FN);
}

/* After a store helper: a10 != 0 → HALT exit (REG_PC already set by the helper) */
static void eg_store_alert_check(xemit_t *e)
{
  xe_movi(e, R_T3, 0);
  u8 *fx = xe_bcc(e, XE_EQ, R_A0, R_T3);
  xe_movi(e, R_EXIT, JEXIT_HALT);
  eg_ret(e);
  xe_patch_bcc(fx, e->ptr);
}

/* ------------------------------------------------------------------ */
/* Translator                                                          */
/* ------------------------------------------------------------------ */

static u8 *fetch_map(u32 pc)
{
  u32 region = pc >> 15;
  u8 *map = memory_map_read[region];
  touch_gamepak_page(region);
  if (!map)
    map = load_gamepak_page(region & 0x3FF);
  return map;
}

static inline u16 jfetch16(u32 pc)
{ return readaddress16(fetch_map(pc), pc & 0x7FFF); }
static inline u32 jfetch32(u32 a)
{ return readaddress32(fetch_map(a), a & 0x7FFF); }

/* N as 0/1 in dst. LAZY_NZ → from nzr (bit31); else from reg[]. */
static inline void eg_load_n(xemit_t *e, u32 dst)
{
  xe_l32i(e, dst, R_BASE, OFF_NF);
}
/* Z as 0/1 in dst (tmp != dst, gets clobbered). LAZY_NZ → (nzr==0); else from reg[]. */
static inline void eg_load_z(xemit_t *e, u32 dst, u32 tmp)
{
  (void)tmp; xe_l32i(e, dst, R_BASE, OFF_ZF);
}

/* Condition → R_T0 = 1/0. N/Z via eg_load_n/eg_load_z (nzr); C/V from reg[] (0/1). */
static void eg_eval_cond(xemit_t *e, u32 cond)
{
  switch (cond)
  {
    case 0x0: eg_load_z(e, R_T0, R_T1); break;                              // eq: Z
    case 0x1: eg_load_z(e, R_T1, R_T2);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T0, R_T0, R_T1); break;      // ne: !Z
    case 0x2: xe_l32i(e, R_T0, R_BASE, OFF_CF); break;                      // cs: C
    case 0x3: xe_l32i(e, R_T1, R_BASE, OFF_CF);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T0, R_T0, R_T1); break;      // cc: !C
    case 0x4: eg_load_n(e, R_T0); break;                                    // mi: N
    case 0x5: eg_load_n(e, R_T1);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T0, R_T0, R_T1); break;      // pl: !N
    case 0x6: xe_l32i(e, R_T0, R_BASE, OFF_VF); break;                      // vs: V
    case 0x7: xe_l32i(e, R_T1, R_BASE, OFF_VF);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T0, R_T0, R_T1); break;      // vc: !V
    case 0x8: xe_l32i(e, R_T1, R_BASE, OFF_CF);                             // hi: C && !Z
              eg_load_z(e, R_T2, R_T3);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T2, R_T0, R_T2);             // !Z
              xe_and(e, R_T0, R_T1, R_T2); break;
    case 0x9: xe_l32i(e, R_T1, R_BASE, OFF_CF);                             // ls: !C || Z
              eg_load_z(e, R_T2, R_T3);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T1, R_T0, R_T1);             // !C
              xe_or(e, R_T0, R_T1, R_T2); break;
    case 0xA: eg_load_n(e, R_T1);                                          // ge: N==V
              xe_l32i(e, R_T2, R_BASE, OFF_VF);
              xe_xor(e, R_T1, R_T1, R_T2);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T0, R_T0, R_T1); break;
    case 0xB: eg_load_n(e, R_T1);                                          // lt: N!=V
              xe_l32i(e, R_T2, R_BASE, OFF_VF);
              xe_xor(e, R_T0, R_T1, R_T2); break;
    case 0xC: eg_load_n(e, R_T1);                                          // gt: !Z && N==V
              xe_l32i(e, R_T2, R_BASE, OFF_VF);
              xe_xor(e, R_T1, R_T1, R_T2);
              eg_load_z(e, R_T2, R_T3);
              xe_or(e, R_T1, R_T1, R_T2);
              xe_movi(e, R_T0, 1); xe_sub(e, R_T0, R_T0, R_T1); break;
    default:  eg_load_n(e, R_T1);                                          // le: Z || N!=V
              xe_l32i(e, R_T2, R_BASE, OFF_VF);
              xe_xor(e, R_T1, R_T1, R_T2);
              eg_load_z(e, R_T2, R_T3);
              xe_or(e, R_T0, R_T1, R_T2); break;
  }
}

/* Condition fusion: emit a branch-to-TAKEN-if-condition-true, returns the fixup.
 * Simple conditions branch directly on nzr/CF/VF (no 0/1 materialisation):
 * EQ/NE/MI/PL = 1 compare instead of eg_eval_cond+0/1+bcc. Combined → 0/1 + bcc. */
static u8 *eg_cond_branch(xemit_t *e, u32 cond)
{
  eg_eval_cond(e, cond);
  xe_movi(e, R_T3, 0);
  return xe_bcc(e, XE_NE, R_T0, R_T3);
}

/* Mem fast-path stubs (cpu_jit_stub.S). Own call0 convention:
 * loads a10=addr,a11=pc -> a10; clobbers a0/a4/a8/a9/a10, reads a3(&reg[0]). */
extern u32 jit_load_u32(u32 addr, u32 pc);
extern u32 jit_load_u16(u32 addr, u32 pc);
extern u32 jit_load_u8 (u32 addr, u32 pc);
extern u32 jit_load_s8 (u32 addr, u32 pc);
extern u32 jit_load_s16(u32 addr, u32 pc);

/* Memory load via a stub (callx0 = no window rotation, instead of the windowed
 * callx8 to the C helper — that was the Pokémon bottleneck). addr is already in
 * R_A0. R_A1=pc only serves the slow path (BIOS open-bus); the stub sets REG_PC
 * there, so no eg_sync_pc on the fast path. The block saves its windowed retw link
 * (a0) in R_T3 around the call: a0 is overwritten by callx0 and a8-a15 wouldn't
 * survive the slow-path callx8, but a4-a7 would. */
static void eg_mem_load(xemit_t *e, void *stub, u32 pc, u32 rd)
{
  tb_pure = 0;
  xe_load_const(e, R_A1, pc);             // a11 = pc (slow-path REG_PC)
#if !JIT_REG_ALLOC
  xe_mov(e, R_T3, 0);                     // a7 = a0  (save windowed retw link)
#endif
  xe_l32r(e, R_FN, xe_lit(e, (u32)(uintptr_t)stub));
  xe_callx0(e, R_FN);                     // a10 = stub(a10, a11)
#if !JIT_REG_ALLOC
  xe_mov(e, 0, R_T3);                     // a0 = a7  (restore retw link)
#else
  // Under reg-alloc R_T3 = a8 (clobbered by the stub) and saving a0 is unnecessary:
  // in call0 mode eg_ret reloads a0 from reg[OFF_BLINK]. callx0 clobbers a0 freely;
  // pins (a5/a6/a7) survive the stub.
#endif
  eg_put(e, R_A0, rd);
}

static void eg_mem_store(xemit_t *e, void *helper, u32 pc, u32 rd)
{
  eg_sync_pc(e, pc);                    // leaves R_T3 = pc
  eg_get(e, R_A1, rd);                  // value
  xe_addi(e, R_A2, R_T3, 2);            // next_pc = pc+2 (no extra literal)
  eg_call(e, helper);
  eg_store_alert_check(e);
}

/* Address in R_A0 = reg[rb] + reg[ro] or reg[rb] + imm */
static void eg_addr_rr(xemit_t *e, u32 rb, u32 ro)
{
  eg_get(e, R_T0, rb);
  eg_get(e, R_T1, ro);
  xe_add(e, R_A0, R_T0, R_T1);
}

static void eg_addr_ri(xemit_t *e, u32 rb, u32 imm)
{
  eg_get(e, R_T0, rb);
  if (imm == 0)
    xe_mov(e, R_A0, R_T0);
  else
    xe_addi(e, R_A0, R_T0, (s32)imm);   // imm ≤ 124 → fits in addi
}

/* Block linking: give the post-entry exec address of pc's block, or 0 if it is not
 * (yet) translated. No translate here — that could flush the cache while we're in a
 * running block; on 0 the branch falls back to the dispatcher, which safely
 * (re)translates outside any block. */
static void *jit_link_lookup(u32 pc)
{
  jhash_t *blk = jhash_lookup(pc);
  return blk ? (jit_ex + blk->offset + POST_ENTRY) : (void *)0;
}

/* Direct-branch link to a constant target. First subtracts the branch cycles; on
 * spent budget (jit_cycles<=0) back to the dispatcher so update_gba pumps. Otherwise
 * look up the post-entry address and jx straight there; miss → dispatcher. Flags stay
 * in the slots (no CPSR sync within the chain). */
static void eg_link(xemit_t *e, u32 target, s32 cyc)
{
  tb_pure = 0;
#if JIT_BLOCK_LINK
  eg_cycles(e, cyc);
  xe_l32r(e, R_S0, xe_lit(e, (u32)(uintptr_t)&jit_cycles));
  xe_l32i(e, R_S1, R_S0, 0);
  xe_movi(e, R_T3, 1);
  u8 *fx_exit = xe_bcc(e, XE_LT, R_S1, R_T3);   // jit_cycles < 1 (<=0) → exit
#if JIT_LINK_CACHE
  /* Fast path: cached target with generation check (flush-safe). */
  int cslot = -1;
  if (jit_link_cache && jit_link_slot_next < jit_link_slot_lim)
    cslot = (int)jit_link_slot_next++;
  if (cslot >= 0)
  {
    u32 sa = (u32)(uintptr_t)&jit_link_cache[cslot];
    xe_l32r(e, R_S0, xe_lit(e, sa));             // R_S0 = &slot
    xe_l32i(e, R_A0, R_S0, 0);                    // R_A0 = slot.gen
    xe_l32r(e, R_S1, xe_lit(e, (u32)(uintptr_t)&jit_code_gen));
    xe_l32i(e, R_S1, R_S1, 0);                    // R_S1 = code_gen
    u8 *fx_slow = xe_bcc(e, XE_NE, R_A0, R_S1);   // gen != code_gen → slow path
    xe_l32i(e, R_A0, R_S0, 4);                    // R_A0 = slot.addr (filled on hit only → valid)
    xe_jx(e, R_A0);                               // CACHE HIT → jump (no callx8/hash)
    xe_patch_bcc(fx_slow, e->ptr);                // slow path starts here
  }
#endif
  xe_load_const(e, R_A0, target);
  xe_l32r(e, R_FN, xe_lit(e, (u32)(uintptr_t)jit_link_lookup));
  xe_callx8(e, R_FN);                           // R_A0 = post-entry address or 0
  xe_movi(e, R_T3, 0);
  u8 *fx_miss = xe_bcc(e, XE_EQ, R_A0, R_T3);   // not translated → exit (do NOT cache)
#if JIT_LINK_CACHE
  if (cslot >= 0)
  {
    u32 sa = (u32)(uintptr_t)&jit_link_cache[cslot];
    xe_l32r(e, R_S0, xe_lit(e, sa));             // &slot (dedupes with fast-path literal)
    xe_s32i(e, R_A0, R_S0, 4);                   // slot.addr = R_A0 (resolved, !=0)
    xe_l32r(e, R_S1, xe_lit(e, (u32)(uintptr_t)&jit_code_gen));
    xe_l32i(e, R_S1, R_S1, 0);
    xe_s32i(e, R_S1, R_S0, 0);                   // slot.gen = code_gen (now valid)
  }
#endif
  xe_jx(e, R_A0);                               // linked jump (same window)
  xe_patch_bcc(fx_exit, e->ptr);
  xe_patch_bcc(fx_miss, e->ptr);
  xe_load_const(e, R_T0, target);
  xe_s32i(e, R_T0, R_BASE, OFF_PC);
  xe_movi(e, R_EXIT, JEXIT_CONTINUE);
  eg_ret(e);
#else
  eg_exit_pc(e, target, cyc, JEXIT_CONTINUE);
#endif
}

/* Which NZCV flags this Thumb instruction writes (FN/FZ/FC/FV), and whether it ends
 * the block. MUST match the emit switch in translate_block 1:1: every *terminates=1
 * here must be a terminated=1 there (else pre-scan and emit drift apart). Continuers
 * return their flag write mask; terminators 0. */
static u32 thumb_flag_wmask(u16 op, int *terminates)
{
  u32 hi = op >> 8;
  *terminates = 0;
  switch (hi)
  {
    case 0x00 ... 0x07: return ((op >> 6) & 0x1F) ? (FN|FZ|FC) : (FN|FZ); // lsl imm
    case 0x08 ... 0x17: return FN|FZ|FC;                                  // lsr/asr imm
    case 0x18: case 0x19: case 0x1A: case 0x1B:                           // add/sub reg
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: return FN|FZ|FC|FV;       // add/sub imm3
    case 0x20 ... 0x27: return FN|FZ;                                     // mov imm8
    case 0x28 ... 0x3F: return FN|FZ|FC|FV;                               // cmp/add/sub imm8
    case 0x40:
      if (((op >> 6) & 3) <= 1) return FN|FZ;                            // and/eor
      *terminates = 1; return 0;                                         // lsl/lsr by reg
    case 0x41: *terminates = 1; return 0;                                 // asr/adc/sbc/ror reg
    case 0x42: return (((op >> 6) & 3) == 0) ? (FN|FZ) : (FN|FZ|FC|FV);   // tst | neg/cmp/cmn
    case 0x43: return FN|FZ;                                              // orr/mul/bic/mvn
    case 0x44: case 0x46:                                                 // add/mov hireg
    {
      u32 hrd = ((op >> 4) & 8) | (op & 7), hrs = (op >> 3) & 0xF;
      if (hrd == REG_PC || hrs == REG_PC) { *terminates = 1; return 0; }
      return 0;
    }
    case 0x45: case 0x47: *terminates = 1; return 0;                      // cmp hireg | bx
    case 0x48 ... 0x4F: return 0;                                         // ldr pc
    case 0x50 ... 0x9F: return 0;                                         // loads/stores
    case 0xA0 ... 0xB0: return 0;                                         // add pc/sp, add sp
    case 0xB4: case 0xB5: case 0xBC: case 0xC0 ... 0xCF: return 0;        // push/stm/ldm (cont)
    default: *terminates = 1; return 0;                                   // pop pc/bcc/b/bl/swi/...
  }
}


/* Generic idle detector: classifies a Thumb body instruction. Returns the read/
 * written low regs (r0-r7, bitmasks), whether it's a memory LOAD, and whether it's
 * unsafe for an idle-loop body. Whitelist = loads (imm offset) + low-reg ALU that
 * don't accumulate; everything else (stores, add/sub-imm8-to-r8 = counter, hireg,
 * sp/pc-relative, push/pop) → unsafe = don't treat the loop as idle. */
static void thumb_idle_classify(u16 op, u32 *rd, u32 *wr, int *is_load, int *unsafe)
{
  u32 hi = op >> 8, ra = op & 7, rb = (op >> 3) & 7, rc = (op >> 6) & 7, r8 = (op >> 8) & 7;
  *rd = 0; *wr = 0; *is_load = 0; *unsafe = 0;
#define IRD(n) (*rd |= (1u << (n)))
#define IWR(n) (*wr |= (1u << (n)))
  switch (hi)
  {
    case 0x00 ... 0x17: IRD(rb); IWR(ra); break;                 // lsl/lsr/asr imm
    case 0x18: case 0x19: case 0x1A: case 0x1B: IRD(rb); IRD(rc); IWR(ra); break; // add/sub reg
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: IRD(rb); IWR(ra); break;          // add/sub imm3
    case 0x20 ... 0x27: IWR(r8); break;                          // mov imm8
    case 0x28 ... 0x2F: IRD(r8); break;                          // cmp imm8
    case 0x40:                                                   // and/eor (reg-shift = unsafe)
      if (((op >> 6) & 3) <= 1) { IRD(ra); IRD(rb); IWR(ra); } else *unsafe = 1; break;
    case 0x42:                                                   // tst/neg/cmp/cmn
      if (((op >> 6) & 3) == 1) { IRD(rb); IWR(ra); }            // neg
      else { IRD(ra); IRD(rb); } break;                          // tst/cmp/cmn (no write)
    case 0x43:                                                   // orr/mul/bic/mvn
      if (((op >> 6) & 3) == 3) { IRD(rb); IWR(ra); }            // mvn
      else { IRD(ra); IRD(rb); IWR(ra); } break;
    case 0x48 ... 0x4F: IWR(r8); *is_load = 1; break;            // ldr rd,[pc,#imm]
    case 0x68 ... 0x6F: case 0x78 ... 0x7F: case 0x88 ... 0x8F:  // ldr/ldrb/ldrh rd,[rb,#imm]
      IRD(rb); IWR(ra); *is_load = 1; break;
    case 0x98 ... 0x9F: IWR(r8); *is_load = 1; break;            // ldr rd,[sp,#imm]
    default: *unsafe = 1; break;                                 // stores/counter/hireg/push-pop/...
  }
#undef IRD
#undef IWR
}

/* Half-flush: wipe only the OLDEST arena half instead of everything, so the recent
 * (hot) half survives — full flushes caused 60-150ms recompile storms. Safety:
 * (a) wiped hash entries become tombstones (probe chains intact, lookups just miss);
 * (b) jit_code_gen++ invalidates every link-cache slot (same generation mechanism
 * that already covered full flushes) → no jump into wiped code; (c) iram_entry of
 * wiped entries is cleared (survivor pins stay valid; the pin pool is only reused
 * on a FULL flush). Full flush remains as a rare safety net on hash pressure. */
static void jit_half_flush(u32 lo, u32 hi)
{
  for (u32 i = 0; i < JIT_HASH_SIZE; i++)
  {
    if (!jit_hash[i].offset || jit_hash[i].pc == 1u)
      continue;
    if (jit_hash[i].offset >= lo && jit_hash[i].offset < hi)
    {
      jit_hash[i].pc = 1u;
      jit_hash[i].iram_entry = 0;
      jit_hash_tomb++;
      jit_hash_used--;
    }
  }
  jit_code_gen++;
  jit_stats.half_flushes++;
}

static jhash_t *translate_block(u32 pc)
{
  /* Arena in two halves; when the current half fills up → wipe the other (oldest)
   * half and continue there. Blocks never straddle a half boundary. Safety net:
   * hash too full (live + tombstones) → old-style full flush. */
  {
    u32 need = jit_used + BLOCK_RESERVE + LIT_SLOTS * 4;
    u32 half = jit_size / 2;
    if (jit_hash_used + jit_hash_tomb > (JIT_HASH_SIZE * 7) / 8)
      jit_invalidate_all();
    else if (jit_used < half && need > half)
    {
      jit_half_flush(half, jit_size);
      jit_used = half;
      jit_link_slot_next = jit_link_cache_n / 2;   /* slot range of half 1 */
      jit_link_slot_lim  = jit_link_cache_n;
    }
    else if (need > jit_size)
    {
      jit_half_flush(0, half);
      jit_used = 64;
      jit_link_slot_next = 0;                      /* slot range of half 0 */
      jit_link_slot_lim  = jit_link_cache_n / 2;
    }
  }

  u8 *blk_wr = jit_wr + ((jit_used + 3) & ~3u);
  jit_used = (u32)(blk_wr - jit_wr);

  xemit_t e;
  xe_init(&e, blk_wr, BLOCK_RESERVE + LIT_SLOTS * 4, LIT_SLOTS,
          (s32)(jit_ex - jit_wr));

  u32 p = pc;
  u32 seq = ws_cyc_seq[(pc >> 24) & 0xF][0];
  s32 cyc = 0;
  int terminated = 0;

  tb_pure = 1;
  tb_n = 0;

  // PASS 1: block length + flag liveness. Decode forward to the terminator or MAXB,
  // bounded by a literal estimate so the emit loop never has to truncate mid-block
  // (that would invalidate the last-writer elision). gen[i] = the flags for which
  // instruction i is the last writer = the only one that emits them.
  u32 gen[MAXB];
  int block_n;
  {
    u8 wmask[MAXB];
    u32 sp = pc;
    int nn = 0;
    u32 est_lits = 2;                   // R_BASE + jit_cycles (block-global)
    while (nn < MAXB)
    {
      u16 sop = jfetch16(sp);
      int term;
      wmask[nn] = (u8)thumb_flag_wmask(sop, &term);
      u32 shi = sop >> 8;
      u32 li = (shi >= 0x50 && shi <= 0x9F) ? 2 :   // load/store: pc + stub
               (shi >= 0x48 && shi <= 0x4F) ? 1 :   // ldr pc (folded const)
               term ? 9 : 0;                        // branch: targets+lookup+cache-slot+gen
      if (nn > 0 && est_lits + li + 3 > LIT_SLOTS)  // would overflow the pool → stop
        break;
      est_lits += li;
      nn++;
      if (term) break;
      sp += 2;
    }
    block_n = nn;
#if JIT_FLAG_ELISION
    int lastw[4] = { -1, -1, -1, -1 };
    for (int i = 0; i < nn; i++)
      for (int f = 0; f < 4; f++)
        if (wmask[i] & (1u << f)) lastw[f] = i;
    for (int i = 0; i < nn; i++)
    {
      u32 gg = 0;
      for (int f = 0; f < 4; f++)
        if (lastw[f] == i) gg |= (1u << f);
      gen[i] = gg;
    }
#else
    for (int i = 0; i < nn; i++) gen[i] = FN | FZ | FC | FV;
#endif
  }
  int idx = 0;

  /* Generic idle detector: is this block a pure memory-poll loop? (ends in a
   * conditional branch that jumps backward to block start; body = only
   * loads + low-reg ALU, ≥1 load, no stores/side-effects, no accumulated register).
   * Such a loop can only exit when the polled memory changes → on a single-core GBA
   * only via an event/IRQ → fast-forwarding is correct. Catches idle loops in games
   * without an idle_loop_target_pc automatically. */
  int block_is_idle = 0;
#if JIT_GENERIC_IDLE
  if (block_n >= 2)
  {
    u16 lastop = jfetch16(p + (block_n - 1) * 2);
    u32 lhi = lastop >> 8;
    if (lhi >= 0xD0 && lhi <= 0xDD)                       // ends in a conditional branch
    {
      u32 lpc = p + (block_n - 1) * 2;
      u32 ltgt = lpc + 4 + (u32)(((s32)(s8)(lastop & 0xFF)) * 2);
      if (ltgt == p)                                      // jumps to block start = self-loop
      {
        u32 wmaskr = 0, livein = 0; int has_load = 0, ok = 1;
        for (int i = 0; i < block_n - 1; i++)
        {
          u32 rdm, wrm; int isld, uns;
          thumb_idle_classify(jfetch16(p + i * 2), &rdm, &wrm, &isld, &uns);
          if (uns) { ok = 0; break; }
          livein |= (rdm & ~wmaskr);                      // read before written = cross-iteration
          wmaskr |= wrm;
          if (isld) has_load = 1;
        }
        if (ok && has_load && !(livein & wmaskr))         // no accumulated register
          block_is_idle = 1;
      }
    }
  }
#endif

  eg_prologue(&e);

  while (!terminated && idx < block_n && !e.overflow)
  {
    u16 op = jfetch16(p);
    u32 g = gen[idx];
    u8 hi = op >> 8;
    u32 rd = op & 7, rs = (op >> 3) & 7, rn = (op >> 6) & 7;
    u32 imm5 = (op >> 6) & 0x1F;
    u32 imm8 = op & 0xFF;
    u32 r8 = (op >> 8) & 7;
    cyc += (s32)seq;

    switch (hi)
    {
      /* ---- shifts met immediate ---- */
      case 0x00 ... 0x07: // lsl rd, rs, imm5
      {
        u32 s = eg_src(&e, rs, R_T0), d = eg_dst(rd, R_T0);
        if (imm5)
        {
          if (g & FC)
          {
            xe_extui(&e, R_S0, s, 32 - imm5, 1); // C = bit[32-imm5] (1 instr, before writing d)
            xe_s32i(&e, R_S0, R_BASE, OFF_CF);  // C = last bit shifted out
          }
          xe_slli(&e, d, s, imm5);
        }
        else if (d != s) xe_mov(&e, d, s);       // imm5==0: rd=rs, no C change
        eg_flags_nz(&e, d, g);
        eg_dst_put(&e, rd, d);
        break;
      }

      case 0x08 ... 0x0F: // lsr rd, rs, imm5
      {
        u32 s = eg_src(&e, rs, R_T0), d = eg_dst(rd, R_T0);
        if (imm5 == 0)
        {
          if (g & FC)
          {
            xe_extui(&e, R_S0, s, 31, 1);
            xe_s32i(&e, R_S0, R_BASE, OFF_CF);  // C = bit31
          }
          xe_movi(&e, d, 0);
        }
        else
        {
          if (g & FC)
          {
            xe_extui(&e, R_S0, s, imm5 - 1, 1);
            xe_s32i(&e, R_S0, R_BASE, OFF_CF);
          }
          eg_srli32(&e, d, s, imm5);
        }
        eg_flags_nz(&e, d, g);
        eg_dst_put(&e, rd, d);
        break;
      }

      case 0x10 ... 0x17: // asr rd, rs, imm5
      {
        u32 s = eg_src(&e, rs, R_T0), d = eg_dst(rd, R_T0);
        if (imm5 == 0)
        {
          xe_srai(&e, d, s, 31);                  // result = sign bits
          if (g & FC)
          {
            xe_extui(&e, R_S0, d, 0, 1);          // C = sign-bit (= d&1)
            xe_s32i(&e, R_S0, R_BASE, OFF_CF);
          }
        }
        else
        {
          if (g & FC)
          {
            xe_extui(&e, R_S0, s, imm5 - 1, 1);
            xe_s32i(&e, R_S0, R_BASE, OFF_CF);
          }
          xe_srai(&e, d, s, imm5);
        }
        eg_flags_nz(&e, d, g);
        eg_dst_put(&e, rd, d);
        break;
      }

      /* ---- add/sub 3-operand ---- */
      case 0x18: case 0x19: // add rd, rs, rn
      case 0x1C: case 0x1D: // add rd, rs, imm3
        eg_get(&e, R_T0, rs);
        if (hi <= 0x19) eg_get(&e, R_T1, rn);
        else            xe_movi(&e, R_T1, (s32)rn);
        xe_add(&e, R_T2, R_T0, R_T1);
        eg_flags_add_cv(&e, g);
        eg_flags_nz(&e, R_T2, g);
        eg_put(&e, R_T2, rd);
        break;

      case 0x1A: case 0x1B: // sub rd, rs, rn
      case 0x1E: case 0x1F: // sub rd, rs, imm3
        eg_get(&e, R_T0, rs);
        if (hi <= 0x1B) eg_get(&e, R_T1, rn);
        else            xe_movi(&e, R_T1, (s32)rn);
        xe_sub(&e, R_T2, R_T0, R_T1);
        eg_flags_sub_cv(&e, g);
        eg_flags_nz(&e, R_T2, g);
        eg_put(&e, R_T2, rd);
        break;

      /* ---- imm8-vormen ---- */
      case 0x20 ... 0x27: // mov r8, imm8
      {
        u32 d = eg_dst(r8, R_T0);
        xe_movi(&e, d, (s32)imm8);
        eg_flags_nz(&e, d, g);
        eg_dst_put(&e, r8, d);
        break;
      }

      case 0x28 ... 0x2F: // cmp r8, imm8
        eg_get(&e, R_T0, r8);
        xe_movi(&e, R_T1, (s32)imm8);
        xe_sub(&e, R_T2, R_T0, R_T1);
        eg_flags_sub_cv(&e, g);
        eg_flags_nz(&e, R_T2, g);
        break;

      case 0x30 ... 0x37: // add r8, imm8
        eg_get(&e, R_T0, r8);
        xe_movi(&e, R_T1, (s32)imm8);
        xe_add(&e, R_T2, R_T0, R_T1);
        eg_flags_add_cv(&e, g);
        eg_flags_nz(&e, R_T2, g);
        eg_put(&e, R_T2, r8);
        break;

      case 0x38 ... 0x3F: // sub r8, imm8
        eg_get(&e, R_T0, r8);
        xe_movi(&e, R_T1, (s32)imm8);
        xe_sub(&e, R_T2, R_T0, R_T1);
        eg_flags_sub_cv(&e, g);
        eg_flags_nz(&e, R_T2, g);
        eg_put(&e, R_T2, r8);
        break;

      /* ---- ALU reg-reg (subset; adc/sbc/shifts-by-reg/ror → classic) ---- */
      case 0x40:
        switch ((op >> 6) & 3)
        {
          case 0: // and
          {
            u32 a = eg_src(&e, rd, R_T0), b = eg_src(&e, rs, R_T1), d = eg_dst(rd, R_T0);
            xe_and(&e, d, a, b);
            eg_flags_nz(&e, d, g); eg_dst_put(&e, rd, d);
            break;
          }
          case 1: // eor
          {
            u32 a = eg_src(&e, rd, R_T0), b = eg_src(&e, rs, R_T1), d = eg_dst(rd, R_T0);
            xe_xor(&e, d, a, b);
            eg_flags_nz(&e, d, g); eg_dst_put(&e, rd, d);
            break;
          }
          default: // lsl/lsr by reg → classic (carry-randgevallen)
            eg_exit_pc(&e, p, cyc - (s32)seq, JEXIT_CLASSIC_ONE);
            terminated = 1;
            break;
        }
        break;

      case 0x41:
        // asr/adc/sbc/ror by reg → classic
        eg_exit_pc(&e, p, cyc - (s32)seq, JEXIT_CLASSIC_ONE);
        terminated = 1;
        break;

      case 0x42:
        switch ((op >> 6) & 3)
        {
          case 0: // tst
          {
            u32 a = eg_src(&e, rd, R_T0), b = eg_src(&e, rs, R_T1);
            xe_and(&e, R_T2, a, b);             // result in scratch (no dest)
            eg_flags_nz(&e, R_T2, g);
            break;
          }
          case 1: // neg
            eg_get(&e, R_T1, rs);
            xe_movi(&e, R_T0, 0);
            xe_sub(&e, R_T2, R_T0, R_T1);
            eg_flags_sub_cv(&e, g);
            eg_flags_nz(&e, R_T2, g);
            eg_put(&e, R_T2, rd);
            break;
          case 2: // cmp
            eg_get(&e, R_T0, rd); eg_get(&e, R_T1, rs);
            xe_sub(&e, R_T2, R_T0, R_T1);
            eg_flags_sub_cv(&e, g);
            eg_flags_nz(&e, R_T2, g);
            break;
          default: // cmn
            eg_get(&e, R_T0, rd); eg_get(&e, R_T1, rs);
            xe_add(&e, R_T2, R_T0, R_T1);
            eg_flags_add_cv(&e, g);
            eg_flags_nz(&e, R_T2, g);
            break;
        }
        break;

      case 0x43:
        switch ((op >> 6) & 3)
        {
          case 0: // orr
          {
            u32 a = eg_src(&e, rd, R_T0), b = eg_src(&e, rs, R_T1), d = eg_dst(rd, R_T0);
            xe_or(&e, d, a, b);
            eg_flags_nz(&e, d, g); eg_dst_put(&e, rd, d);
            break;
          }
          case 1: // mul
          {
            u32 a = eg_src(&e, rd, R_T0), b = eg_src(&e, rs, R_T1), d = eg_dst(rd, R_T0);
            xe_mull(&e, d, a, b);
            eg_flags_nz(&e, d, g); eg_dst_put(&e, rd, d);
            break;
          }
          case 2: // bic
            eg_get(&e, R_T0, rd); eg_get(&e, R_T1, rs);
            xe_movi(&e, R_T2, -1);
            xe_xor(&e, R_T1, R_T1, R_T2);
            xe_and(&e, R_T0, R_T0, R_T1);
            eg_flags_nz(&e, R_T0, g); eg_put(&e, R_T0, rd);
            break;
          default: // mvn
          {
            u32 b = eg_src(&e, rs, R_T1), d = eg_dst(rd, R_T0);
            xe_movi(&e, R_T2, -1);
            xe_xor(&e, d, b, R_T2);
            eg_flags_nz(&e, d, g); eg_dst_put(&e, rd, d);
            break;
          }
        }
        break;

      /* ---- hi-register ops ---- */
      case 0x44: case 0x46:
      {
        u32 hrd = ((op >> 4) & 8) | (op & 7);
        u32 hrs = (op >> 3) & 0xF;
        if (hrd == REG_PC || hrs == REG_PC)
        { // PC-varianten: zeldzaam → classic
          eg_exit_pc(&e, p, cyc - (s32)seq, JEXIT_CLASSIC_ONE);
          terminated = 1;
          break;
        }
        eg_get(&e, R_T0, hrs);
        if (hi == 0x44)
        {
          eg_get(&e, R_T1, hrd);
          xe_add(&e, R_T0, R_T0, R_T1);
        }
        eg_put(&e, R_T0, hrd);
        break;
      }
      case 0x45: // cmp hireg → classic (incl. PC-randgevallen)
        eg_exit_pc(&e, p, cyc - (s32)seq, JEXIT_CLASSIC_ONE);
        terminated = 1;
        break;

      case 0x47: // bx
      {
        u32 hrs = (op >> 3) & 0xF;
        if (hrs == REG_PC)
        {
          eg_exit_pc(&e, p, cyc - (s32)seq, JEXIT_CLASSIC_ONE);
          terminated = 1;
          break;
        }
        eg_get(&e, R_T0, hrs);
        xe_movi(&e, R_T1, 1);
        xe_and(&e, R_T2, R_T0, R_T1);        // bit0
        xe_movi(&e, R_T3, 0);
        u8 *fx = xe_bcc(&e, XE_EQ, R_T2, R_T3); // bit0==0 → ARM-pad
        // Thumb-pad: PC = val & ~1
        xe_movi(&e, R_T1, -2);
        xe_and(&e, R_T0, R_T0, R_T1);
        eg_exit_pcreg(&e, R_T0, cyc, JEXIT_CONTINUE);
        // ARM-pad:
        xe_patch_bcc(fx, e.ptr);
        xe_movi(&e, R_T1, -4);
        xe_and(&e, R_T0, R_T0, R_T1);
        xe_l32i(&e, R_T1, R_BASE, OFF_CPSR); // CPSR &= ~0x20 (T-bit weg)
        xe_movi(&e, R_T2, 0x20);
        xe_movi(&e, R_T3, -1);
        xe_xor(&e, R_T2, R_T2, R_T3);
        xe_and(&e, R_T1, R_T1, R_T2);
        xe_s32i(&e, R_T1, R_BASE, OFF_CPSR);
        eg_exit_pcreg(&e, R_T0, cyc, JEXIT_MODE);
        terminated = 1;
        break;
      }

      /* ---- pc-relatieve constanten ---- */
      case 0x48 ... 0x4F: // ldr r8, [pc, imm8] → ROM-literal folden
      {
        u32 a = ((p + 4) & ~3u) + imm8 * 4;
        xe_load_const(&e, R_T0, jfetch32(a));
        eg_put(&e, R_T0, r8);
        cyc += ws_cyc_nseq[(a >> 24) & 0xF][1];
        break;
      }
      case 0xA0 ... 0xA7: // add r8, pc, imm
        xe_load_const(&e, R_T0, ((p + 4) & ~3u) + imm8 * 4);
        eg_put(&e, R_T0, r8);
        break;
      case 0xA8 ... 0xAF: // add r8, sp, imm
        eg_get(&e, R_T0, REG_SP);
        xe_load_const(&e, R_T1, imm8 * 4);
        xe_add(&e, R_T0, R_T0, R_T1);
        eg_put(&e, R_T0, r8);
        break;
      case 0xB0: // add sp, ±imm
      {
        s32 off = (op & 0x80) ? -(s32)((op & 0x7F) * 4) : (s32)((op & 0x7F) * 4);
        eg_get(&e, R_T0, REG_SP);
        if (off >= -128 && off <= 127)
          xe_addi(&e, R_T0, R_T0, off);
        else
        {
          xe_load_const(&e, R_T1, (u32)off);
          xe_add(&e, R_T0, R_T0, R_T1);
        }
        eg_put(&e, R_T0, REG_SP);
        break;
      }

      /* ---- loads/stores ---- */
      case 0x50: case 0x51: eg_addr_rr(&e, rs, rn); eg_mem_store(&e, jit_st32, p, rd); break;
      case 0x52: case 0x53: eg_addr_rr(&e, rs, rn); eg_mem_store(&e, jit_st16, p, rd); break;
      case 0x54: case 0x55: eg_addr_rr(&e, rs, rn); eg_mem_store(&e, jit_st8, p, rd); break;
      case 0x56: case 0x57: eg_addr_rr(&e, rs, rn); eg_mem_load(&e, jit_load_s8, p, rd); break;
      case 0x58: case 0x59: eg_addr_rr(&e, rs, rn); eg_mem_load(&e, jit_load_u32, p, rd); break;
      case 0x5A: case 0x5B: eg_addr_rr(&e, rs, rn); eg_mem_load(&e, jit_load_u16, p, rd); break;
      case 0x5C: case 0x5D: eg_addr_rr(&e, rs, rn); eg_mem_load(&e, jit_load_u8, p, rd); break;
      case 0x5E: case 0x5F: eg_addr_rr(&e, rs, rn); eg_mem_load(&e, jit_load_s16, p, rd); break;

      case 0x60 ... 0x67: eg_addr_ri(&e, rs, imm5 * 4); eg_mem_store(&e, jit_st32, p, rd); break;
      case 0x68 ... 0x6F: eg_addr_ri(&e, rs, imm5 * 4); eg_mem_load(&e, jit_load_u32, p, rd); break;
      case 0x70 ... 0x77: eg_addr_ri(&e, rs, imm5); eg_mem_store(&e, jit_st8, p, rd); break;
      case 0x78 ... 0x7F: eg_addr_ri(&e, rs, imm5); eg_mem_load(&e, jit_load_u8, p, rd); break;
      case 0x80 ... 0x87: eg_addr_ri(&e, rs, imm5 * 2); eg_mem_store(&e, jit_st16, p, rd); break;
      case 0x88 ... 0x8F: eg_addr_ri(&e, rs, imm5 * 2); eg_mem_load(&e, jit_load_u16, p, rd); break;

      case 0x90 ... 0x97: // str r8, [sp, imm8*4]
        eg_get(&e, R_T0, REG_SP);
        xe_load_const(&e, R_T1, imm8 * 4);
        xe_add(&e, R_A0, R_T0, R_T1);
        eg_mem_store(&e, jit_st32, p, r8);
        break;
      case 0x98 ... 0x9F: // ldr r8, [sp, imm8*4]
        eg_get(&e, R_T0, REG_SP);
        xe_load_const(&e, R_T1, imm8 * 4);
        xe_add(&e, R_A0, R_T0, R_T1);
        eg_mem_load(&e, jit_load_u32, p, r8);
        break;

      /* ---- push/pop/stm/ldm via helper ---- */
      case 0xB4: case 0xB5: case 0xBC: case 0xBD:
      case 0xC0 ... 0xCF:
      {
        u32 rlist = imm8;
        u32 rb, mode;
        if (hi == 0xB4) { rb = REG_SP; mode = 2; }
        else if (hi == 0xB5) { rb = REG_SP; mode = 2; rlist |= 0x100; }
        else if (hi == 0xBC) { rb = REG_SP; mode = 3; }
        else if (hi == 0xBD) { rb = REG_SP; mode = 3; rlist |= 0x100; }
        else if (hi <= 0xC7) { rb = r8; mode = 0; }
        else { rb = r8; mode = 1; }

        eg_sync_pc(&e, p);              // R_T3 = p
        xe_load_const(&e, R_A0, rlist | (rb << 12) | (mode << 16));
        xe_addi(&e, R_A1, R_T3, 2);     // next_pc without an extra literal
#if JIT_REG_ALLOC
        // jit_blockmem reads/writes reg[] directly (rlist + base). Flush pins so the
        // helper sees the current values, and reload afterwards because an ldm/pop
        // may have overwritten a pinned reg.
        for (int i = 0; i < NPIN; i++)
          xe_s32i(&e, pin_xt[i], R_BASE, OFF_REG(pin_arm[i]));
#endif
        eg_call(&e, jit_blockmem);
#if JIT_REG_ALLOC
        for (int i = 0; i < NPIN; i++)
          xe_l32i(&e, pin_xt[i], R_BASE, OFF_REG(pin_arm[i]));
#endif
        // a10: 0=continue, 1=pop-pc (PC set), 5=HALT (PC set)
        xe_movi(&e, R_T3, 0);
        u8 *fx_cont = xe_bcc(&e, XE_EQ, R_A0, R_T3);
        // a10 != 0 → exit: a2 = a10 - 1 (1→CONTINUE=0, 5→HALT=4)
        xe_addi(&e, R_EXIT, R_A0, -1);
        eg_cycles(&e, cyc);
        eg_ret(&e);
        xe_patch_bcc(fx_cont, e.ptr);
        if (hi == 0xBD) // pop pc ends the block anyway
        {
          // (a10 was 1 → already handled above; this path is unreachable
          //  but for safety: exit via the REG_PC the helper set)
          xe_movi(&e, R_EXIT, JEXIT_CONTINUE);
          eg_cycles(&e, cyc);
          eg_ret(&e);
          terminated = 1;
        }
        break;
      }

      /* ---- branches (end the block) ---- */
      case 0xD0 ... 0xDD: // bcc
      {
        u32 tgt = p + 4 + (u32)(((s32)(s8)imm8) * 2);
        u8 *fx = eg_cond_branch(&e, hi - 0xD0);  // branch-to-taken-if-true (cond fusion)
        eg_link(&e, p + 2, cyc + (s32)ws_cyc_nseq[(p >> 24) & 0xF][0]);   // not taken
        xe_patch_bcc(fx, e.ptr);
        // Idle loop? Curated (idle_loop_target_pc) or generically detected (block_is_idle:
        // pure poll loop that jumps to block start). Taken branch (spin) → fast-forward.
        if ((idle_loop_target_pc && p == idle_loop_target_pc && tgt <= p) ||
            (block_is_idle && tgt == pc))
          eg_idle_exit(&e, tgt);
        else
          eg_link(&e, tgt, cyc + (s32)ws_cyc_nseq[(tgt >> 24) & 0xF][0]);   // taken
        terminated = 1;
        break;
      }

      case 0xE0 ... 0xE7: // b
      {
        u32 offset = op & 0x7FF;
        u32 tgt = p + (u32)(((s32)(offset << 21) >> 20) + 4);
        eg_link(&e, tgt, cyc + (s32)ws_cyc_nseq[(tgt >> 24) & 0xF][0]);
        terminated = 1;
        break;
      }

      case 0xF0 ... 0xF7: // bl (fused with the high halfword)
      {
        u16 op2 = jfetch16(p + 2);
        if ((op2 >> 11) == 0x1F)
        {
          u32 off_lo = op & 0x7FF;
          u32 off_hi = op2 & 0x7FF;
          u32 tgt = p + 4 + (u32)(((s32)(off_lo << 21) >> 9)) + off_hi * 2;
          xe_load_const(&e, R_T0, (p + 4) | 1);
          eg_put(&e, R_T0, REG_LR);
          eg_link(&e, tgt, cyc + (s32)seq +
                  (s32)ws_cyc_nseq[(tgt >> 24) & 0xF][0]);
          tb_n++; // BL counts as two Thumb instructions (verify budget)
        }
        else
          eg_exit_pc(&e, p, cyc - (s32)seq, JEXIT_CLASSIC_ONE);
        terminated = 1;
        break;
      }

      /* ---- rest (swi, misc, unpaired) → classic ---- */
      default:
        eg_exit_pc(&e, p, cyc - (s32)seq, JEXIT_CLASSIC_ONE);
        terminated = 1;
        break;
    }

    p += 2;
    tb_n++;
    idx++;
  }

  if (!terminated)
    eg_exit_pc(&e, p, cyc, JEXIT_CONTINUE);

  if (e.overflow || xe_bcc_overflow)
  {
    // Safety net with guaranteed progress: a minimal block that hands this one
    // instruction to the classic interpreter (always fits).
    // (xe_bcc_overflow = a conditional branch beyond ±127 bytes, e.g. from an
    //  inflated eg_link under JIT_LINK_CACHE → otherwise a silent misjump.)
    if (xe_bcc_overflow) jit_stats.bcc_overflow++;
    xe_init(&e, blk_wr, BLOCK_RESERVE + LIT_SLOTS * 4, LIT_SLOTS,
            (s32)(jit_ex - jit_wr));
    // eg_prologue (instead of a bare xe_entry+l32r): correct in every ABI mode and
    // under reg-alloc loads the pins, so eg_ret's pin flush doesn't write stale
    // a5/a6/a7 over reg[0..2] (and restores the call0 blink link correctly).
    eg_prologue(&e);
    eg_exit_pc(&e, pc, 0, JEXIT_CLASSIC_ONE);
  }

  u32 len = (u32)(e.ptr - blk_wr);
  jit_cache_sync(blk_wr, jit_ex + (blk_wr - jit_wr), len);
  jit_used += (len + 15) & ~15u;
  u32 entry_off = (u32)(blk_wr - jit_wr) + LIT_SLOTS * 4;
  jit_stats.blocks++;

  // wlen = full block byte length (literals + code) from blk_wr, for the IRAM
  // copy. blk_wr is 16-aligned, so a 16-aligned IRAM copy preserves the l32r
  // offsets and alignment rounding exactly.
  return jhash_insert(pc, entry_off, (u16)tb_n, tb_pure ? JF_PURE : 0, (u16)len);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

static int run_classic_until_rom_thumb(u32 budget)
{
  jit_stats.classic_calls++;
  cached_exit_flag = 0;
  cached_exit_budget = budget;
  execute_arm((u32)jit_cycles);
  if (!cached_exit_flag)
  {
    cached_exit_budget = 0;
    return 0;
  }
  jit_cycles = cached_exit_cycles;
  return 1;
}

// Copy a hot block into internal IRAM (direct SRAM, no I-cache → no PSRAM fetch
// stalls). A byte copy of [block_base..end] preserves l32r/branch offsets
// (16-aligned src+dst); memw+isync makes the code visible to the fetch. On full
// IRAM: do nothing (runcount is already past the threshold → no retry). Sets
// iram_entry = the exec address of the entry in the copy.
static void jit_pin_block(jhash_t *blk)
{
  u32 al = (jit_iram_used + 15) & ~15u;
  // jit_iram is MALLOC_CAP_EXEC = instruction-bus RAM: data writes to it MUST be
  // 32-bit words. A plain memcpy does byte/halfword tail writes (wlen is rarely a
  // multiple of 4) -> StoreProhibited panic. Copy per u32 with rounded-up length;
  // src (block_base) is 4-aligned, dst 16-aligned, and the <=3 extra source bytes
  // fall inside the BLOCK_RESERVE slack.
  u32 clen = ((u32)blk->wlen + 3u) & ~3u;
  if (!blk->wlen || al + clen > jit_iram_size)
    return;
  const u32 *src = (const u32 *)(jit_wr + blk->offset - LIT_SLOTS * 4);  // block_base
  u32 *dst = (u32 *)(jit_iram + al);
  for (u32 i = 0; i < clen / 4; i++)
    dst[i] = src[i];
  __asm__ volatile("memw" ::: "memory");
  __asm__ volatile("isync");
  jit_iram_used = al + clen;
  blk->iram_entry = (u32)(uintptr_t)(jit_iram + al + LIT_SLOTS * 4);   // entry = after the pool
  jp_pinned++;
}


static void execute_jit_impl(u32 cycles);
void execute_jit(u32 cycles)
{
  execute_jit_impl(cycles);
}
static void execute_jit_impl(u32 cycles)
{
  if (!jit_enabled || !jit_wr || cheat_master_hook != ~0U)
  {
    execute_arm(cycles);
    return;
  }

  jit_cycles = (s32)cycles;


  // Host pointers for the mem fast-path stubs (cpu_jit_stub.S) in the REG_USERDEF
  // slots. Each entry is set again so a savestate load can't break them.
  // SMC_DETECTION=0 → iwram/ewram without shadow.
  reg[32] = (u32)(uintptr_t)&iwram[0];               // SLOT_IWRAM
  reg[34] = (u32)(uintptr_t)&ewram[0];               // SLOT_EWRAM
  reg[36] = (u32)(uintptr_t)&memory_map_read[0];     // SLOT_MAP
  reg[37] = (u32)(uintptr_t)&vram[0];                // SLOT_VRAM (stores, increment 2)

  while (1)
  {
    if (reg[CPU_HALT_STATE] != CPU_ACTIVE)
    {
      u32 ret = update_gba(jit_cycles);
      if (completed_frame(ret))
        return;
      jit_cycles = (s32)cycles_to_run(ret);
    }

    u32 pc = reg[REG_PC];

    if (!(reg[REG_CPSR] & 0x20) || (u32)((pc >> 24) - 0x08) > 5)
    {
      if (!run_classic_until_rom_thumb(1))
        return;
      continue;
    }

    if (jit_cycles <= 0)
    {
      u32 ret = update_gba(jit_cycles);
      if (completed_frame(ret))
        return;
      jit_cycles = (s32)cycles_to_run(ret);
      continue;
    }

    jhash_t *blk = jhash_lookup(pc);
    if (!blk)
      blk = translate_block(pc);

#if JIT_IRAM_PIN
    blk->runcount++;                 // hotness (pinning)
#endif
#if JIT_IRAM_PIN
    if (jit_iram && !blk->iram_entry && blk->runcount == PIN_THRESHOLD)
      jit_pin_block(blk);            // no-op when no exec-IRAM is free
#endif
    jit_block_fn fn = (jit_block_fn)(blk->iram_entry
                          ? (u8 *)(uintptr_t)blk->iram_entry
                          : (jit_ex + blk->offset));
    jit_alert = 0;


    // CPSR ↔ flag slots: the interpreter keeps NZCV in CPSR bits, the JIT in
    // reg[REG_*_FLAG] slots. Convert at every boundary, else blocks branch on stale
    // flags (or vice versa the interpreter/SWI path).
    {
      u32 cpsr = reg[REG_CPSR];
      reg[REG_N_FLAG] = cpsr >> 31;
      reg[REG_Z_FLAG] = (cpsr >> 30) & 1;
      reg[REG_C_FLAG] = (cpsr >> 29) & 1;
      reg[REG_V_FLAG] = (cpsr >> 28) & 1;
    }

#if JIT_CALL0_BLOCKS
    u32 rc = jit_call0((void *)fn);   // call0 via the asm bridge (no windowed entry/retw)
#else
    u32 rc = fn();
#endif
    jit_stats.block_runs++;

    reg[REG_CPSR] = (reg[REG_N_FLAG] << 31) | (reg[REG_Z_FLAG] << 30) |
                    (reg[REG_C_FLAG] << 29) | (reg[REG_V_FLAG] << 28) |
                    (reg[REG_CPSR] & 0xFF);


    // Idle-loop detection (per block, cheap in C)
    if (reg[REG_PC] == idle_loop_target_pc && jit_cycles > 0)
      jit_cycles = 0;

    switch (rc)
    {
      case JEXIT_CONTINUE:
        break;
      case JEXIT_MODE:
      case JEXIT_HALT:
        break; // the loop top handles CPSR/halt
      case JEXIT_CLASSIC_ONE:
        if (!run_classic_until_rom_thumb(2))
          return;
        break;
    }
  }
}
