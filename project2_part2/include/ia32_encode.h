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
#define PREFIX_16BIT   0x66
#define BASE_MODRM     0xc0
#define REG_SHIFT      0x3
#define MODRM_SHIFT    0x6
#define RM_SHIFT       0x0
#define REG_MASK       0x7
#define RM_MASK        0x7
#define MOD_MASK       0x3

// register defs based on MOD RM table
#define REG_EAX        0x0
#define REG_ECX        0x1
#define REG_EDX        0x2
#define REG_EBX        0x3
#define REG_ESP        0x4
#define REG_EBP        0x5
#define REG_ESI        0x6
#define REG_EDI        0x7

// register defs based for x86_64, requires REX extension
#define REG_R8        0x0
#define REG_R9        0x1
#define REG_R10       0x2
#define REG_R11       0x3
#define REG_R12       0x4
#define REG_R13       0x5
#define REG_R14       0x6
#define REG_R15       0x7

// byte offset
#define BYTE1_OFF      0x1
#define BYTE2_OFF      0x2
#define BYTE3_OFF      0x3
#define BYTE4_OFF      0x4

// ISIZE (instruction size in bytes, for move example 2byte = 16bit)

#define ISZ_1         0x1
#define ISZ_2         0x2
#define ISZ_4         0x4
#define ISZ_8         0x8

// x86_64 support defines
#define REX_PREFIX    0x40
#define REX_W         0x8
#define REX_R         0x4
#define REX_X         0x2
#define REX_B         0x1

// code generation defines

#define MAX_INSTR_BYTES 10000
#define MAX_DATA_BYTE  (10*1024)  // allocate 10K

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


static inline volatile char *build_mov_register_to_register(short mov_size, int src_reg, int dest_reg, volatile char *tgt_addr)
{
	// for 16 bit mode we need to treat it special because it requires a prefix

	if (mov_size == 2) {
		(*tgt_addr ++) = PREFIX_16BIT;
	}

	// now lets look at each size and determine which opcode required

	switch(mov_size)  {

	case 1: 
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8a;
		 tgt_addr += BYTE2_OFF;
		 break;

	case 2:  // can overload this case because same opcode, but already set prefix
	case 4: 
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		 tgt_addr += BYTE2_OFF;
		 break;

	default:
		 fprintf(stderr,"ERROR: Incorrect size (%d) passed to register to register move\n", mov_size);
		 return (NULL);

	}
			
        return(tgt_addr);
}

static inline volatile char *build_mov_imm_to_register(short mov_size, int imm_value, int dest_reg, volatile char *tgt_addr)
{
	// for 16 bit mode we need to treat it special because it requires a prefix

	// if (mov_size == 2) {
	// 	(*tgt_addr ++) = PREFIX_16BIT;
	// }

	// now lets look at each size and determine which opcode required

	switch(mov_size)  {

	case 1: 
		(*(short *) tgt_addr) = imm_value << 8 | (0xb0 + dest_reg);
		 tgt_addr += BYTE2_OFF;
		 break;

	case 2:  
        (*(int *) tgt_addr) = (imm_value << 16) | ((0xb8 + dest_reg) << 8) | PREFIX_16BIT;
		 tgt_addr += BYTE4_OFF;
		 break;

	case 4: 
        (*tgt_addr ++) = 0xb8 + dest_reg;
		(*(int *) tgt_addr) = imm_value;
		 tgt_addr += BYTE4_OFF;
		 break;

	default:
		 fprintf(stderr,"ERROR: Incorrect size (%d) passed to immediate to register move\n", mov_size);
		 return (NULL);

	}
			
        return(tgt_addr);
}

static inline volatile char *build_mov_register_to_memory(short mov_size, int src_reg, int dest_reg, long displacement, volatile char *tgt_addr)
{
	// for 16 bit mode we need to treat it special because it requires a prefix
	int mod;

	if (displacement == 0) {
		mod = 0x00;
	}
	else if(displacement <= 127 && displacement >= -128) {
		mod = 0x01;
	}
	else {
		mod = 0x02;
	}


	if (mov_size == 2) {
        (*tgt_addr++) = PREFIX_16BIT;
    }

    switch(mov_size) {

    case 1:
        (*(short *) tgt_addr) = ((mod << MODRM_SHIFT) +(src_reg << REG_SHIFT) + dest_reg) << 8 | 0x88;
        tgt_addr += BYTE2_OFF;
        break;

    case 2:
        (*(short *) tgt_addr) = ((mod << MODRM_SHIFT) +(src_reg << REG_SHIFT) + dest_reg) << 8 | 0x89;
        tgt_addr += BYTE2_OFF;
        break;

    case 4:
        (*(short *) tgt_addr) = ((mod << MODRM_SHIFT) +(src_reg << REG_SHIFT) + dest_reg) << 8 | 0x89;
        tgt_addr += BYTE2_OFF;
        break;

    default:
        fprintf(stderr,"ERROR: Incorrect size (%d) passed to register to memory move\n", mov_size);
        return (NULL);
    }

	if(mod == 0x01) {
		(*(char *) tgt_addr) = (char) displacement;
		tgt_addr += BYTE1_OFF;
	}
	else if(mod == 0x02) {
		(*(int *) tgt_addr) = (int) displacement;
		tgt_addr += BYTE4_OFF;
	}
    return(tgt_addr);
}



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

	if (x86_64f) {
		// this is a quick hack for REX_B prefix

		(*(char *) tgt_addr)=(REX_PREFIX | REX_B);
		tgt_addr += BYTE1_OFF;
	}

	(*(char *) tgt_addr) = 0x50+reg_index;;
	tgt_addr += BYTE1_OFF;
			
        return(tgt_addr);
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

	if (x86_64f) {
		// this is a quick hack for REX_B prefix

		(*(char *) tgt_addr)=(REX_PREFIX | REX_B);
		tgt_addr += BYTE1_OFF;
	}

	(*(char *) tgt_addr) = 0x58+reg_index;;
	tgt_addr += BYTE1_OFF;
			
        return(tgt_addr);
}

static inline volatile char *build_enter(short frame_size, char level, volatile char *tgt_addr) 
{
	(*(char *)tgt_addr) = 0xC8;
	tgt_addr += BYTE1_OFF;
	
	(*(short *) tgt_addr) = frame_size;
	tgt_addr += BYTE2_OFF;
	
	(*(char *) tgt_addr) = level;
	tgt_addr += BYTE1_OFF;

	return (tgt_addr);
}

static inline volatile char *build_leave(volatile char *tgt_addr) 
{
	(*(char *) tgt_addr) = 0xC9;
	tgt_addr += BYTE1_OFF;

	return (tgt_addr);
}

static inline volatile char *build_ret(volatile char *tgt_addr) 
{
	(*(char *) tgt_addr) = 0xC3;
	tgt_addr += BYTE1_OFF;

	return (tgt_addr);
}

static inline volatile char *build_mov_memory_to_register(short mov_size, int src_reg, long displacement, int dest_reg, volatile char *tgt_addr)
{
	int mod;

	if(mov_size == 2) {
		(*(char *) tgt_addr) = 0x66;
		tgt_addr += BYTE1_OFF;
	}

		(*(char *) tgt_addr) = (mov_size ==1) ? 0x8A : 0x8B;
		tgt_addr += BYTE1_OFF;
	

	if(displacement == 0) {
		mod = 0x00;
	}
	else if (displacement <= 127 && displacement >= -128) {
		mod = 0x01;
	}
	else {
		mod = 0x02;
	}

	(*(char *) tgt_addr) = ((mod << 6) | (dest_reg << 3) | src_reg);
	tgt_addr += BYTE1_OFF;

	if(mod == 0x01) {
		(*(char *) tgt_addr) = displacement;
		tgt_addr += BYTE1_OFF;
	}
	else if (mod == 0x02) {
		(*(int *) tgt_addr) = displacement;
		tgt_addr += BYTE4_OFF;
	}

	return tgt_addr;
}