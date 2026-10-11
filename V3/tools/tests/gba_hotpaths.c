/* No CRT dependencies: run as a host DLL using Zig cc and the ESP lld linker.
 * The production functions are extracted by test_gba_hotpaths.py, not copied. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;
#define NULL ((void *)0)
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)
#define JIT_HASH_BITS 14
#define JIT_HASH_SIZE (1u << JIT_HASH_BITS)
typedef struct {
  u32 pc, offset;
  u16 n_instr, flags, wlen, _pad;
  u32 runcount, iram_entry;
} jhash_t;
static jhash_t jit_hash[JIT_HASH_SIZE];
static u32 jit_hash_used, jit_hash_tomb, jit_code_gen;
static struct { u32 half_flushes; } jit_stats;
#include "jit_functions.h"

static int hash_counts_valid(void)
{
  u32 live = 0, tomb = 0;
  for (u32 i = 0; i < JIT_HASH_SIZE; i++) {
    if (!jit_hash[i].offset) continue;
    if (jit_hash[i].pc == 1u) tomb++;
    else live++;
  }
  return live == jit_hash_used && tomb == jit_hash_tomb;
}

__declspec(dllexport) int test_jit_hash(void)
{
  for (u32 i = 0; i < JIT_HASH_SIZE; i++) jit_hash[i].offset = 0;
  jit_hash_used = jit_hash_tomb = jit_code_gen = jit_stats.half_flushes = 0;
  /* Three distinct PCs with the same initial bucket exercise probe-chain retention. */
  u32 a = 0x08000000u, b = a + 2, c;
  while (jhash_pc(b) != jhash_pc(a)) b += 2;
  c = b + 2;
  while (jhash_pc(c) != jhash_pc(a)) c += 2;
  jhash_insert(a, 64, 2, 0, 16);
  jhash_insert(b, 1024, 3, 1, 32);
  CHECK(hash_counts_valid());
  jit_half_flush(0, 1024);
  CHECK(!jhash_lookup(a));
  CHECK(jhash_lookup(b) && jhash_lookup(b)->offset == 1024);
  CHECK(hash_counts_valid());
  jhash_insert(c, 128, 4, 2, 48); /* The original code fails the next invariant. */
  CHECK(hash_counts_valid());
  CHECK(jhash_lookup(c)->n_instr == 4 && jhash_lookup(c)->wlen == 48);
  CHECK(jhash_lookup(b)->flags == 1);
  for (u32 round = 0; round < 100; round++) {
    jit_half_flush(0, 1024);
    CHECK(hash_counts_valid());
    CHECK(!jhash_lookup(c) && jhash_lookup(b));
    jhash_insert(c, 128, 4, 2, 48);
    CHECK(hash_counts_valid());
  }
  CHECK(jit_code_gen == 101 && jit_stats.half_flushes == 101);
  jit_half_flush(1024, 2048);
  CHECK(!jhash_lookup(b) && jhash_lookup(c));
  CHECK(hash_counts_valid());
  jit_half_flush(0, 2048);
  CHECK(hash_counts_valid() && jit_hash_used == 0);
  return 0;
}

/* ROM loader IO doubles. Data depends on absolute offset so a stale FILE cursor
 * or an incorrectly mapped cache page cannot pass the byte comparisons. */
#define PAGE (32u * 1024)
#define BLOCK (32u * PAGE)
#define ROM_PREFETCH 0
#define SEEK_SET 0
#define SEEK_END 2
static u8 cache[3][BLOCK];
static u8 *gamepak_buffers[3] = {cache[0], cache[1], cache[2]};
static u16 gamepak_block_pages[3] = {32, 32, 3};
static u32 gamepak_buffer_count, gamepak_buffer_blocksize = BLOCK, gamepak_size;
static struct { int phy_rom; } gamepak_blk_queue[96];
static u8 *mapped[1024];
static u32 evict_next;
static long fake_size;
static u32 cursor, raw_calls, file_calls, close_calls, raw_close_calls;
static u32 seek_calls, bad_seek_call, raw_fail_mask;
static int raw_available, open_fails, short_read;
static void *gamepak_file_large;
#if ROM_RAW_READ
static void *g_rawrom;
#endif

static u8 rom_byte(u32 offset) { return (u8)((offset >> 15) * 37u + (offset & 255u)); }
void *memset(void *dst, int value, __SIZE_TYPE__ size)
{
  for (__SIZE_TYPE__ i = 0; i < size; i++) ((u8 *)dst)[i] = (u8)value;
  return dst;
}
static void *fopen(const char *name, const char *mode)
{
  (void)name; (void)mode;
  return open_fails ? NULL : &cursor;
}
static int fclose(void *file) { (void)file; close_calls++; return 0; }
static int fseek(void *file, long offset, int origin)
{
  (void)file;
  if (++seek_calls == bad_seek_call) return -1;
  cursor = (u32)(origin == SEEK_END ? fake_size + offset : offset);
  return 0;
}
static long ftell(void *file) { (void)file; return fake_size; }
static __SIZE_TYPE__ fread(void *dst, __SIZE_TYPE__ size, __SIZE_TYPE__ count, void *file)
{
  (void)file;
  file_calls++;
  u32 n = (u32)(size * count);
  if (cursor >= (u32)fake_size) return 0;
  if (n > (u32)fake_size - cursor) n = (u32)fake_size - cursor;
  if (short_read && n) n--;
  for (u32 i = 0; i < n; i++) ((u8 *)dst)[i] = rom_byte(cursor + i);
  cursor += n;
  return n / size;
}
#if ROM_RAW_READ
static void *rg_storage_rawrom_open(const char *name)
{ (void)name; return raw_available ? &raw_calls : NULL; }
static void rg_storage_rawrom_close(void *handle) { (void)handle; raw_close_calls++; }
static int rg_storage_rawrom_read(void *handle, u32 offset, void *dst, u32 len)
{
  (void)handle;
  u32 call = raw_calls++;
  if ((offset | len) & 511u || offset > (u32)fake_size || len > (u32)fake_size - offset)
    return 0;
  if (raw_fail_mask & (1u << call)) {
    /* Partial raw read contaminates the buffer; fallback must replace it. */
    memset(dst, 0xDE, len / 2);
    return 0;
  }
  for (u32 i = 0; i < len; i++) ((u8 *)dst)[i] = rom_byte(offset + i);
  return 1;
}
#endif
static u32 evict_gamepak_page(void) { return evict_next++; }
#define map_null(type, start, end) do { \
  for (u32 m = 0; m < 1024; m++) mapped[m] = NULL; \
} while (0)
#define map_rom_entry(type, page, ptr, total) (mapped[page] = (ptr))
#include "rom_functions.h"

static void reset_rom(long bytes)
{
  fake_size = bytes;
  cursor = evict_next = raw_calls = file_calls = close_calls = raw_close_calls = 0;
  seek_calls = bad_seek_call = raw_fail_mask = 0;
  raw_available = 1; open_fails = short_read = 0;
  gamepak_buffer_count = 3;
  gamepak_file_large = NULL;
#if ROM_RAW_READ
  g_rawrom = NULL;
#endif
  memset(cache, 0xA5, sizeof(cache));
  for (u32 i = 0; i < 1024; i++) mapped[i] = NULL;
}

static int check_rom(u32 expected_pages)
{
  CHECK(evict_next == expected_pages);
  for (u32 page = 0; page < expected_pages; page++) {
    CHECK(mapped[page] != NULL);
    CHECK(gamepak_blk_queue[page].phy_rom == (int)page);
    for (u32 i = 0; i < PAGE; i++) {
      u32 offset = page * PAGE + i;
      CHECK(mapped[page][i] == (offset < (u32)fake_size ? rom_byte(offset) : 0));
    }
  }
  CHECK(mapped[expected_pages] == NULL);
  return 0;
}

__declspec(dllexport) int test_rom_load(void)
{
  int result;
  /* 16MiB ROM, two full cache blocks plus a three-page partial block. */
  reset_rom(16 * 1024 * 1024);
  CHECK(load_gamepak_raw("test") == 0);
  if ((result = check_rom(67))) return result;
  CHECK(raw_calls == (ROM_RAW_READ ? 3u : 0u));
  CHECK(file_calls == (ROM_RAW_READ ? 0u : 3u));

  /* Raw -> failed raw/stdio -> raw, and failed raw/stdio -> raw -> stdio.
   * Neither raw success nor a partial failure advances FILE's cursor. */
  for (u32 mask = 1; mask <= 7; mask++) {
    reset_rom(16 * 1024 * 1024);
    raw_fail_mask = mask;
    CHECK(load_gamepak_raw("test") == 0);
    if ((result = check_rom(67))) return result;
    CHECK(file_calls == (ROM_RAW_READ ? ((mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1)) : 3u));
  }
  reset_rom(16 * 1024 * 1024);
  raw_available = 0;
  CHECK(load_gamepak_raw("test") == 0);
  CHECK(raw_calls == 0 && file_calls == 3);
  if ((result = check_rom(67))) return result;

  /* Aligned and unaligned short last blocks, a tiny ROM, and a one-block ROM. */
  static const u32 sizes[] = {BLOCK, BLOCK + PAGE, BLOCK + 512, BLOCK + 513, 193};
  for (u32 n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
    reset_rom((long)sizes[n]);
    CHECK(load_gamepak_raw("test") == 0);
    u32 pages = (sizes[n] + PAGE - 1) / PAGE;
    CHECK(gamepak_size == pages * PAGE);
    if ((result = check_rom(pages))) return result;
    /* Bytes outside the final mapped page are not touched. */
    u32 block = (pages - 1) / 32, last = (pages - 1) % 32 + 1;
    if (last < 32) CHECK(cache[block][last * PAGE] == 0xA5);
  }

  /* Failures must return an error and release both file handles. */
  for (u32 failure = 0; failure < 7; failure++) {
    reset_rom(BLOCK);
    raw_available = 0; /* force stdio for seek/short-read checks */
    if (failure == 0) short_read = 1;
    if (failure == 1) bad_seek_call = 1;
    if (failure == 2) bad_seek_call = 2;
    if (failure == 3) fake_size = -1;
    if (failure == 4) fake_size = 0;
    if (failure == 5) fake_size = 32 * 1024 * 1024 + 1;
    if (failure == 6) gamepak_buffer_count = 0;
    CHECK(load_gamepak_raw("test") == -1);
    CHECK(gamepak_file_large == NULL && close_calls == 1 && evict_next == 0);
  }
  reset_rom(BLOCK);
  raw_fail_mask = 1; short_read = 1;
  CHECK(load_gamepak_raw("test") == -1);
  CHECK(close_calls == 1 && raw_close_calls == (ROM_RAW_READ ? 1u : 0u));
  reset_rom(BLOCK);
  open_fails = 1;
  CHECK(load_gamepak_raw("test") == -1);
  CHECK(close_calls == 0 && raw_close_calls == (ROM_RAW_READ ? 1u : 0u));
  return 0;
}
