/*
 * ia32_encode.h — inline x86/x86_64 instruction builders
 *
 * Every function here writes raw machine bytes into a mmap'd buffer and
 * returns a pointer to the next free byte.  The caller is responsible for
 * keeping the destination pointer aligned and within bounds.
 *
 * Quick reference on how x86 instruction encoding works:
 *
 *   [ Prefixes ] [ Opcode ] [ ModR/M ] [ SIB ] [ Displacement ] [ Immediate ]
 *
 * The ModR/M byte packs three fields into one byte:
 *
 *   Bits 7-6  Mod  — addressing mode
 *                    00 = no displacement (register-indirect)
 *                    01 = 8-bit signed displacement
 *                    10 = 32-bit signed displacement
 *                    11 = register-to-register (no memory)
 *   Bits 5-3  Reg  — source or destination register
 *   Bits 2-0  R/M  — other register (or base for memory access)
 *
 * In 64-bit mode, a REX prefix byte (0x40–0x4F) extends the register file
 * and selects 64-bit operand size.  The individual bits are:
 *   REX.W (bit 3) — 64-bit operand size
 *   REX.R (bit 2) — extends the Reg field to access R8–R15
 *   REX.X (bit 1) — extends the SIB Index field
 *   REX.B (bit 0) — extends the R/M or Opcode-reg field (for PUSH/POP r64)
 *
 * See Intel SDM Vol. 2, Chapter 2 for the full encoding reference.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <limits.h>    /* for PAGESIZE */

/* -----------------------------------------------------------------------
 * Encoding constants
 * ----------------------------------------------------------------------- */

#define PREFIX_16BIT   0x66   /* operand-size override: selects 16-bit width */
#define BASE_MODRM     0xc0   /* Mod=11 base — register-to-register mode     */
#define REG_SHIFT      0x3    /* shift Reg field into bits 5-3 of ModR/M     */
#define MODRM_SHIFT    0x6    /* shift Mod field into bits 7-6 of ModR/M     */
#define RM_SHIFT       0x0    /* R/M field sits in the low 3 bits            */
#define REG_MASK       0x7
#define RM_MASK        0x7
#define MOD_MASK       0x3

/* 32-bit register encodings — these are the values that go into ModR/M */
#define REG_EAX        0x0
#define REG_ECX        0x1
#define REG_EDX        0x2
#define REG_EBX        0x3    /* we keep EBX reserved as the data-area base pointer */
#define REG_ESP        0x4
#define REG_EBP        0x5
#define REG_ESI        0x6
#define REG_EDI        0x7

/*
 * x86_64 extended registers (R8–R15) — same 3-bit encoding as above,
 * but require a REX prefix to distinguish them from the legacy registers.
 * Guard against redefinition from sys/ucontext.h when _GNU_SOURCE is set.
 */
#ifndef REG_R8
#define REG_R8        0x0
#endif
#ifndef REG_R9
#define REG_R9        0x1
#endif
#ifndef REG_R10
#define REG_R10       0x2
#endif
#ifndef REG_R11
#define REG_R11       0x3
#endif
#ifndef REG_R12
#define REG_R12       0x4
#endif
#ifndef REG_R13
#define REG_R13       0x5
#endif
#ifndef REG_R14
#define REG_R14       0x6
#endif
#ifndef REG_R15
#define REG_R15       0x7
#endif

/* Byte-offset constants used when advancing the instruction pointer */
#define BYTE1_OFF      0x1
#define BYTE2_OFF      0x2
#define BYTE3_OFF      0x3
#define BYTE4_OFF      0x4

/* Operand sizes in bytes */
#define ISZ_1         0x1
#define ISZ_2         0x2
#define ISZ_4         0x4
#define ISZ_8         0x8

/* REX prefix fields — combine with bitwise OR, e.g. (REX_PREFIX | REX_W) */
#define REX_PREFIX    0x40
#define REX_W         0x8    /* 64-bit operand size */
#define REX_R         0x4    /* extends ModR/M Reg  */
#define REX_X         0x2    /* extends SIB Index   */
#define REX_B         0x1    /* extends R/M or opcode-reg (used for PUSH/POP r64) */

/* LOCK prefix — makes the following memory instruction atomic on the bus */
#define LOCK_PREFIX   0xF0

/* -----------------------------------------------------------------------
 * Memory region sizes — how much space we allocate per thread
 * ----------------------------------------------------------------------- */
#define MAX_THREADS     4
#define MAX_DEF_INSTRS  25
#define MAX_INSTR_BYTES (3*PAGESIZE)   /* 12 KB — plenty for 25 encoded instructions */
#define MAX_DATA_BYTES  (10*PAGESIZE)  /* 40 KB shared data area                     */
#define MAX_COMM_BYTES  (PAGESIZE)     /* 4 KB comm area — holds barrier counters     */

/* Indices into the test_info pointer array */
#define NUM_PTRS 3
#define CODE 0
#define DATA 1
#define COMM 2

/* =======================================================================
 * Stack helpers: PUSH and POP a single register
 *
 * In 64-bit mode the legacy PUSH/POP opcodes only reach RAX–RDI.
 * To reach R8–R15 you need a REX.B prefix before the opcode, which is
 * what x86_64f=1 does below.
 * ======================================================================= */

static inline volatile char *build_push_reg(int reg_index, int x86_64f, volatile char *tgt_addr)
{
	if (x86_64f) {
		/* REX.B prefix extends the opcode-reg field so 0x50+reg hits R8–R15 */
		(*(char *) tgt_addr) = (REX_PREFIX | REX_B);
		tgt_addr += BYTE1_OFF;
	}
	(*(char *) tgt_addr) = 0x50 + reg_index;
	tgt_addr += BYTE1_OFF;
        return(tgt_addr);
}

static inline volatile char *build_pop_reg(int reg_index, int x86_64f, volatile char *tgt_addr)
{
	if (x86_64f) {
		(*(char *) tgt_addr) = (REX_PREFIX | REX_B);
		tgt_addr += BYTE1_OFF;
	}
	(*(char *) tgt_addr) = 0x58 + reg_index;
	tgt_addr += BYTE1_OFF;
        return(tgt_addr);
}

/* PUSHA/POPA — 32-bit only (invalid in 64-bit long mode, used in the #else path) */
static inline volatile char *build_pusha(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0x60;
	return (tgt_addr);
}

static inline volatile char *build_popa(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0x61;
	return (tgt_addr);
}

/* =======================================================================
 * RET — near return, 1 byte (0xC3)
 * ======================================================================= */
static inline volatile char *build_ret(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0xc3;
	return (tgt_addr);
}

/* =======================================================================
 * MOV reg, reg — register-to-register move
 *
 * The key insight here is that all the register info lives in a single
 * ModR/M byte.  With Mod=11 (BASE_MODRM), both operands are registers.
 * The encoding is:  opcode | ModR/M
 *   8-bit:  0x8A ModR/M
 *   16-bit: 0x66 (prefix) 0x8B ModR/M
 *   32-bit: 0x8B ModR/M
 *   64-bit: REX.W 0x8B ModR/M
 * ======================================================================= */
static inline volatile char *build_mov_register_to_register(short mov_size, int src_reg, int dest_reg, volatile char *tgt_addr)
{
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;

	switch(mov_size) {
	case 1:
		/* 8-bit MOV uses opcode 0x8A */
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8a;
		 tgt_addr += BYTE2_OFF;
		 break;
	case 2:
	case 4:
		/* 16/32-bit MOV both use 0x8B; the prefix already selected the width */
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		 tgt_addr += BYTE2_OFF;
		 break;
	case 8:
		/* REX.W promotes the 32-bit 0x8B to a 64-bit operation */
		(*(char *) tgt_addr) = (REX_PREFIX | REX_W);
		tgt_addr += BYTE1_OFF;
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		tgt_addr += BYTE2_OFF;
		break;
	default:
		 fprintf(stderr,"ERROR: Incorrect size (%d) passed to register to register move\n", mov_size);
		 return (NULL);
	}
        return(tgt_addr);
}

/* =======================================================================
 * MOV reg, imm — load an immediate constant into a register
 *
 * x86 has a compact "MOV r, imm" form: opcode is 0xB0+reg (8-bit) or
 * 0xB8+reg (16/32/64-bit).  The immediate follows the opcode directly
 * — no ModR/M needed.
 * ======================================================================= */
static inline volatile char *build_mov_imm_to_register(short mov_size, long imm_value, int dest_reg, volatile char *tgt_addr)
{
	switch (mov_size) {
	case 1:
		/* B0+rd ib — 2 bytes total */
		(*(short *) tgt_addr) = imm_value << 8 | (0xb0 + dest_reg);
		tgt_addr += BYTE2_OFF;
		break;
	case 2:
		/* 66 B8+rd iw — pack prefix + opcode + 16-bit imm into one write */
		(*(int *) tgt_addr) = (imm_value << 16) | ((0xb8 + dest_reg) << 8) | PREFIX_16BIT;
		tgt_addr += BYTE4_OFF;
		break;
	case 4:
		/* B8+rd id — opcode then 32-bit immediate */
		(*tgt_addr++) = 0xb8 + dest_reg;
		(*(int *) tgt_addr) = imm_value;
		tgt_addr += BYTE4_OFF;
		break;
	case 8:
		/* REX.W B8+rd io — opcode then full 64-bit immediate (10 bytes total) */
		(*tgt_addr++) = (REX_PREFIX | REX_W);
		(*tgt_addr++) = 0xb8 + dest_reg;
		(*(long *) tgt_addr) = imm_value;
		tgt_addr += ISZ_8;
		break;
	default:
		fprintf(stderr, "ERROR: Incorrect size (%d) passed to immediate to register move\n", mov_size);
		return (NULL);
	}
	return (tgt_addr);
}

/* =======================================================================
 * MOV reg, [base_reg + disp] — load from memory into a register
 *
 * The Mod field in ModR/M selects how the displacement is encoded:
 *   Mod=00: no displacement   → just [base_reg]
 *   Mod=01: 8-bit disp8       → [base_reg + disp8]
 *   Mod=10: 32-bit disp32     → [base_reg + disp32]
 *
 * We always use EBX (kept in RBX) as the base register, so all loads
 * stay anchored to the shared data area.
 * ======================================================================= */
static inline volatile char *build_mov_memory_to_register(short mov_size, int dest_reg, int base_reg, long displacement, volatile char *tgt_addr)
{
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;
	if (mov_size == 8)
		(*tgt_addr++) = (REX_PREFIX | REX_W);

	/* Select the load opcode based on width */
	switch (mov_size) {
	case 1:  (*tgt_addr++) = 0x8a; break;
	case 2:
	case 4:
	case 8:  (*tgt_addr++) = 0x8b; break;
	default:
		fprintf(stderr, "ERROR: Incorrect size (%d) passed to memory to register move\n", mov_size);
		return (NULL);
	}

	/* Encode ModR/M + optional displacement bytes */
	if (displacement == 0) {
		(*(char *) tgt_addr) = ((0x0 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
	} else if ((displacement >= -128) && (displacement <= 127)) {
		(*(char *) tgt_addr) = ((0x1 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(char *) tgt_addr) = (char)displacement;
		tgt_addr += BYTE1_OFF;
	} else {
		(*(char *) tgt_addr) = ((0x2 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(int *) tgt_addr) = (int)displacement;
		tgt_addr += BYTE4_OFF;
	}
	return (tgt_addr);
}

/* =======================================================================
 * MOV [base_reg + disp], reg — store a register into memory
 *
 * Mirror image of the load above.  Store opcodes are 0x88 (8-bit) and
 * 0x89 (16/32/64-bit).  The ModR/M Reg field now holds the source register
 * and R/M holds the base register for the memory address.
 * ======================================================================= */
static inline volatile char *build_mov_register_to_memory(short mov_size, int src_reg, int dest_reg, long displacement, volatile char *tgt_addr)
{
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;
	if (mov_size == 8)
		(*tgt_addr++) = (REX_PREFIX | REX_W);

	switch (mov_size) {
	case 1:  (*tgt_addr++) = 0x88; break;
	case 2:
	case 4:
	case 8:  (*tgt_addr++) = 0x89; break;
	default:
		fprintf(stderr, "ERROR: Incorrect size (%d) passed to register to memory move\n", mov_size);
		return (NULL);
	}

	if (displacement == 0) {
		(*(char *) tgt_addr) = ((0x0 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + dest_reg);
		tgt_addr += BYTE1_OFF;
	} else if ((displacement >= -128) && (displacement <= 127)) {
		(*(char *) tgt_addr) = ((0x1 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + dest_reg);
		tgt_addr += BYTE1_OFF;
		(*(char *) tgt_addr) = (char)displacement;
		tgt_addr += BYTE1_OFF;
	} else {
		(*(char *) tgt_addr) = ((0x2 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + dest_reg);
		tgt_addr += BYTE1_OFF;
		(*(int *) tgt_addr) = (int)displacement;
		tgt_addr += BYTE4_OFF;
	}
	return (tgt_addr);
}

/* =======================================================================
 * XADD [base_reg + disp], reg — atomic exchange-and-add
 *
 * XADD does: tmp = [mem]; [mem] = [mem] + reg; reg = tmp
 * It's always the memory form here because LOCK is only valid when one
 * operand is in memory.  The two-byte escape opcode is 0x0F 0xC0/0xC1.
 *
 * Encoding layout:
 *   [LOCK] [0x66] [REX.W] 0x0F 0xC0|0xC1 ModR/M [disp]
 * ======================================================================= */
static inline volatile char *build_xadd(short mov_size, int src_reg, int base_reg, long displacement, int use_lock, volatile char *tgt_addr)
{
	if (use_lock)
		(*tgt_addr++) = LOCK_PREFIX;
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;
	if (mov_size == 8)
		(*tgt_addr++) = (REX_PREFIX | REX_W);

	(*tgt_addr++) = 0x0f;
	(*tgt_addr++) = (mov_size == 1) ? 0xc0 : 0xc1;   /* C0=8-bit, C1=wider */

	/* ModR/M: Reg=src_reg (the register operand), R/M=base_reg (memory) */
	if (displacement == 0) {
		(*(char *) tgt_addr) = ((0x0 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
	} else if ((displacement >= -128) && (displacement <= 127)) {
		(*(char *) tgt_addr) = ((0x1 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(char *) tgt_addr) = (char)displacement;
		tgt_addr += BYTE1_OFF;
	} else {
		(*(char *) tgt_addr) = ((0x2 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(int *) tgt_addr) = (int)displacement;
		tgt_addr += BYTE4_OFF;
	}
	return (tgt_addr);
}

/* =======================================================================
 * XCHG [base_reg + disp], reg — atomic exchange
 *
 * XCHG swaps the register and memory operand atomically.  Note that a
 * memory-form XCHG has an implicit LOCK even without the prefix; we emit
 * it explicitly when use_lock=1 to make the intent clear in the byte stream.
 *
 * Encoding: [LOCK] [0x66] [REX.W] 0x86|0x87 ModR/M [disp]
 *   0x86 = 8-bit,  0x87 = 16/32/64-bit
 * ======================================================================= */
static inline volatile char *build_xchg(short mov_size, int src_reg, int base_reg, long displacement, int use_lock, volatile char *tgt_addr)
{
	if (use_lock)
		(*tgt_addr++) = LOCK_PREFIX;
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;
	if (mov_size == 8)
		(*tgt_addr++) = (REX_PREFIX | REX_W);

	(*tgt_addr++) = (mov_size == 1) ? 0x86 : 0x87;

	if (displacement == 0) {
		(*(char *) tgt_addr) = ((0x0 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
	} else if ((displacement >= -128) && (displacement <= 127)) {
		(*(char *) tgt_addr) = ((0x1 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(char *) tgt_addr) = (char)displacement;
		tgt_addr += BYTE1_OFF;
	} else {
		(*(char *) tgt_addr) = ((0x2 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(int *) tgt_addr) = (int)displacement;
		tgt_addr += BYTE4_OFF;
	}
	return (tgt_addr);
}

/* =======================================================================
 * Memory fence instructions — each is a fixed 3-byte sequence.
 *
 * These serialize memory operations so that all CPUs see stores and loads
 * in the same order.  They are critical for multi-processor validation:
 *
 *   MFENCE (0x0F 0xAE 0xF0) — orders ALL loads and stores
 *   SFENCE (0x0F 0xAE 0xF8) — orders stores only
 *   LFENCE (0x0F 0xAE 0xE8) — orders loads only
 *
 * Reference: Intel SDM Vol. 2, §8.3 Serializing Instructions
 * ======================================================================= */
static inline volatile char *build_mfence(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0x0f;
	(*tgt_addr++) = 0xae;
	(*tgt_addr++) = 0xf0;
	return (tgt_addr);
}

static inline volatile char *build_sfence(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0x0f;
	(*tgt_addr++) = 0xae;
	(*tgt_addr++) = 0xf8;
	return (tgt_addr);
}

static inline volatile char *build_lfence(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0x0f;
	(*tgt_addr++) = 0xae;
	(*tgt_addr++) = 0xe8;
	return (tgt_addr);
}
