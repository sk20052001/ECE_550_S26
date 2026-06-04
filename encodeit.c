
//
// simple encoding example fr IA-32 validation project
//
// project_part2
// 
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>

#include "ia32_encode.h"


#ifndef PAGESIZE
#define PAGESIZE 4096
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

// globals to aid debug to start
volatile char *mptr=0,*next_ptr=0, *mdptr=0;
int num_inst=0;
int inst_goal=25;
unsigned int inst_seed=1;

// declarations for starting test
typedef int (*funct_t)();
funct_t start_test;
int build_instructions();
int executeit();

static inline int rand_range(int min_value, int max_value);
static inline int pick_instruction_kind(void);
static inline short pick_mov_size(void);
static inline int pick_general_reg(short mov_size);
static inline int pick_source_reg(short mov_size);
static inline int pick_dest_reg(short mov_size, int source_reg);
static inline long pick_displacement(void);
static inline long pick_immediate(short mov_size);
static inline void emit_random_instruction(void);

int main(int argc, char *argv[])
{

	int ibuilt=0,rc=0;

	if (argc > 1)
	{
		inst_seed = (unsigned int)strtoul(argv[1], NULL, 0);
	}

	if (argc > 2)
	{
		inst_goal = atoi(argv[2]);
	}

	if (inst_goal < 4)
	{
		inst_goal = 4;
	}

	srand(inst_seed);

	/* allocate buffer to perform stores and loads to, and set permissions  */

	mdptr = (volatile char *)mmap(
		(void *) 0,
		(MAX_DATA_BYTE+PAGESIZE-1),
		PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_SHARED,
		0, 0
		);

	if(mdptr == MAP_FAILED) { 
		printf("data mptr allocation failed\n"); 
		exit(1); 
	}


	/* allocate buffer to build instructions into, and set permissions to allow execution of this memory area */

	mptr = (volatile char *)mmap(
		(void *) 0,
		(MAX_INSTR_BYTES+PAGESIZE-1),
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_ANONYMOUS | MAP_SHARED,
		0, 0
		);

	if(mptr == MAP_FAILED) { 
		printf("instr  mptr allocation failed\n"); 
		exit(1); 
	}

	next_ptr=mptr;                  // init next_ptr

	ibuilt=build_instructions();  	// build instructions

	/* ok now that I built the critters, time to execute them */

	start_test=(funct_t) mptr;
	executeit(start_test);


	fprintf(stderr,"generation program complete, %d instructions generated, and executed\n",ibuilt);

	// clean up the allocation before getting out

	munmap((caddr_t)mdptr,(MAX_DATA_BYTE+PAGESIZE-1));
	munmap((caddr_t)mptr,(MAX_INSTR_BYTES+PAGESIZE-1));


}

//
// Routine:  rand_range
//
// Description:
//
// Return a random integer within an inclusive range.
//
static inline int rand_range(int min_value, int max_value)
{
	return min_value + (rand() % (max_value - min_value + 1));
}

//
// Routine:  pick_instruction_kind
//
// Description:
//
// Pick one of the instruction families the professor asked for.
//
static inline int pick_instruction_kind(void)
{
	return rand_range(0, 3);
}

//
// Routine:  pick_mov_size
//
// Description:
//
// Pick a legal operand width for the generator.
//
static inline short pick_mov_size(void)
{
	static const short sizes[] = { ISZ_1, ISZ_2, ISZ_4, ISZ_8 };
	return sizes[rand_range(0, 3)];
}

//
// Routine:  pick_general_reg
//
// Description:
//
// Pick a general-purpose register but keep the base pointer register out of the
// random pool so memory addressing stays stable.
//
static inline int pick_general_reg(short mov_size)
{
	static const int regs_word_dword_qword[] = { REG_EAX, REG_ECX, REG_EDX, REG_ESI, REG_EDI };
	static const int regs_byte[] = { REG_EAX, REG_ECX, REG_EDX, REG_ESI };

	if (mov_size == ISZ_1)
	{
		return regs_byte[rand_range(0, 3)];
	}

	return regs_word_dword_qword[rand_range(0, 4)];
}

//
// Routine:  pick_source_reg
//
// Description:
//
// Pick a source register for a randomized instruction.
//
static inline int pick_source_reg(short mov_size)
{
	return pick_general_reg(mov_size);
}

//
// Routine:  pick_dest_reg
//
// Description:
//
// Pick a destination register that is different from the source register.
//
static inline int pick_dest_reg(short mov_size, int source_reg)
{
	int dest_reg = pick_general_reg(mov_size);

	while (dest_reg == source_reg)
	{
		dest_reg = pick_general_reg(mov_size);
	}

	return dest_reg;
}

//
// Routine:  pick_displacement
//
// Description:
//
// Pick one of the professor-specified displacement choices.
//
static inline long pick_displacement(void)
{
	static const long disps[] = { 0, 8, 32 };
	return disps[rand_range(0, 2)];
}

//
// Routine:  pick_immediate
//
// Description:
//
// Pick a random immediate that fits the selected instruction width.
//
static inline long pick_immediate(short mov_size)
{
	switch (mov_size)
	{
	case ISZ_1:
		return rand_range(0x01, 0x7f);
	case ISZ_2:
		return rand_range(0x0100, 0x7fff);
	case ISZ_4:
		return (long)(((unsigned long long)rand() << 16) ^ (unsigned long long)rand());
	case ISZ_8:
	default:
		return (long)((((unsigned long long)rand() << 48) ^
			((unsigned long long)rand() << 32) ^
			((unsigned long long)rand() << 16) ^
			(unsigned long long)rand()));
	}
}

//
// Routine:  emit_random_instruction
//
// Description:
//
// Emit one randomized instruction from the required set.
//
static inline void emit_random_instruction(void)
{
	int kind = pick_instruction_kind();
	short mov_size = pick_mov_size();
	int src_reg = pick_source_reg(mov_size);
	int dest_reg = pick_dest_reg(mov_size, src_reg);
	long displacement = pick_displacement();
	long imm_value = pick_immediate(mov_size);

	switch (kind)
	{
	case 0:
		next_ptr = build_mov_imm_to_register(mov_size, imm_value, dest_reg, next_ptr);
		break;

	case 1:
		next_ptr = build_mov_register_to_register(mov_size, src_reg, dest_reg, next_ptr);
		break;

	case 2:
		next_ptr = build_mov_register_to_memory(mov_size, src_reg, REG_EBX, displacement, next_ptr);
		break;

	case 3:
	default:
		next_ptr = build_mov_memory_to_register(mov_size, dest_reg, REG_EBX, displacement, next_ptr);
		break;
	}

	num_inst++;
	fprintf(stderr, "next ptr is now 0x%lx\n", (long) next_ptr);
}

/*
 * Function: executeit
 * 
 * Description:
 *
 * This function will start executing at the function address passed into it 
 * and return an integer return value that will be used to indicate pass(0)/fail(1)
 *
 * INTPUTs:  funct_t start_addr :      function pointer 
 *
 * Returns:  int                :      0 for pass, 1 for fail
 */   
int executeit(funct_t start_addr) 
{

	volatile int i,rc=0;

	i=0;

	rc=(*start_addr)();

	return(0);
}

//
// Routine:  add_headeri
//
// Description:
//
// Build the prologue for the generated code so it behaves like a real
// callable function.
//
static inline volatile char *add_headeri(volatile char *tgt_addr)
{
#if defined(__x86_64__) || defined(_M_X64)
	// Build a real x86_64 stack frame first so the generated code can return cleanly.
	(*tgt_addr++) = 0xc8;                         // enter
	(*(short *)tgt_addr) = 2048;                  // stack size
	tgt_addr += BYTE2_OFF;
	(*tgt_addr++) = 0x00;                         // nesting level

	// Save the callee-saved registers we may rely on inside the generated block.
	(*tgt_addr++) = 0x53;                         // push rbx
	tgt_addr = build_push_reg(REG_R12, 1, tgt_addr);
	tgt_addr = build_push_reg(REG_R13, 1, tgt_addr);
	tgt_addr = build_push_reg(REG_R14, 1, tgt_addr);
	tgt_addr = build_push_reg(REG_R15, 1, tgt_addr);

	// Keep the data base pointer in rbx so load/store helpers have a stable anchor.
	tgt_addr = build_mov_imm_to_register(ISZ_8, (long)mdptr, REG_EBX, tgt_addr);
#else
	// On 32-bit x86, save everything up front and use ebx as the data base pointer.
	(*tgt_addr++) = 0xc8;                         // enter
	(*(short *)tgt_addr) = 2048;                  // stack size
	tgt_addr += BYTE2_OFF;
	(*tgt_addr++) = 0x00;                         // nesting level

	tgt_addr = build_pusha(tgt_addr);
	tgt_addr = build_mov_imm_to_register(ISZ_4, (long)mdptr, REG_EBX, tgt_addr);
#endif

	return (tgt_addr);
}

//
// Routine:  add_endi
//
// Description:
//
// Build the epilogue for the generated code so it unwinds and returns cleanly
// to the caller.
//
static inline volatile char *add_endi(volatile char *tgt_addr)
{
#if defined(__x86_64__) || defined(_M_X64)
	// Restore the registers in reverse order, then unwind the stack frame and return.
	tgt_addr = build_pop_reg(REG_R15, 1, tgt_addr);
	tgt_addr = build_pop_reg(REG_R14, 1, tgt_addr);
	tgt_addr = build_pop_reg(REG_R13, 1, tgt_addr);
	tgt_addr = build_pop_reg(REG_R12, 1, tgt_addr);
	(*tgt_addr++) = 0x5b;                         // pop rbx

	(*tgt_addr++) = 0xc9;                         // leave
	(*tgt_addr++) = 0xc3;                         // ret
#else
	// 32-bit x86 uses popa to match the push-all prologue.
	tgt_addr = build_popa(tgt_addr);
	(*tgt_addr++) = 0xc9;                         // leave
	(*tgt_addr++) = 0xc3;                         // ret
#endif

	return (tgt_addr);
}

//
// Routine:  build_instructions
//
// Description:
//
// INPUT: none yet
// 
// OUTPUT: returns the number of instructions built
// 
int build_instructions() {

	// Start with the standard function-style prologue.
	next_ptr = add_headeri(next_ptr);

	// Make sure the required instruction families appear, then randomize the rest.
	emit_random_instruction(); // imm -> reg
	emit_random_instruction(); // reg -> reg
	emit_random_instruction(); // reg -> memory
	emit_random_instruction(); // memory -> reg

	while (num_inst < inst_goal)
	{
		emit_random_instruction();
	}

	// Finish with the matching epilogue so the generated block returns cleanly.
	next_ptr = add_endi(next_ptr);

	return (num_inst);
}
