/*
 * Description:
 *
 *
 * GENERAL Instruction Format
 *
 * -----------------------------------------------------------------
 * | Instruciton    |   Opcode | ModR/M | Displacement | Immediate |
 * | Prefixe        |          |        |              |           |
 * -----------------------------------------------------------------
 *
 *  7  6  5   3  2   0
 * --------------------
 * | Mod | Reg* | R/M |
 * --------------------
 */

#include <stdio.h>
#include <stdlib.h>
/*
 * definitions we need to support these functions.
 *
 * see Table 2-2 in SDM for register/MODRM encode usage
 *
 *
 */
#define PREFIX_16BIT 0x66
#define BASE_MODRM 0xc0
#define REG_SHIFT 0x3
#define MODRM_SHIFT 0x6
#define RM_SHIFT 0x0
#define REG_MASK 0x7
#define RM_MASK 0x7
#define MOD_MASK 0x3

// register defs based on MOD RM table
#define REG_EAX 0x0
#define REG_ECX 0x1
#define REG_EDX 0x2
#define REG_EBX 0x3
#define REG_ESP 0x4
#define REG_EBP 0x5
#define REG_ESI 0x6
#define REG_EDI 0x7

// register defs based for x86_64, requires REX extension
#define REG_R8 0x0
#define REG_R9 0x1
#define REG_R10 0x2
#define REG_R11 0x3
#define REG_R12 0x4
#define REG_R13 0x5
#define REG_R14 0x6
#define REG_R15 0x7

// byte offset
#define BYTE1_OFF 0x1
#define BYTE2_OFF 0x2
#define BYTE3_OFF 0x3
#define BYTE4_OFF 0x4

// ISIZE (instruction size in bytes, for move example 2byte = 16bit)

#define ISZ_1 0x1
#define ISZ_2 0x2
#define ISZ_4 0x4
#define ISZ_8 0x8

// x86_64 support defines
#define REX_PREFIX 0x40
#define REX_W 0x8
#define REX_R 0x4
#define REX_X 0x2
#define REX_B 0x1

// code generation defines

#define MAX_INSTR_BYTES 10000
#define MAX_DATA_BYTE (10 * 1024) // allocate 10K

/*
 * Function: build_mov_register_to_register
 *
 * Description:
 *
 * Inputs:
 *
 *  short mov_size               :  size of the move being requested
 *  int   src_reg                :  register source encoding
 *  int   dest_reg               :  destintion reg of move
 *  volatile char *tgt_addr      :  starting memory address of where to store instruction
 *
 * Output:
 *
 *  returns adjusted address after encoding instruction
 *
 */

/*
 * Function: build_push_reg
 *
 * Description:
 *
 * builds push register
 *
 * Inputs:
 *
 *  int reg_index                :  index of register
 *  int x86_64f                  :  flag to indicate if we need to extend to rex format
 *  volatile char *tgt_addr      :  starting memory address of where to store instruction
 *
 * Output:
 *
 *  returns adjusted address after encoding instruction
 *
 */

static inline volatile char *build_push_reg(int reg_index, int x86_64f, volatile char *tgt_addr)
{

	if (x86_64f)
	{
		// this is a quick hack for REX_B prefix

		(*(char *)tgt_addr) = (REX_PREFIX | REX_B);
		tgt_addr += BYTE1_OFF;
	}

	(*(char *)tgt_addr) = 0x50 + reg_index;
	;
	tgt_addr += BYTE1_OFF;

	return (tgt_addr);
}

/*
 * Function: build_pop_reg
 *
 * Description:
 *
 * builds pop register
 *
 * Inputs:
 *
 *  int reg_index                :  index of register
 *  int x86_64f                  :  flag to indicate if we need to exted to rex format
 *  volatile char *tgt_addr      :  starting memory address of where to store instruction
 *
 * Output:
 *
 *  returns adjusted address after encoding instruction
 *
 */

static inline volatile char *build_pop_reg(int reg_index, int x86_64f, volatile char *tgt_addr)
{

	if (x86_64f)
	{
		// this is a quick hack for REX_B prefix

		(*(char *)tgt_addr) = (REX_PREFIX | REX_B);
		tgt_addr += BYTE1_OFF;
	}

	(*(char *)tgt_addr) = 0x58 + reg_index;
	;
	tgt_addr += BYTE1_OFF;

	return (tgt_addr);
}

static inline volatile char *build_pusha(volatile char *tgt_addr)
{
	// 32-bit x86 push-all. This is not valid in x86_64 mode.
	(*tgt_addr++) = 0x60;
	return (tgt_addr);
}

static inline volatile char *build_popa(volatile char *tgt_addr)
{
	// 32-bit x86 pop-all. This matches build_pusha and restores the saved registers.
	(*tgt_addr++) = 0x61;
	return (tgt_addr);
}

static inline volatile char *build_mov_register_to_register(short mov_size, int src_reg, int dest_reg, volatile char *tgt_addr)
{
	// for 16 bit mode we need to treat it special because it requires a prefix

	if (mov_size == 2)
	{
		(*tgt_addr++) = PREFIX_16BIT;
	}

	// now lets look at each size and determine which opcode required

	switch (mov_size)
	{

	case 1:
		(*(short *)tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8a;
		tgt_addr += BYTE2_OFF;
		break;

	case 2:
		(*(short *)tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		tgt_addr += BYTE2_OFF;
		break;

	case 4:
		(*(short *)tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		tgt_addr += BYTE2_OFF;
		break;

	case 8:
		(*(char *)tgt_addr) = (REX_PREFIX | REX_W);
		tgt_addr += BYTE1_OFF;
		(*(short *)tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		tgt_addr += BYTE2_OFF;
		break;

	default:
		fprintf(stderr, "ERROR: Incorrect size (%d) passed to register to register move\n", mov_size);
		return (NULL);
	}

	return (tgt_addr);
}

static inline volatile char *build_mov_imm_to_register(short mov_size, long imm_value, int dest_reg, volatile char *tgt_addr)
{
	// for 16 bit mode we need to treat it special because it requires a prefix

	// if (mov_size == 2) {
	// 	(*tgt_addr ++) = PREFIX_16BIT;
	// }

	// now lets look at each size and determine which opcode required

	switch (mov_size)
	{

	case 1:
		(*(short *)tgt_addr) = imm_value << 8 | (0xb0 + dest_reg);
		tgt_addr += BYTE2_OFF;
		break;

	case 2:
		(*(int *)tgt_addr) = (imm_value << 16) | ((0xb8 + dest_reg) << 8) | PREFIX_16BIT;
		tgt_addr += BYTE4_OFF;
		break;

	case 4:
		(*tgt_addr++) = 0xb8 + dest_reg;
		(*(int *)tgt_addr) = imm_value;
		tgt_addr += BYTE4_OFF;
		break;

	case 8:
		// this is a quick hack for REX_W prefix
		(*tgt_addr++) = (REX_PREFIX | REX_W);
		(*tgt_addr++) = 0xb8 + dest_reg;
		(*(long *)tgt_addr) = imm_value;
		tgt_addr += ISZ_8;
		break;

	default:
		fprintf(stderr, "ERROR: Incorrect size (%d) passed to immediate to register move\n", mov_size);
		return (NULL);
	}

	return (tgt_addr);
}

static inline volatile char *build_mov_memory_to_register(short mov_size, int dest_reg, int base_reg, long displacement, volatile char *tgt_addr)
{
	// Build a register load from memory using the base pointer we keep in rbx.
	// We only use the x86_64 form here, so 64-bit moves get a REX.W prefix.

	if (mov_size == 2)
	{
		(*tgt_addr++) = PREFIX_16BIT;
	}

	if (mov_size == 8)
	{
		(*tgt_addr++) = (REX_PREFIX | REX_W);
	}

	switch (mov_size)
	{
	case 1:
		(*tgt_addr++) = 0x8a;
		break;

	case 2:
		(*tgt_addr++) = 0x8b;
		break;

	case 4:
		(*tgt_addr++) = 0x8b;
		break;

	case 8:
		(*tgt_addr++) = 0x8b;
		break;

	default:
		fprintf(stderr, "ERROR: Incorrect size (%d) passed to memory to register move\n", mov_size);
		return (NULL);
	}

	if (displacement == 0)
	{
		(*(char *)tgt_addr) = ((0x0 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
	}
	else if ((displacement >= -128) && (displacement <= 127))
	{
		(*(char *)tgt_addr) = ((0x1 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(char *)tgt_addr) = (char)displacement;
		tgt_addr += BYTE1_OFF;
	}
	else
	{
		(*(char *)tgt_addr) = ((0x2 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(int *)tgt_addr) = (int)displacement;
		tgt_addr += BYTE4_OFF;
	}

	return (tgt_addr);
}

static inline volatile char *build_mov_register_to_memory(short mov_size, int src_reg, int dest_reg, long displacement, volatile char *tgt_addr)
{
	// Store a register through the base pointer in rbx.
	// This keeps the generated loads and stores tied to mdptr.

	if (mov_size == 2)
	{
		(*tgt_addr++) = PREFIX_16BIT;
	}

	if (mov_size == 8)
	{
		(*tgt_addr++) = (REX_PREFIX | REX_W);
	}

	switch (mov_size)
	{
	case 1:
		(*tgt_addr++) = 0x88;
		break;

	case 2:
		(*tgt_addr++) = 0x89;
		break;

	case 4:
		(*tgt_addr++) = 0x89;
		break;

	case 8:
		(*tgt_addr++) = 0x89;
		break;

	default:
		fprintf(stderr, "ERROR: Incorrect size (%d) passed to register to memory move\n", mov_size);
		return (NULL);
	}

	if (displacement == 0)
	{
		(*(char *)tgt_addr) = ((0x0 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + dest_reg);
		tgt_addr += BYTE1_OFF;
	}
	else if ((displacement >= -128) && (displacement <= 127))
	{
		(*(char *)tgt_addr) = ((0x1 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + dest_reg);
		tgt_addr += BYTE1_OFF;
		(*(char *)tgt_addr) = (char)displacement;
		tgt_addr += BYTE1_OFF;
	}
	else
	{
		(*(char *)tgt_addr) = ((0x2 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + dest_reg);
		tgt_addr += BYTE1_OFF;
		(*(int *)tgt_addr) = (int)displacement;
		tgt_addr += BYTE4_OFF;
	}

	return (tgt_addr);
}
