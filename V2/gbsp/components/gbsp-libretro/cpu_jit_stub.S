# cpu_jit_stub.S — memory fast-path stubs (Xtensa LX7 / ESP32-S3).
#
# Our S3 JIT is "route A": ALL ARM registers live in reg[] memory, so the stub
# need not save ARM registers. The only overhead we remove is the windowed callx8
# to the C helper. Hence a cheap call0 (callx0 — no window rotation) to these
# hand-written IRAM stubs.
#
# Calling convention from JIT blocks (callx0):
#   loads : a10 = GBA address, a11 = current PC   -> result in a10
#   Clobbers ONLY a0(link), a4, a8, a9, a10. Reads a3 (= &reg[0], guaranteed live
#   by the block) and a11. The block saves its own windowed retw link (a0) in a13
#   around the callx0.
#
# Fast path: IWRAM(0x03)/EWRAM(0x02)/ROM(0x08..0x0C), aligned. Cycles deducted
# exactly like the JIT_READ macro (ws_cyc_nseq[region][col], region<0x10).
# Slow path: delegate to the existing C helper jit_ld* (unaligned, BIOS/IO/OAM/
# palette/VRAM, non-resident ROM page). That does its own cycle deduction +
# REG_PC-dependent open-bus, so the stub sets REG_PC first.
#
# SMC_DETECTION=0 in this build: no iwram/ewram shadow, no store alerts from RAM.

	.equ	REG_PC_OFF, 15*4
	.equ	SLOT_IWRAM, 32*4	# reg[32] = &iwram[0]           (set every frame)
	.equ	SLOT_EWRAM, 34*4	# reg[34] = &ewram[0]
	.equ	SLOT_MAP,   36*4	# reg[36] = &memory_map_read[0]

	.section .iram1,"ax",@progbits
	.literal_position
	# Explicit literals (bare `as` doesn't relax `movi reg,symbol`). l32r only
	# reads backwards, so the pool sits before all code.
	.literal lit_wsnseq, ws_cyc_nseq
	.literal lit_cycles, jit_cycles
	.literal lit_ld32,  jit_ld32
	.literal lit_ld16,  jit_ld16
	.literal lit_ld8,   jit_ld8
	.literal lit_lds8,  jit_lds8
	.literal lit_lds16, jit_lds16
	.literal lit_regbase, reg		# &reg[0] (for jit_call0's own link-save)
	.align 4

# Region dispatch: compute the host pointer in a9 from the address in a10.
# Non-fast regions jump to \slow. Clobbers a8/a9; a10 (address) stays.
	.macro REGION_DISPATCH slow
	ssai	24
	srl	a8, a10			# region = addr >> 24
	addi	a9, a8, -3
	beqz	a9, 201f		# 0x03 IWRAM
	addi	a9, a8, -2
	beqz	a9, 202f		# 0x02 EWRAM
	addi	a9, a8, -8
	bltz	a9, \slow		# < 0x08 -> slow (BIOS/IO/pal/vram/oam)
	movi	a9, 0x0C
	bltu	a9, a8, \slow		# > 0x0C -> slow (SRAM/EEPROM/3rd ROM mirror)
	# ROM via memory_map_read[addr>>15]
	l32i	a9, a3, SLOT_MAP
	extui	a8, a10, 15, 13		# page = addr>>15 (region<0x10 => <0x2000)
	addx4	a9, a8, a9
	l32i	a9, a9, 0		# map[page]
	beqz	a9, \slow		# page not resident -> slow (loads it)
	extui	a8, a10, 0, 15		# offset = addr & 0x7FFF
	add	a9, a9, a8
	j	203f
201:
	l32i	a9, a3, SLOT_IWRAM
	extui	a8, a10, 0, 15		# addr & 0x7FFF
	add	a9, a9, a8
	j	203f
202:
	l32i	a9, a3, SLOT_EWRAM
	slli	a8, a10, 14
	srli	a8, a8, 14		# addr & 0x3FFFF
	add	a9, a9, a8
203:
	.endm

# Cycle deduction: jit_cycles -= ws_cyc_nseq[region][\col]. Region from a10 (address);
# a9 (host pointer) stays intact. Clobbers a4/a8/a10.
	.macro DEDUCT_CYCLES col
	ssai	24
	srl	a8, a10			# region
	l32r	a10, lit_wsnseq
	addx2	a10, a8, a10		# &ws_cyc_nseq[region][0]  (region*2 + base)
	l8ui	a10, a10, \col		# cost (col=1 for 32-bit, else 0)
	l32r	a8, lit_cycles
	l32i	a4, a8, 0
	sub	a4, a4, a10
	s32i	a4, a8, 0
	.endm

# ---- windowed -> call0 block bridge ----
# u32 jit_call0(void *block_entry);  The dispatcher (windowed C) calls this; we
# callx0 the block (call0 — no windowed entry/retw per block). The block runs in
# THIS window (a0-a15 flat), puts its exit code in a2 and returns via ret.n. Our
# own windowed return link (a0) is overwritten by callx0, so we save it in reg[39]
# (backend-private). a2 = block_entry = exit code (call0).
	.align	4
	.global	jit_call0
	.type	jit_call0,@function
jit_call0:
	entry	a1, 64
	l32r	a3, lit_regbase
	s32i	a0, a3, 39*4		# save windowed return link
	callx0	a2			# call block; block ret a2 = exit code
	l32r	a3, lit_regbase
	l32i	a0, a3, 39*4		# restore windowed return link
	retw.n

# ---- loads ----

	.global	jit_load_u32
	.type	jit_load_u32,@function
jit_load_u32:
	extui	a9, a10, 0, 2		# aligned? (addr & 3)
	bnez	a9, 9f
	REGION_DISPATCH 9f
	DEDUCT_CYCLES 1
	l32i	a10, a9, 0
	ret
9:
	s32i	a11, a3, REG_PC_OFF	# REG_PC for BIOS open-bus
	l32r	a8, lit_ld32
	callx8	a8			# a10 = jit_ld32(a10)
	ret

	.global	jit_load_u16
	.type	jit_load_u16,@function
jit_load_u16:
	extui	a9, a10, 0, 1		# aligned? (addr & 1)
	bnez	a9, 9f
	REGION_DISPATCH 9f
	DEDUCT_CYCLES 0
	l16ui	a10, a9, 0
	ret
9:
	s32i	a11, a3, REG_PC_OFF
	l32r	a8, lit_ld16
	callx8	a8
	ret

	.global	jit_load_u8
	.type	jit_load_u8,@function
jit_load_u8:
	REGION_DISPATCH 9f
	DEDUCT_CYCLES 0
	l8ui	a10, a9, 0
	ret
9:
	s32i	a11, a3, REG_PC_OFF
	l32r	a8, lit_ld8
	callx8	a8
	ret

	.global	jit_load_s8
	.type	jit_load_s8,@function
jit_load_s8:
	REGION_DISPATCH 9f
	DEDUCT_CYCLES 0
	l8ui	a10, a9, 0
	slli	a10, a10, 24		# sign-extend byte
	srai	a10, a10, 24
	ret
9:
	s32i	a11, a3, REG_PC_OFF
	l32r	a8, lit_lds8
	callx8	a8
	ret

	.global	jit_load_s16
	.type	jit_load_s16,@function
jit_load_s16:
	extui	a9, a10, 0, 1		# aligned? (addr & 1) — unaligned LDRSH
	bnez	a9, 9f			# = sign-extended byte: let the C helper handle it
	REGION_DISPATCH 9f
	DEDUCT_CYCLES 0
	l16ui	a10, a9, 0
	slli	a10, a10, 16		# sign-extend halfword
	srai	a10, a10, 16
	ret
9:
	s32i	a11, a3, REG_PC_OFF
	l32r	a8, lit_lds16
	callx8	a8
	ret
