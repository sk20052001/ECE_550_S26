
//
// simple encoding examples for IA-32 validtion project
//

#include <stdio.h>
#include <stdlib.h>
#include "ia32_encode.h"

// globals to aid debug to start
volatile char *mptr=0,*next_ptr=0;
int num_inst=0;

main(int argc, char *argv[])
{

	int ibuilt=0;


	/* allocate buffer to build instructions into */

	mptr=malloc(MAX_INSTR_BYTES);
	next_ptr=mptr;                  // init next_ptr

	ibuilt=build_instructions();  	// build instructions

	fprintf(stderr,"generation program complete, instructions generated: %d\n",ibuilt);
}

//
// Routine:  build_instructions
//
// Description:
//
// INPUT: none yet
// 
// OUTPUT: return the number of instructions built
// 
int build_instructions() {

	// example instruction generation..


    // Register to Register
    // test move cl into bl
    next_ptr=build_mov_register_to_register(ISZ_1, REG_ECX, REG_EBX, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


	// test move cx into bx
	next_ptr=build_mov_register_to_register(ISZ_2, REG_ECX, REG_EBX, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


	// test move edx into edi
	next_ptr=build_mov_register_to_register(ISZ_4, REG_EDX, REG_EDI, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


    // Immediate to Register
    // test move 0x34 into al
    next_ptr=build_mov_imm_to_register(ISZ_1, 0x34, REG_EAX, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


	// test move 0x3456 into bx
	next_ptr=build_mov_imm_to_register(ISZ_2, 0x3456, REG_EBX, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


	// test move 0x23456789 into edi
	next_ptr=build_mov_imm_to_register(ISZ_4, 0x23456789, REG_EDI, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


    // Register to Memory
    // test move dl into [al]
    next_ptr=build_mov_register_to_memory(ISZ_1, REG_EDX, REG_EAX, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


	// test move cx into [si]
	next_ptr=build_mov_register_to_memory(ISZ_2, REG_ECX, REG_ESI, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);


	// test move eax into [edi]
	next_ptr=build_mov_register_to_memory(ISZ_4, REG_EAX, REG_EDI, next_ptr);
	num_inst++;

	fprintf(stderr,"next ptr is now 0x%lx\n", (long) next_ptr);

	return (num_inst);
}
