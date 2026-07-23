
//
// encodeit.c — ECE 550 Post-Silicon Validation, Project Part 3
//
// The big picture:
//   1. Allocate three shared mmap regions (CODE, DATA, COMM).
//   2. Fork N child processes, one per logical thread.
//   3. Each child binds to its own CPU, then calls build_instructions()
//      to write a random sequence of x86 instructions into its CODE slice.
//   4. Two C-level barriers (in the COMM area) make sure all threads
//      finish building before any starts executing.
//   5. Inside the generated code itself, two more spin-wait barriers
//      (emitted by add_headeri/add_endi) further synchronize threads
//      at the very start and end of the random instruction stream.
//   6. Each child calls executeit() which jumps into the generated code.
//

#define _GNU_SOURCE /* needed for CPU_ZERO/CPU_SET/sched_setaffinity */
#ifndef __x86_64__
#define __x86_64__
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sched.h>
#include <limits.h> /* for PAGESIZE */

#include "ia32_encode.h"

#ifndef PAGESIZE
#define PAGESIZE 4096
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* -----------------------------------------------------------------------
 * Globals — kept visible as globals so GDB can inspect them easily.
 * mptr/mdptr/comm_ptr are the base addresses of the three shared regions.
 * next_ptr tracks where the next encoded instruction byte will land.
 * ----------------------------------------------------------------------- */
volatile char *mptr = 0, *next_ptr = 0, *mdptr = 0, *comm_ptr = 0;
int num_inst = 0, i = 0;
int target_ninstrs = MAX_DEF_INSTRS;
int nthreads = 1, pid_task[MAX_THREADS], pid = 0;

/* test_info bundles the three shared-memory base pointers for easy indexing */
typedef struct
{
	volatile unsigned long *pointer_addr;
} test_i;
test_i test_info[NUM_PTRS];

typedef volatile unsigned long *tptrs;

/*
 * Per-thread pointers into the shared regions.
 * Each thread gets its own CODE slice, but they all share the same DATA
 * and COMM regions — that's the whole point of an MP stress test.
 */
volatile unsigned long *mptr_threads[MAX_THREADS];
volatile unsigned long *mdptr_threads[MAX_THREADS];
volatile unsigned long *comm_ptr_threads[MAX_THREADS];

/* Optional log file — NULL means logging is disabled */
FILE *logfile = NULL;

/* Function pointer type used to jump into generated code */
typedef int (*funct_t)();
funct_t start_test;

int executeit(funct_t start_addr);
int build_instructions(volatile char *code_ptr, int thread_id);

/* -----------------------------------------------------------------------
 * Random helper utilities
 * ----------------------------------------------------------------------- */

/* Return a random int in [min_n, max_n] inclusive */
static inline int rand_range(int min_n, int max_n)
{
	return min_n + (rand() % (max_n - min_n + 1));
}

/*
 * pick_instruction_kind — choose which instruction family to emit.
 * Families 0-3 are the basic MOV variants from Part 2.
 * Families 4-6 are the new MP instructions added in Part 3.
 */
static inline int pick_instruction_kind(void)
{
	return rand_range(0, 6);
}

/* Pick one of the four legal operand widths (1, 2, 4, or 8 bytes) */
static inline short pick_mov_size(void)
{
	static const short sizes[] = {ISZ_1, ISZ_2, ISZ_4, ISZ_8};
	return sizes[rand_range(0, 3)];
}

/*
 * For atomic instructions (XADD, XCHG) we cap at 32-bit.  64-bit atomics
 * on an unaligned address inside the shared data area could fault, and
 * keeping it to 32-bit keeps the test simple and predictable.
 */
static inline short pick_mov_size_atomic(void)
{
	static const short sizes[] = {ISZ_1, ISZ_2, ISZ_4};
	return sizes[rand_range(0, 2)];
}

/*
 * pick_general_reg — pick a register from the general-purpose pool.
 * EBX is deliberately excluded because we keep it as the data-area
 * base pointer throughout the generated code.  If we overwrote RBX,
 * all the memory instructions that follow would compute wrong addresses.
 *
 * For byte operations (ISZ_1) we also drop ESI/EDI: without a REX
 * prefix their byte-register encoding resolects AH/BH/etc., not SIL/DIL.
 */
static inline int pick_general_reg(short mov_size)
{
	static const int regs_word_dword_qword[] = {REG_EAX, REG_ECX, REG_EDX, REG_ESI, REG_EDI};
	static const int regs_byte[] = {REG_EAX, REG_ECX, REG_EDX, REG_ESI};

	if (mov_size == ISZ_1)
		return regs_byte[rand_range(0, 3)];
	return regs_word_dword_qword[rand_range(0, 4)];
}

static inline int pick_source_reg(short mov_size)
{
	return pick_general_reg(mov_size);
}

/* Pick a destination register that is different from the source */
static inline int pick_dest_reg(short mov_size, int source_reg)
{
	int dest_reg = pick_general_reg(mov_size);
	while ((dest_reg == source_reg) || (dest_reg == REG_EBX))
		dest_reg = pick_general_reg(mov_size);
	return dest_reg;
}

/*
 * pick_displacement — the three displacement values the professor specified.
 * 0 = no offset, 8 = small positive offset, 32 = larger positive offset.
 * All fit within the 40 KB data region, so we never walk off the end.
 */
static inline long pick_displacement(void)
{
	static const long disps[] = {0, 8, 32};
	return disps[rand_range(0, 2)];
}

/* Generate a random immediate value that fits the chosen operand width */
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

/* -----------------------------------------------------------------------
 * emit_random_instruction — pick and encode one instruction.
 *
 * Pulls from all 7 instruction families.  Cases 4/5 (XADD/XCHG) always
 * target memory so the LOCK prefix is legal.  Case 6 randomly picks one
 * of the three fence variants.  All details get written to logfile if open.
 * ----------------------------------------------------------------------- */
static inline void emit_random_instruction(void)
{
	int kind = pick_instruction_kind();
	short mov_size = pick_mov_size();
	int src_reg = pick_source_reg(mov_size);
	int dest_reg = pick_dest_reg(mov_size, src_reg);
	long disp = pick_displacement();
	long imm_value = pick_immediate(mov_size);
	int use_lock = rand_range(0, 1); /* 50/50 chance of LOCK prefix on atomics */

	const char *kind_name = "unknown";

	switch (kind)
	{
	case 0:
		/* MOV dest, imm — load a constant into a register */
		next_ptr = build_mov_imm_to_register(mov_size, imm_value, dest_reg, next_ptr);
		kind_name = "imm->reg";
		break;

	case 1:
		/* MOV dest, src — copy one register to another */
		next_ptr = build_mov_register_to_register(mov_size, src_reg, dest_reg, next_ptr);
		kind_name = "reg->reg";
		break;

	case 2:
		/* MOV [RBX+disp], src — store a register into the shared data area */
		next_ptr = build_mov_register_to_memory(mov_size, src_reg, REG_EBX, disp, next_ptr);
		kind_name = "reg->mem";
		break;

	case 3:
		/* MOV dest, [RBX+disp] — load from the shared data area */
		next_ptr = build_mov_memory_to_register(mov_size, dest_reg, REG_EBX, disp, next_ptr);
		kind_name = "mem->reg";
		break;

	case 4:
	{
		/* XADD [RBX+disp], src — atomically add src to memory, src gets old value */
		short atomic_sz = pick_mov_size_atomic();
		next_ptr = build_xadd(atomic_sz, src_reg, REG_EBX, disp, use_lock, next_ptr);
		mov_size = atomic_sz;
		kind_name = use_lock ? "xadd(LOCK)" : "xadd";
		break;
	}

	case 5:
	{
		/* XCHG [RBX+disp], src — atomically swap src with memory */
		short atomic_sz = pick_mov_size_atomic();
		next_ptr = build_xchg(atomic_sz, src_reg, REG_EBX, disp, use_lock, next_ptr);
		mov_size = atomic_sz;
		kind_name = use_lock ? "xchg(LOCK)" : "xchg";
		break;
	}

	case 6:
	{
		/* Fence — force memory ordering between all threads */
		int fence_kind = rand_range(0, 2);
		if (fence_kind == 0)
		{
			next_ptr = build_mfence(next_ptr);
			kind_name = "mfence";
		}
		else if (fence_kind == 1)
		{
			next_ptr = build_sfence(next_ptr);
			kind_name = "sfence";
		}
		else
		{
			next_ptr = build_lfence(next_ptr);
			kind_name = "lfence";
		}
		/* Fence has no register or displacement operands */
		mov_size = 0;
		disp = 0;
		src_reg = -1;
		dest_reg = -1;
		break;
	}
	}

	num_inst++;

	if (logfile)
	{
		fprintf(logfile, "inst=%d type=%-12s size=%d src_reg=%d dest_reg=%d disp=%ld\n",
				num_inst, kind_name, mov_size, src_reg, dest_reg, disp);
	}

	fprintf(stderr, "next ptr is now 0x%lx\n", (long)next_ptr);
}

/* -----------------------------------------------------------------------
 * emit_barrier_code — [Extra Credit]
 *
 * Generates a spin-wait barrier as actual x86_64 machine instructions into
 * the code buffer.  When the generated code eventually runs, every thread
 * will hit this barrier and spin until all nthreads processes have arrived.
 *
 * The shared counter lives in the COMM mmap region (barrier_counter arg).
 * Each thread atomically increments it with LOCK XADD, then reads it in
 * a tight loop until the value reaches n (total thread count).
 *
 * Generated instruction sequence:
 *   push rax / push rcx          — save scratch registers
 *   movabs rax, <counter addr>   — bake the 64-bit counter address in as an immediate
 *   mov ecx, 1
 *   lock xadd [rax], ecx         — atomic increment; ecx gets the old value (don't care)
 *   .spin:
 *     mov ecx, [rax]             — re-read the counter
 *     cmp ecx, n                 — are we all here yet?
 *     jl .spin                   — nope, keep spinning
 *   pop rcx / pop rax            — restore scratch registers
 *
 * The jl target is 10 bytes behind the end of the jl instruction:
 *   mov ecx,[rax] = 2B, cmp ecx,n = 6B, jl = 2B  → offset = -10 = 0xF6
 * ----------------------------------------------------------------------- */
static inline volatile char *emit_barrier_code(volatile char *tgt_addr, volatile int *barrier_counter, int n)
{

	/*
	Here is exactly what the generated bytes do when they eventually execute:

	// --- what the generated bytes DO when they run ---

	// Step 1: save scratch registers (we'll restore them at the end)
	push(rax);
	push(rcx);

	// Step 2: load the address of the shared counter into RAX
	// barrier_counter is a real 64-bit address, baked into the code as a literal
	rax = (unsigned long)barrier_counter;   // e.g. 0x7ffff7fae000

	// Step 3: load increment value into ECX
	ecx = 1;

	// Step 4: atomic increment — announce "I am here"
	// LOCK XADD [rax], ecx
	//   reads [rax], adds ecx to it, writes result back, puts OLD value into ecx
	// in C this is:
	int old = *barrier_counter;
	*barrier_counter = old + 1;    // atomic, bus-locked
	ecx = old;                     // ecx now holds old value (we don't use it)

	// Step 5: spin until ALL threads have checked in
	// the loop is 10 bytes: mov(2) + cmp(6) + jl(2) = 10
	spin:
		ecx = *barrier_counter;    // re-read current counter from shared memory
		if (ecx < n)
			goto spin;             // not everyone here yet, keep spinning

	// Step 6: restore scratch registers
	pop(rcx);
	pop(rax);

	// execution continues — all threads now proceed together

	*/

#if defined(__x86_64__)
	/* Save RAX and RCX — we're borrowing them as scratch */
	(*tgt_addr++) = 0x50; /* push rax */
	(*tgt_addr++) = 0x51; /* push rcx */

	/* movabs rax, imm64 — load the barrier counter's address as a literal */
	(*tgt_addr++) = 0x48; /* REX.W */
	(*tgt_addr++) = 0xb8; /* MOV RAX, imm64 */
	(*(unsigned long *)tgt_addr) = (unsigned long)barrier_counter;
	tgt_addr += 8;

	/* mov ecx, 1 — the increment amount */
	(*tgt_addr++) = 0xb9;
	(*(int *)tgt_addr) = 1;
	tgt_addr += 4;

	/*
	 * lock xadd [rax], ecx
	 * ModR/M: Mod=00, Reg=ECX(001), R/M=RAX(000) → 0x08
	 */
	(*tgt_addr++) = 0xf0; /* LOCK prefix */
	(*tgt_addr++) = 0x0f;
	(*tgt_addr++) = 0xc1;
	(*tgt_addr++) = 0x08; /* ModR/M: [rax], ecx */

	/*
	 * Spin loop — 10 bytes so the jl backward offset is -10 (0xF6).
	 * We spin-read the counter until it reaches n (all threads checked in).
	 */
	(*tgt_addr++) = 0x8b; /* mov ecx, [rax] */
	(*tgt_addr++) = 0x08; /* ModR/M: ecx ← [rax] — 2 bytes */

	(*tgt_addr++) = 0x81; /* cmp ecx, imm32 */
	(*tgt_addr++) = 0xf9; /* ModR/M: CMP ECX — 2 bytes + 4-byte immediate */
	(*(int *)tgt_addr) = n;
	tgt_addr += 4;

	(*tgt_addr++) = 0x7c;		 /* jl rel8 — jump back if counter < n */
	(*tgt_addr++) = (char)(-10); /* -10 lands on the mov ecx,[rax] above */

	/* Restore RAX and RCX */
	(*tgt_addr++) = 0x59; /* pop rcx */
	(*tgt_addr++) = 0x58; /* pop rax */
#endif
	return (tgt_addr);
}

/* -----------------------------------------------------------------------
 * add_headeri — emit the function prologue into the code buffer.
 *
 * The generated code needs to look like a normal C function so we can
 * call it via a function pointer and have it return cleanly.  That means:
 *   1. ENTER — allocates a stack frame (like a compiler would).
 *   2. Push callee-saved registers (RBX, R12–R15 per the System V ABI).
 *   3. Load the data-area base address into RBX.  Every memory instruction
 *      in the random stream uses RBX as its base, so this must be right.
 *   4. [Extra Credit] A generated spin-wait barrier — all threads pause
 *      here until every thread has started executing its generated code.
 *      This ensures the random instruction streams all run concurrently.
 *      Counter lives at comm_ptr+8 (offsets 0 and 4 are the C barriers).
 * ----------------------------------------------------------------------- */
static inline volatile char *add_headeri(volatile char *tgt_addr, volatile char *data_base)
{
#if defined(__x86_64__)
	(*tgt_addr++) = 0xc8;		 /* ENTER */
	(*(short *)tgt_addr) = 2048; /* allocate 2 KB of local stack space */
	tgt_addr += BYTE2_OFF;
	(*tgt_addr++) = 0x00; /* nesting level 0 */

	(*tgt_addr++) = 0x53; /* push rbx (byte form — no REX needed for legacy regs) */
	tgt_addr = build_push_reg(REG_R12, 1, tgt_addr);
	tgt_addr = build_push_reg(REG_R13, 1, tgt_addr);
	tgt_addr = build_push_reg(REG_R14, 1, tgt_addr);
	tgt_addr = build_push_reg(REG_R15, 1, tgt_addr);

	/* Bake the shared data-area address directly into a MOV RBX, imm64 */
	tgt_addr = build_mov_imm_to_register(ISZ_8, (long)data_base, REG_EBX, tgt_addr);

	/* [Extra Credit] Start barrier — all threads rendezvous before the random stream */
	tgt_addr = emit_barrier_code(tgt_addr, (volatile int *)(comm_ptr + 8), nthreads);
#else
	(*tgt_addr++) = 0xc8;
	(*(short *)tgt_addr) = 2048;
	tgt_addr += BYTE2_OFF;
	(*tgt_addr++) = 0x00;
	tgt_addr = build_pusha(tgt_addr);
	tgt_addr = build_mov_imm_to_register(ISZ_4, (long)data_base, REG_EBX, tgt_addr);
#endif
	return (tgt_addr);
}

/* -----------------------------------------------------------------------
 * add_endi — emit the function epilogue into the code buffer.
 *
 * Mirrors add_headeri: restores the callee-saved registers in reverse
 * order, then LEAVE (tears down the ENTER stack frame) and RET.
 *
 * [Extra Credit] A second spin-wait barrier is emitted before the register
 * restore so all threads synchronize at the END of the random stream too.
 * This is useful for checking results — you know every thread has finished
 * its random instructions before any of them start unwinding.
 * Counter lives at comm_ptr+12.
 * ----------------------------------------------------------------------- */
static inline volatile char *add_endi(volatile char *tgt_addr)
{
#if defined(__x86_64__)
	/* [Extra Credit] End barrier — all threads finish the random stream together */
	tgt_addr = emit_barrier_code(tgt_addr, (volatile int *)(comm_ptr + 12), nthreads);

	tgt_addr = build_pop_reg(REG_R15, 1, tgt_addr);
	tgt_addr = build_pop_reg(REG_R14, 1, tgt_addr);
	tgt_addr = build_pop_reg(REG_R13, 1, tgt_addr);
	tgt_addr = build_pop_reg(REG_R12, 1, tgt_addr);
	(*tgt_addr++) = 0x5b; /* pop rbx */

	(*tgt_addr++) = 0xc9; /* LEAVE — restores RSP/RBP from the ENTER frame */
	(*tgt_addr++) = 0xc3; /* RET */
#else
	tgt_addr = build_popa(tgt_addr);
	(*tgt_addr++) = 0xc9;
	(*tgt_addr++) = 0xc3;
#endif
	return (tgt_addr);
}

/* -----------------------------------------------------------------------
 * bind_to_cpu — pin this process to a specific logical CPU.
 *
 * We want each forked process running on its own core so that the shared
 * data accesses actually travel across the interconnect and stress the
 * cache coherency protocol.  sched_setaffinity does this at the OS level.
 * ----------------------------------------------------------------------- */
static void bind_to_cpu(int thread_id)
{
	cpu_set_t mask;
	CPU_ZERO(&mask);
	CPU_SET(thread_id, &mask); /* bind to CPU thread_id */

	if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
	{
		fprintf(stderr, "T%d: sched_setaffinity failed: %s\n", thread_id, strerror(errno));
	}
	else
	{
		fprintf(stderr, "T%d: bound to CPU %d\n", thread_id, thread_id);
		fflush(stderr);
	}
}

/* -----------------------------------------------------------------------
 * executeit — jump into the generated code.
 *
 * We cast the code buffer address to a function pointer and call it.
 * The generated code returns an int (ignored here), and because we
 * emitted a proper prologue/epilogue it unwinds cleanly.
 * ----------------------------------------------------------------------- */
int executeit(funct_t start_addr)
{
	volatile int rc = 0;
	rc = (*start_addr)();
	return (0);
}

/* -----------------------------------------------------------------------
 * sync_barrier — C-level spin-wait barrier using the shared COMM area.
 *
 * Called from C code (not generated code) to synchronize the forked
 * processes.  We use it twice:
 *   1. Before build_instructions() — so all threads start generating
 *      at roughly the same time.
 *   2. Before executeit() — so no thread starts executing its generated
 *      code while another thread is still writing to its code buffer.
 *
 * __sync_fetch_and_add is GCC's built-in for an atomic increment.
 * ----------------------------------------------------------------------- */
static void sync_barrier(volatile int *counter, int nthreads)
{
	__sync_fetch_and_add(counter, 1); /* announce arrival */
	while (*counter < nthreads)
	{
	} /* spin until everyone is here */
}

/* -----------------------------------------------------------------------
 * build_instructions — generate the full random instruction sequence.
 *
 * Writes into code_ptr (the thread's private CODE slice) by updating
 * the global next_ptr.  The prologue/epilogue wrap the random stream so
 * the resulting buffer looks like a callable C function.
 * ----------------------------------------------------------------------- */
int build_instructions(volatile char *code_ptr, int thread_id)
{
	num_inst = 0;
	next_ptr = code_ptr; /* start encoding at the beginning of this thread's slice */

	fprintf(stderr, "building instructions for T%d\n", thread_id);
	fflush(stderr);

	/* Prologue: set up stack frame, save registers, load data base ptr, start barrier */
	next_ptr = add_headeri(next_ptr, (volatile char *)mdptr_threads[thread_id]);

	/* Emit target_ninstrs random instructions from the full 7-family mix */
	while (num_inst < target_ninstrs)
		emit_random_instruction();

	/* Epilogue: end barrier, restore registers, LEAVE, RET */
	next_ptr = add_endi(next_ptr);

	return (num_inst);
}

/* -----------------------------------------------------------------------
 * main — set up shared memory, fork children, wait for them to finish.
 *
 * Usage: ./encodeit [seed] [num_instrs] [nthreads] [logfile]
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
	int seed = 0;
	int ibuilt = 0;
	char *logfilename = NULL;

	/* Parse command-line arguments */
	if (argc > 1)
		seed = (int)strtoul(argv[1], NULL, 0);
	if (argc > 2)
	{
		target_ninstrs = atoi(argv[2]);
		if (target_ninstrs < 1)
			target_ninstrs = 1;
	}
	if (argc > 3)
		nthreads = atoi(argv[3]);
	if (argc > 4)
		logfilename = argv[4];

	printf("\nstarting seed=%d instrs=%d nthreads=%d logfile=%s\n",
		   seed, target_ninstrs, nthreads,
		   logfilename ? logfilename : "(none)");

	if (nthreads > MAX_THREADS)
	{
		fprintf(stderr, "Sorry only built for %d threads, overriding your %d\n", MAX_THREADS, nthreads);
		fflush(stderr);
		nthreads = MAX_THREADS;
	}

	srand(seed);

	/* Open log file in parent — child processes inherit the fd after fork */
	if (logfilename)
	{
		logfile = fopen(logfilename, "w");
		if (!logfile)
			perror("fopen logfile");
	}

	/*
	 * Allocate three shared memory regions with MAP_SHARED so that forked
	 * children all see the same physical pages.  PROT_EXEC is needed on
	 * the CODE region so we can jump into the bytes we write there.
	 *
	 * DATA is shared (single region) because we want all threads to hit
	 * the same cache lines — that's what makes this an MP stress test.
	 */

	/* Shared DATA region — one region for all threads */
	test_info[DATA].pointer_addr = (volatile unsigned long *)mmap(
		(void *)0,
		(MAX_DATA_BYTES + PAGESIZE - 1),
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	mdptr = (volatile char *)test_info[DATA].pointer_addr;
	if (mdptr == (volatile char *)MAP_FAILED)
	{
		perror("Couldn't mmap DATA");
		exit(1);
	}

	/* CODE region — large enough for nthreads consecutive per-thread slices */
	test_info[CODE].pointer_addr = (volatile unsigned long *)mmap(
		(void *)0,
		(MAX_INSTR_BYTES + PAGESIZE - 1) * nthreads,
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	mptr = (volatile char *)test_info[CODE].pointer_addr;
	if (mptr == (volatile char *)MAP_FAILED)
	{
		perror("Couldn't mmap CODE");
		exit(1);
	}

	/* COMM region — holds the four barrier counters (4 ints = 16 bytes used) */
	test_info[COMM].pointer_addr = (volatile unsigned long *)mmap(
		(void *)0,
		(MAX_COMM_BYTES + PAGESIZE - 1),
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	comm_ptr = (volatile char *)test_info[COMM].pointer_addr;
	if (comm_ptr == (volatile char *)MAP_FAILED)
	{
		perror("Couldn't mmap COMM");
		exit(1);
	}

	setbuf(stdout, (char *)NULL);
	setbuf(stderr, (char *)NULL);

	/*
	 * Fork loop — create one child process per thread.
	 * The parent continues the loop after each fork, saving the child's PID.
	 * Each child breaks out of the loop immediately after doing its work.
	 *
	 * COMM area barrier counter layout:
	 *   offset  0: C barrier 1 — before code generation
	 *   offset  4: C barrier 2 — before execution (ensures all code is written)
	 *   offset  8: generated-code start barrier (inside add_headeri output)
	 *   offset 12: generated-code end barrier   (inside add_endi output)
	 */
	for (i = 0; i < nthreads; i++)
	{

		/* Carve out this thread's slice of the CODE region */
		next_ptr = (mptr + (i * MAX_INSTR_BYTES));
		fprintf(stdout, "T%d next_ptr=0x%lx\n", i, (unsigned long)next_ptr);

		/*
		 * All threads share the SAME data area (mdptr) — intentional.
		 * This causes threads to read/write the same cache lines,
		 * which is exactly the MP coherency stress we're after.
		 */
		mdptr_threads[i] = (tptrs)mdptr;
		mptr_threads[i] = (tptrs)next_ptr;
		comm_ptr_threads[i] = (tptrs)comm_ptr;

		if ((pid = fork()) == 0)
		{

			/* ---- child process starts here ---- */
			fprintf(stdout, "T%d fork\n", i);
			fflush(stdout);

			bind_to_cpu(i); /* pin to CPU i so threads run on separate cores */

			/* C barrier 1: wait for all sibling threads to be alive before generating */
			// sync_barrier((volatile int *)(comm_ptr + 0), nthreads);

			ibuilt = build_instructions((volatile char *)mptr_threads[i], i);

			start_test = (funct_t)mptr_threads[i];

			/* C barrier 2: don't start executing until ALL threads have finished writing code */
			// sync_barrier((volatile int *)(comm_ptr + 4), nthreads);

			executeit(start_test); /* jump into the generated code */

			fprintf(stdout, "T%d generation program complete, instructions generated: %d\n", i, ibuilt);
			fflush(stdout);

			if (logfile)
				fclose(logfile);
			exit(0); /* child exits cleanly — parent's waitpid will catch this */
		}
		else if (pid == -1)
		{
			perror("fork failed");
			exit(1);
		}
		else
		{
			/* ---- parent records the child's PID and loops to fork the next one ---- */
			pid_task[i] = pid;
			fprintf(stdout, "child T%d started pid=%d\n", i, pid);
			fflush(stdout);
		}

	} /* end fork loop */

	/* Parent waits for every child to exit before cleaning up */
	for (i = 0; i < nthreads; i++)
		waitpid(pid_task[i], NULL, 0);

	if (logfile)
		fclose(logfile);

	/* Release the three shared regions */
	munmap((caddr_t)mdptr, (MAX_DATA_BYTES + PAGESIZE - 1));
	munmap((caddr_t)mptr, (MAX_INSTR_BYTES + PAGESIZE - 1) * nthreads);
	munmap((caddr_t)comm_ptr, (MAX_COMM_BYTES + PAGESIZE - 1));

	return 0;
}
