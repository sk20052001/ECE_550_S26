# ECE 550 Post-Silicon Validation — Project Part 3: Full Code Walkthrough

This document explains every line of every source file. The goal is that after reading this, anyone on the team can present and defend any piece of the code in front of the professor.

The project has three source files:
- `Makefile` — build and run automation
- `include/ia32_encode.h` — the x86 instruction encoding library
- `encodeit.c` — the main program: argument parsing, process management, instruction generation, execution

---

## Table of Contents
1. [How x86 Instruction Encoding Works (Background)](#background)
2. [Makefile](#makefile)
3. [ia32_encode.h](#ia32_encodeh)
4. [encodeit.c](#encodeitc)

---

## Background: How x86 Instruction Encoding Works

Before reading the code, you need to understand how x86 instructions are laid out in memory as raw bytes. Every instruction is a sequence of byte fields:

```
[ Prefixes (0-4 bytes) ] [ Opcode (1-3 bytes) ] [ ModR/M (1 byte) ] [ Displacement (0/1/4 bytes) ] [ Immediate (0/1/2/4/8 bytes) ]
```

**Prefixes** modify the instruction. Common ones used in this project:
- `0x66` — operand-size override (switches between 16-bit and 32-bit)
- `0x40`–`0x4F` — REX prefix in 64-bit mode (lets you access 64-bit registers and the R8–R15 set)
- `0xF0` — LOCK prefix (makes the memory operation atomic on the system bus)

**The ModR/M byte** packs three fields into a single byte. This is the key to understanding most of the encoding functions:

```
  Bit 7   Bit 6   Bit 5   Bit 4   Bit 3   Bit 2   Bit 1   Bit 0
 [ Mod           ] [  Reg field       ] [  R/M field       ]
```

- **Mod (bits 7–6):** Addressing mode.
  - `00` = register-indirect, no displacement: `[reg]`
  - `01` = register + 8-bit signed displacement: `[reg + disp8]`
  - `10` = register + 32-bit signed displacement: `[reg + disp32]`
  - `11` = register-to-register (no memory involved)
- **Reg (bits 5–3):** Usually the destination or one of the two register operands.
- **R/M (bits 2–0):** Usually the other register, or the base register for memory access.

The register encoding numbers (0–7) map as follows:
```
0 = EAX/RAX    1 = ECX/RCX    2 = EDX/RDX    3 = EBX/RBX
4 = ESP/RSP    5 = EBP/RBP    6 = ESI/RSI    7 = EDI/RDI
```

In 64-bit mode, R8–R15 use the same 0–7 encoding but require a REX prefix byte with the appropriate bit set to distinguish them.

---

## Makefile

```makefile
all: clean build run
```
The default target (what runs when you just type `make`) is a chain: clean first, then build, then run. This ensures you always test with a freshly compiled binary.

```makefile
build: encodeit
```
The `build` target depends on `encodeit`. Make will only rebuild it if the source files are newer than the binary.

```makefile
encodeit:
	gcc -g -o encodeit encodeit.c -I include
```
Compiles `encodeit.c` with debug symbols (`-g` so GDB can show source lines), outputs the binary named `encodeit`, and tells the compiler to look in the `include/` directory for header files (`-I include`). This is a single-file compile — the header is `#include`'d directly, so no separate object file step is needed.

```makefile
SEED     ?= 0
NINSTRS  ?= 25
NTHREADS ?= 1
LOGFILE  ?=
```
These are Make variables with default values. The `?=` operator means "set this only if it hasn't already been set on the command line." So `make run` uses these defaults, but `make run SEED=42 NTHREADS=4` overrides them. `LOGFILE` defaults to empty (no log file).

```makefile
RUN_ARGS := $(SEED) $(NINSTRS) $(NTHREADS)
ifneq ($(LOGFILE),)
    RUN_ARGS += $(LOGFILE)
endif
```
Builds the argument string that gets passed to the binary. `LOGFILE` is only appended if it's non-empty, because the program treats the 4th argument as optional — passing an empty string would confuse it.

```makefile
run: build
	./encodeit $(RUN_ARGS)
```
The `run` target first ensures the binary is built, then runs it with whatever arguments are in `RUN_ARGS`.

```makefile
gdb: build
	gdb -q ./encodeit -ex 'set follow-fork-mode child' -ex 'br executeit' -ex 'run $(RUN_ARGS)' -ex 'x/60ai start_addr'
```
Launches GDB with several automatic commands (`-ex`):
- `-q` suppresses the banner
- `set follow-fork-mode child` makes GDB follow the child process after `fork()` instead of staying with the parent, since all the interesting work happens in the child
- `br executeit` sets a breakpoint at the `executeit` function, which is right before we jump into the generated code
- `run $(RUN_ARGS)` starts the program with the same arguments as `make run`
- `x/60ai start_addr` disassembles 60 instructions starting at the generated code address — `a` means auto-size, `i` means disassemble as instructions

```makefile
.PHONY: all build run gdb clean
```
Declares these targets as "phony" — meaning Make should always run them even if a file with that name happens to exist on disk.

```makefile
clean:
	rm -f encodeit
```
Removes the compiled binary. The `-f` flag prevents an error if the file doesn't exist.

---

## ia32_encode.h

This header file is the heart of the project. It contains inline functions that write raw x86 machine bytes into a memory buffer. Every function takes a `volatile char *tgt_addr` pointing to the next empty byte in the buffer, writes some bytes, and returns the updated pointer.

### File Header and Includes

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <limits.h>    /* for PAGESIZE */
```
Standard includes. `sys/mman.h` is needed for the `mmap`-related constants used in the size defines below. `limits.h` provides `PAGESIZE` (typically 4096 bytes) on Linux.

---

### Encoding Constants

```c
#define PREFIX_16BIT   0x66
```
The operand-size override prefix. Inserting this byte before an instruction switches it from 32-bit mode to 16-bit mode. For example, a regular `MOV EAX, EBX` becomes `MOV AX, BX` when preceded by `0x66`.

```c
#define BASE_MODRM     0xc0
```
`0xC0` in binary is `11 000 000`. The top two bits (`11`) set Mod=3, which means register-to-register mode (no memory access). We OR this with the shifted register fields to build the complete ModR/M byte.

```c
#define REG_SHIFT      0x3
```
The Reg field occupies bits 5–3 of ModR/M. To place a register number (0–7) into those bits, we left-shift it by 3.

```c
#define MODRM_SHIFT    0x6
```
The Mod field occupies bits 7–6 of ModR/M. To place the addressing mode (0–3) into those bits, we left-shift it by 6.

```c
#define RM_SHIFT       0x0
```
The R/M field occupies bits 2–0. No shift needed — it sits at the bottom of the byte.

```c
#define REG_MASK       0x7
#define RM_MASK        0x7
#define MOD_MASK       0x3
```
Bit masks for extracting the three ModR/M fields. Not used directly in this project but included for completeness and as documentation.

---

### Register Encodings

```c
#define REG_EAX        0x0
#define REG_ECX        0x1
#define REG_EDX        0x2
#define REG_EBX        0x3    /* we keep EBX reserved as the data-area base pointer */
#define REG_ESP        0x4
#define REG_EBP        0x5
#define REG_ESI        0x6
#define REG_EDI        0x7
```
These are the 3-bit encodings for the eight legacy x86 general-purpose registers. These numbers go directly into the Reg and R/M fields of the ModR/M byte. EBX (3) is marked because our generated code dedicates RBX as the base pointer for the shared data area — we never randomly assign it as a destination.

```c
#ifndef REG_R8
#define REG_R8        0x0
#endif
... (through REG_R15)
```
The extended x86_64 registers R8–R15 also use a 3-bit encoding (0–7), but they need a REX prefix byte to distinguish them from the legacy registers. The `#ifndef` guards are required because when `_GNU_SOURCE` is defined (which we need for `sched_setaffinity`), the system header `sys/ucontext.h` defines its own `REG_R8` through `REG_R15` as enum values for signal-handler register contexts. Without these guards, the compiler would emit a warning about macro redefinition on every compile.

---

### Byte Offset and Size Constants

```c
#define BYTE1_OFF      0x1
#define BYTE2_OFF      0x2
#define BYTE3_OFF      0x3
#define BYTE4_OFF      0x4
```
Used to advance the instruction pointer after writing a 1-, 2-, 3-, or 4-byte value. Writing `tgt_addr += BYTE2_OFF` is cleaner and more self-documenting than `tgt_addr += 2`.

```c
#define ISZ_1         0x1
#define ISZ_2         0x2
#define ISZ_4         0x4
#define ISZ_8         0x8
```
Operand sizes in bytes. Passed to every instruction builder so it can choose the right opcode and prefixes. ISZ_1 = 8-bit byte, ISZ_2 = 16-bit word, ISZ_4 = 32-bit dword, ISZ_8 = 64-bit qword.

---

### REX Prefix Constants

```c
#define REX_PREFIX    0x40
```
The base REX byte. In binary: `0100 0000`. The four low bits are the extension bits. A bare `0x40` is technically a no-op REX, but we combine it with the bits below using bitwise OR.

```c
#define REX_W         0x8    /* 64-bit operand size */
```
When ORed into the REX byte, this bit (`0100 1000` = `0x48`) promotes the operation to 64-bit. This is how we write `MOV RAX, imm64` instead of `MOV EAX, imm32`.

```c
#define REX_R         0x4    /* extends ModR/M Reg  */
#define REX_X         0x2    /* extends SIB Index   */
#define REX_B         0x1    /* extends R/M or opcode-reg */
```
REX.R lets the Reg field address R8–R15. REX.X extends the SIB index (not used here). REX.B extends R/M (or the register embedded in the opcode for PUSH/POP — that's how we push R12–R15).

```c
#define LOCK_PREFIX   0xF0
```
The LOCK prefix forces the CPU to assert the bus lock signal for the duration of the following memory instruction, making the read-modify-write cycle atomic. It is only valid when the destination operand is in memory — applying it to a register-only instruction is illegal and will cause a `#UD` (invalid opcode) exception.

---

### Memory Region Size Constants

```c
#define MAX_THREADS     12
```
Maximum number of child processes the program supports. Arrays like `pid_task[]` and `mptr_threads[]` are sized to this value.

```c
#define MAX_DEF_INSTRS  25
```
Default number of random instructions to generate per thread if the user doesn't specify a count on the command line. 25 was specified by the professor.

```c
#define MAX_INSTR_BYTES (3*PAGESIZE)
```
How many bytes to allocate for each thread's code buffer. `3 * 4096 = 12,288` bytes. A single encoded instruction is at most about 15 bytes, so 12 KB comfortably holds 25 instructions plus the prologue and epilogue.

```c
#define MAX_DATA_BYTES  (10*PAGESIZE)
```
Size of the shared data region: `10 * 4096 = 40,960` bytes. All threads share this single 40 KB region. Our largest displacement is 32 bytes, so all memory accesses stay well within bounds.

```c
#define MAX_COMM_BYTES  (PAGESIZE)
```
Size of the communication region: one page (4096 bytes). We only use 16 bytes of it (four 4-byte int counters for the barriers), but allocating a full page is required — `mmap` works in page-granularity.

```c
#define NUM_PTRS 3
#define CODE 0
#define DATA 1
#define COMM 2
```
Indices used to index into the `test_info[]` array. Instead of remembering that index 0 is CODE and index 2 is COMM, we use these named constants.

---

### build_push_reg

```c
static inline volatile char *build_push_reg(int reg_index, int x86_64f, volatile char *tgt_addr)
```
`static` means this function is only visible within this translation unit. `inline` suggests the compiler should inline the call rather than generate an actual function call — important since we call these inside a tight code-generation loop. Returns the updated pointer.

```c
	if (x86_64f) {
		(*(char *) tgt_addr) = (REX_PREFIX | REX_B);
		tgt_addr += BYTE1_OFF;
	}
```
If `x86_64f` is 1, we're pushing one of the extended registers R8–R15. These require a REX.B prefix before the opcode. `REX_PREFIX | REX_B` = `0x40 | 0x01` = `0x41`. We write this byte and advance the pointer by 1.

```c
	(*(char *) tgt_addr) = 0x50 + reg_index;
	tgt_addr += BYTE1_OFF;
```
The PUSH opcode for the legacy registers is `0x50` (PUSH RAX) through `0x57` (PUSH RDI). Adding `reg_index` selects the right register. With the REX.B prefix already emitted, this same formula reaches R8–R15.

---

### build_pop_reg

Identical structure to `build_push_reg` but uses opcode `0x58 + reg_index` (POP). REX.B is still needed for R8–R15.

---

### build_pusha / build_popa

```c
static inline volatile char *build_pusha(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0x60;
	return (tgt_addr);
}
```
`PUSHA` (opcode `0x60`) pushes all eight 32-bit general-purpose registers onto the stack in one instruction. This is only valid in 32-bit mode — it is an invalid opcode in 64-bit long mode. We use it in the `#else` branch of the 32-bit fallback code path. `POPA` (`0x61`) pops them all back in reverse order.

---

### build_ret

```c
static inline volatile char *build_ret(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0xc3;
	return (tgt_addr);
}
```
`0xC3` is the near RET instruction — pops the return address from the stack and jumps to it. This is what makes the generated code callable as a function pointer. `(*tgt_addr++)` writes the byte `0xC3` and then increments the pointer in one expression (post-increment).

---

### build_mov_register_to_register

```c
static inline volatile char *build_mov_register_to_register(short mov_size, int src_reg, int dest_reg, volatile char *tgt_addr)
```
Encodes `MOV dest, src` — copy one register into another.

```c
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;
```
For 16-bit moves, the operand-size override prefix `0x66` must come first. Without it, the processor would treat the following `0x8B` opcode as a 32-bit move.

```c
	switch(mov_size) {
	case 1:
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8a;
		tgt_addr += BYTE2_OFF;
		break;
```
For 8-bit (byte) moves, the opcode is `0x8A`. We're writing two bytes in one 16-bit store: the lower byte is the opcode (`0x8A`), the upper byte is the ModR/M. The ModR/M is constructed as: `BASE_MODRM` (sets Mod=11, register-to-register) plus `dest_reg` shifted left 3 (puts destination in the Reg field) plus `src_reg` (sits in the R/M field at the bottom). The result is shifted left 8 and ORed with the opcode so that when stored as a little-endian 16-bit value, the opcode comes first in memory.

```c
	case 2:
	case 4:
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		tgt_addr += BYTE2_OFF;
		break;
```
16-bit and 32-bit moves both use opcode `0x8B`. The operand-size prefix already selected the width before the switch statement, so the opcode is the same for both.

```c
	case 8:
		(*(char *) tgt_addr) = (REX_PREFIX | REX_W);
		tgt_addr += BYTE1_OFF;
		(*(short *) tgt_addr) = ((BASE_MODRM) + (dest_reg << REG_SHIFT) + src_reg) << 8 | 0x8b;
		tgt_addr += BYTE2_OFF;
		break;
```
64-bit moves prefix with `REX.W` (`0x48`), which tells the processor to use 64-bit operand size, then use the same `0x8B` opcode. Three bytes total: `0x48 0x8B ModR/M`.

---

### build_mov_imm_to_register

Encodes `MOV dest, immediate_constant`. x86 has a compact form for this: the register number is embedded directly into the opcode byte, so no ModR/M is needed.

```c
	case 1:
		(*(short *) tgt_addr) = imm_value << 8 | (0xb0 + dest_reg);
		tgt_addr += BYTE2_OFF;
		break;
```
`0xB0` through `0xB7` are the 8-bit MOV immediate opcodes. `0xB0 + dest_reg` selects the target register. The immediate (1 byte) follows immediately. We pack both into a single 16-bit write: opcode in the low byte, immediate in the high byte.

```c
	case 2:
		(*(int *) tgt_addr) = (imm_value << 16) | ((0xb8 + dest_reg) << 8) | PREFIX_16BIT;
		tgt_addr += BYTE4_OFF;
		break;
```
For 16-bit, we need: `0x66` (prefix), `0xB8+reg` (opcode), then 2 bytes of immediate. We pack all four bytes into one 32-bit write. In memory (little-endian), the `PREFIX_16BIT` lands first (lowest address), then the opcode, then the 2-byte immediate.

```c
	case 4:
		(*tgt_addr++) = 0xb8 + dest_reg;
		(*(int *) tgt_addr) = imm_value;
		tgt_addr += BYTE4_OFF;
		break;
```
32-bit: opcode byte followed by a 4-byte immediate. `0xB8` through `0xBF` are the 32-bit MOV immediate opcodes.

```c
	case 8:
		(*tgt_addr++) = (REX_PREFIX | REX_W);
		(*tgt_addr++) = 0xb8 + dest_reg;
		(*(long *) tgt_addr) = imm_value;
		tgt_addr += ISZ_8;
		break;
```
64-bit: REX.W prefix, then `0xB8+reg`, then 8 bytes of immediate. This is the `MOVABS` form — the only way to load a full 64-bit constant directly into a register. 10 bytes total. We use this to bake the data-area base address into the generated code's prologue.

---

### build_mov_memory_to_register

Encodes `MOV dest_reg, [base_reg + displacement]` — a memory load. This is more complex because we need to encode the addressing mode in ModR/M.

```c
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;
	if (mov_size == 8)
		(*tgt_addr++) = (REX_PREFIX | REX_W);
```
Emit the right prefix(es) before the opcode. These are separate `if` statements (not `else if`) because they handle mutually exclusive sizes, but it's correct since only one can be true at a time.

```c
	switch (mov_size) {
	case 1:  (*tgt_addr++) = 0x8a; break;
	case 2:
	case 4:
	case 8:  (*tgt_addr++) = 0x8b; break;
```
`0x8A` is the 8-bit load opcode; `0x8B` is the 16/32/64-bit load opcode. For 64-bit the REX.W prefix already promotes it.

```c
	if (displacement == 0) {
		(*(char *) tgt_addr) = ((0x0 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
```
`Mod=00` means no displacement — just `[base_reg]`. ModR/M = `00 dest_reg base_reg`. Since `0x0 << 6 = 0`, this simplifies to `dest_reg << 3 | base_reg`.

```c
	} else if ((displacement >= -128) && (displacement <= 127)) {
		(*(char *) tgt_addr) = ((0x1 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(char *) tgt_addr) = (char)displacement;
		tgt_addr += BYTE1_OFF;
```
`Mod=01` means 8-bit signed displacement. ModR/M sets Mod=01 (`0x1 << 6 = 0x40`), then the displacement byte follows. Our displacement choices (0, 8, 32) always fit in a signed byte.

```c
	} else {
		(*(char *) tgt_addr) = ((0x2 << MODRM_SHIFT) + (dest_reg << REG_SHIFT) + base_reg);
		tgt_addr += BYTE1_OFF;
		(*(int *) tgt_addr) = (int)displacement;
		tgt_addr += BYTE4_OFF;
```
`Mod=10` means 32-bit displacement. ModR/M sets Mod=10 (`0x2 << 6 = 0x80`), then four bytes of displacement follow. This path would be hit for displacements larger than 127.

---

### build_mov_register_to_memory

Encodes `MOV [base_reg + displacement], src_reg` — a memory store. The structure is a mirror image of the load. Opcode `0x88` for 8-bit stores, `0x89` for wider stores. The only difference in the ModR/M encoding is that the Reg field now holds the *source* register (the value being stored) and the R/M field holds the base register for the memory address.

---

### build_xadd

```c
static inline volatile char *build_xadd(short mov_size, int src_reg, int base_reg, long displacement, int use_lock, volatile char *tgt_addr)
```
Encodes `XADD [base_reg + disp], src_reg`. XADD is an atomic exchange-and-add: it reads the memory value, adds the register to it, writes the result back to memory, and stores the *old* memory value into the register. All of this happens atomically when the LOCK prefix is present.

```c
	if (use_lock)
		(*tgt_addr++) = LOCK_PREFIX;
```
The LOCK prefix (`0xF0`) is emitted first, before everything else including the operand-size prefix. It asserts the bus lock for the entire instruction.

```c
	if (mov_size == 2)
		(*tgt_addr++) = PREFIX_16BIT;
	if (mov_size == 8)
		(*tgt_addr++) = (REX_PREFIX | REX_W);
```
Operand-size selection, same pattern as the MOV functions.

```c
	(*tgt_addr++) = 0x0f;
	(*tgt_addr++) = (mov_size == 1) ? 0xc0 : 0xc1;
```
XADD uses a two-byte escape opcode: `0x0F` followed by `0xC0` (8-bit form) or `0xC1` (16/32/64-bit form). The `0x0F` escape is how x86 accesses its extended instruction set — hundreds of instructions live behind this prefix.

```c
	/* ModR/M byte — same Mod/disp logic as the MOV memory functions */
```
The ModR/M byte and optional displacement bytes are encoded identically to `build_mov_register_to_memory`. The Reg field holds `src_reg` (the register operand) and R/M holds `base_reg` (the memory base address).

---

### build_xchg

```c
static inline volatile char *build_xchg(short mov_size, int src_reg, int base_reg, long displacement, int use_lock, volatile char *tgt_addr)
```
Encodes `XCHG [base_reg + disp], src_reg`. XCHG atomically swaps the register and memory values.

Important note: according to the Intel SDM, a memory-form XCHG has an *implicit* LOCK semantics even without the explicit `0xF0` prefix. The CPU always treats it as atomic. We emit the explicit prefix when `use_lock=1` to make the intent visible in the byte stream, matching the professor's requirement to randomize with/without LOCK.

```c
	(*tgt_addr++) = (mov_size == 1) ? 0x86 : 0x87;
```
XCHG uses a single-byte opcode: `0x86` for 8-bit, `0x87` for 16/32/64-bit. No two-byte escape needed (unlike XADD). The rest of the ModR/M and displacement encoding is the same as all other memory-form instructions.

---

### build_mfence / build_sfence / build_lfence

```c
static inline volatile char *build_mfence(volatile char *tgt_addr)
{
	(*tgt_addr++) = 0x0f;
	(*tgt_addr++) = 0xae;
	(*tgt_addr++) = 0xf0;
	return (tgt_addr);
}
```
Each fence instruction is exactly 3 bytes. The encoding is `0x0F 0xAE` (two-byte escape into the memory control instruction group) followed by a third byte that selects which fence:
- `0xF0` → MFENCE: orders all memory operations (both loads and stores) globally. Every core will see all previous stores from this core before any subsequent load or store.
- `0xF8` → SFENCE: orders stores only. Ensures all previous stores are globally visible before subsequent stores.
- `0xE8` → LFENCE: orders loads only. Ensures all previous loads complete before subsequent loads.

These are critical for multi-core correctness: without fences, out-of-order CPUs can reorder memory accesses in ways that violate program assumptions.

---

## encodeit.c

### File Header and Includes

```c
#define _GNU_SOURCE
```
Must be defined before any `#include` to unlock GNU extensions in the standard library. Specifically needed for `sched_setaffinity`, `CPU_ZERO`, and `CPU_SET` from `<sched.h>`. Without this, those symbols don't exist.

```c
#ifndef __x86_64__
#define __x86_64__
#endif
```
Forces the x86_64 code paths to be compiled even in environments where the compiler doesn't automatically set this macro. This ensures our 64-bit prologue/epilogue and barrier code always gets compiled.

```c
#include <stdio.h>     /* printf, fprintf, fopen, fclose, perror */
#include <stdlib.h>    /* rand, srand, atoi, strtoul, exit */
#include <errno.h>     /* errno — set by system calls on failure */
#include <string.h>    /* strerror — converts errno to a human-readable message */
#include <unistd.h>    /* fork, getpid */
#include <sys/types.h> /* pid_t */
#include <sys/mman.h>  /* mmap, munmap, MAP_ANONYMOUS, MAP_SHARED, PROT_* */
#include <sys/wait.h>  /* waitpid */
#include <sched.h>     /* sched_setaffinity, cpu_set_t, CPU_ZERO, CPU_SET */
#include <limits.h>    /* PAGESIZE */
```
Each include pulls in the declarations needed for the system calls and library functions used in the program.

```c
#include "ia32_encode.h"
```
Pulls in all the instruction encoding functions, constants, and size defines. The quotes (not angle brackets) mean the compiler looks in the local `include/` directory first.

```c
#ifndef PAGESIZE
#define PAGESIZE 4096
#endif
```
Safety fallback in case `limits.h` doesn't define `PAGESIZE`. On Linux x86_64, the page size is always 4096 bytes.

```c
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
```
`MAP_ANONYMOUS` is the Linux name; `MAP_ANON` is the BSD name. This alias makes the code portable.

---

### Global Variables

```c
volatile char *mptr = 0, *next_ptr = 0, *mdptr = 0, *comm_ptr = 0;
```
Four global pointers kept as globals intentionally so GDB can inspect them at any time with `p mptr` or `x/60ai mptr`. `volatile` tells the compiler not to cache the value in a register — always re-read from memory — because forked child processes share these pages and the values can change from outside the current process.

- `mptr` — base address of the CODE region (the start of all generated code)
- `next_ptr` — current write position within the code buffer (moves forward as instructions are emitted)
- `mdptr` — base address of the shared DATA region (where memory loads/stores go)
- `comm_ptr` — base address of the COMM region (holds barrier counters)

```c
int num_inst = 0, i = 0;
```
`num_inst` tracks how many instructions have been emitted so far in `build_instructions`. `i` is the loop variable for the fork loop, declared global so it can be inspected in GDB.

```c
int target_ninstrs = MAX_DEF_INSTRS;
```
How many random instructions to generate. Defaults to `MAX_DEF_INSTRS` (25) but can be overridden by argv[2].

```c
int nthreads = 1, pid_task[MAX_THREADS], pid = 0;
```
`nthreads` is how many child processes to fork. `pid_task[]` stores the PID of each child so the parent can `waitpid` for them. `pid` holds the return value of `fork()` temporarily.

```c
typedef struct {
    volatile unsigned long *pointer_addr;
} test_i;
test_i test_info[NUM_PTRS];
```
A simple struct that wraps a pointer. `test_info[CODE]`, `test_info[DATA]`, and `test_info[COMM]` hold the base addresses of the three mmap'd regions. The struct is mostly a way to keep the three pointers organized under named indices.

```c
typedef volatile unsigned long *tptrs;
```
A type alias for `volatile unsigned long *`. Used when casting pointers into the per-thread arrays.

```c
volatile unsigned long *mptr_threads[MAX_THREADS];
volatile unsigned long *mdptr_threads[MAX_THREADS];
volatile unsigned long *comm_ptr_threads[MAX_THREADS];
```
Per-thread pointer arrays. Each entry corresponds to one forked child:
- `mptr_threads[i]` — where thread `i` starts writing its instructions
- `mdptr_threads[i]` — the data base address that thread `i`'s generated code uses (all point to the same `mdptr`)
- `comm_ptr_threads[i]` — the barrier counter area (all point to the same `comm_ptr`)

```c
FILE *logfile = NULL;
```
File handle for the optional log file. `NULL` means logging is disabled. Set in `main()` if the user provides a filename.

```c
typedef int (*funct_t)();
funct_t start_test;
```
Defines a function pointer type that takes no arguments and returns `int`. `start_test` will hold the address of the generated code buffer, cast to this type, so we can call the generated instructions as if they were a C function.

```c
int executeit(funct_t start_addr);
int build_instructions(volatile char *code_ptr, int thread_id);
```
Forward declarations — these functions are defined later in the file but referenced before their definitions, so we declare their signatures here.

---

### rand_range

```c
static inline int rand_range(int min_n, int max_n)
{
    return min_n + (rand() % (max_n - min_n + 1));
}
```
Returns a random integer in the closed interval `[min_n, max_n]`. `rand()` returns a value in `[0, RAND_MAX]`. Taking `% (max_n - min_n + 1)` maps it to `[0, range]`, and adding `min_n` shifts it to `[min_n, max_n]`. The `+1` ensures the upper bound is inclusive.

---

### pick_instruction_kind

```c
static inline int pick_instruction_kind(void)
{
    return rand_range(0, 6);
}
```
Returns a number 0 through 6 uniformly at random. Each number maps to one instruction family in `emit_random_instruction`:
- 0: imm → reg (MOV register, immediate)
- 1: reg → reg (MOV register, register)
- 2: reg → mem (MOV memory, register — a store)
- 3: mem → reg (MOV register, memory — a load)
- 4: XADD (atomic exchange-and-add to memory)
- 5: XCHG (atomic exchange with memory)
- 6: fence (MFENCE, SFENCE, or LFENCE)

---

### pick_mov_size

```c
static inline short pick_mov_size(void)
{
    static const short sizes[] = {ISZ_1, ISZ_2, ISZ_4, ISZ_8};
    return sizes[rand_range(0, 3)];
}
```
Picks one of the four operand widths: 1, 2, 4, or 8 bytes. The `static const` array is allocated once at program startup, not re-created on each call. `rand_range(0, 3)` picks an index, and we return the corresponding size constant.

---

### pick_mov_size_atomic

```c
static inline short pick_mov_size_atomic(void)
{
    static const short sizes[] = {ISZ_1, ISZ_2, ISZ_4};
    return sizes[rand_range(0, 2)];
}
```
Same idea but excludes ISZ_8. The reason: the shared DATA area is zeroed by `mmap` but not aligned to 8-byte boundaries at every possible offset. A 64-bit XADD or XCHG to an unaligned address would cause a fault. Since we're generating addresses as `RBX + {0, 8, 32}`, and RBX itself might not be 8-byte aligned at those offsets, we play it safe by capping atomics at 32-bit.

---

### pick_general_reg

```c
static inline int pick_general_reg(short mov_size)
{
    static const int regs_word_dword_qword[] = {REG_EAX, REG_ECX, REG_EDX, REG_ESI, REG_EDI};
    static const int regs_byte[]             = {REG_EAX, REG_ECX, REG_EDX, REG_ESI};

    if (mov_size == ISZ_1)
        return regs_byte[rand_range(0, 3)];
    return regs_word_dword_qword[rand_range(0, 4)];
}
```
Picks a register from the valid pool, with two important exclusions:

1. **EBX is never in either array.** We reserve RBX throughout the generated code as the base pointer for the shared data area. If a random instruction overwrote RBX, every subsequent load/store instruction would compute the wrong address and likely crash.

2. **For byte (8-bit) operations, ESI is included but EDI is not.** This is an x86 encoding detail: without a REX prefix, the register field values 4–7 in byte mode encode AH, CH, DH, BH — not SPL, BPL, SIL, DIL. We keep ESI (6) which encodes as DH in byte mode — that's technically a different register, but it doesn't crash, and the goal is encoding coverage. EDI (7) is excluded to keep the pool small and avoid confusion.

---

### pick_source_reg and pick_dest_reg

```c
static inline int pick_source_reg(short mov_size)
{
    return pick_general_reg(mov_size);
}
```
Just calls `pick_general_reg` — a wrapper for clarity.

```c
static inline int pick_dest_reg(short mov_size, int source_reg)
{
    int dest_reg = pick_general_reg(mov_size);
    while ((dest_reg == source_reg) || (dest_reg == REG_EBX))
        dest_reg = pick_general_reg(mov_size);
    return dest_reg;
}
```
Picks a destination that is different from the source. The `while` loop retries until it gets a valid choice. EBX is excluded a second time here as a safety net — `pick_general_reg` already excludes it, but the loop guard makes the invariant explicit and handles any future changes to the pool. We don't want `MOV EAX, EAX` (pointless self-move) so we ensure `dest_reg != source_reg`.

---

### pick_displacement

```c
static inline long pick_displacement(void)
{
    static const long disps[] = {0, 8, 32};
    return disps[rand_range(0, 2)];
}
```
Returns one of three byte offsets from the data-area base. These were specified by the professor. All three fit within a signed 8-bit value (–128 to +127), so the encoder always takes the `Mod=01` (8-bit displacement) path when these are non-zero — resulting in shorter encoded instructions.

---

### pick_immediate

```c
static inline long pick_immediate(short mov_size)
{
    switch (mov_size) {
    case ISZ_1: return rand_range(0x01, 0x7f);
```
For 8-bit moves, pick a value in [1, 127]. We avoid zero to make the instructions distinguishable in GDB output.

```c
    case ISZ_2: return rand_range(0x0100, 0x7fff);
```
For 16-bit moves, pick in [256, 32767] — values that require at least 2 bytes, so the instruction is clearly 16-bit.

```c
    case ISZ_4:
        return (long)(((unsigned long long)rand() << 16) ^ (unsigned long long)rand());
```
For 32-bit moves, we XOR two `rand()` calls shifted apart to build a pseudo-random 32-bit value. `rand()` only guarantees 15 bits of randomness on some platforms, so combining two calls gives better coverage.

```c
    case ISZ_8:
    default:
        return (long)((((unsigned long long)rand() << 48) ^
                       ((unsigned long long)rand() << 32) ^
                       ((unsigned long long)rand() << 16) ^
                        (unsigned long long)rand()));
```
For 64-bit, XOR four `rand()` calls shifted into different 16-bit windows to fill all 64 bits with pseudo-random data.

---

### emit_random_instruction

```c
static inline void emit_random_instruction(void)
{
    int   kind       = pick_instruction_kind();
    short mov_size   = pick_mov_size();
    int   src_reg    = pick_source_reg(mov_size);
    int   dest_reg   = pick_dest_reg(mov_size, src_reg);
    long  disp       = pick_displacement();
    long  imm_value  = pick_immediate(mov_size);
    int   use_lock   = rand_range(0, 1);
```
Picks all parameters upfront before the switch statement. Not all parameters are used by every instruction kind (e.g., fences use none of them), but pre-picking them all simplifies the switch cases and avoids partial initialization.

```c
    const char *kind_name = "unknown";
```
String label for the log file. Initialized to "unknown" as a safety net; it will always be overwritten inside the switch before the log write.

```c
    case 0:
        next_ptr = build_mov_imm_to_register(mov_size, imm_value, dest_reg, next_ptr);
        kind_name = "imm->reg";
        break;
```
`next_ptr` is the global write position. Each builder returns the new write position after encoding the instruction. We store it back into `next_ptr` so the next call starts where this one left off.

```c
    case 2:
        next_ptr = build_mov_register_to_memory(mov_size, src_reg, REG_EBX, disp, next_ptr);
```
Memory operations always use `REG_EBX` as the base register — that's the register we pre-loaded with the shared data area address in the prologue. This is hardcoded, not random.

```c
    case 4: {
        short atomic_sz = pick_mov_size_atomic();
        next_ptr = build_xadd(atomic_sz, src_reg, REG_EBX, disp, use_lock, next_ptr);
        mov_size = atomic_sz;
```
For atomics we pick a new size (capped at 32-bit) because the MOV size picked earlier might be ISZ_8. We overwrite `mov_size` with the atomic size so the log file records the correct width.

```c
    case 6: {
        int fence_kind = rand_range(0, 2);
        if (fence_kind == 0) { next_ptr = build_mfence(next_ptr); kind_name = "mfence"; }
        else if (fence_kind == 1) { next_ptr = build_sfence(next_ptr); kind_name = "sfence"; }
        else { next_ptr = build_lfence(next_ptr); kind_name = "lfence"; }
        mov_size = 0; disp = 0; src_reg = -1; dest_reg = -1;
```
Fence instructions have no register or memory operands, so we zero out those fields so the log file doesn't print misleading values. Setting registers to `-1` signals "not applicable."

```c
    num_inst++;
```
Increments the global instruction count. `build_instructions` uses this to know when to stop.

```c
    if (logfile) {
        fprintf(logfile, "inst=%d type=%-12s size=%d src_reg=%d dest_reg=%d disp=%ld\n",
                num_inst, kind_name, mov_size, src_reg, dest_reg, disp);
    }
```
Writes one log line per instruction. `%-12s` left-pads the type string to 12 characters for column alignment. The `if (logfile)` check is `NULL`-safe — if no log file was opened, this entire block is skipped.

```c
    fprintf(stderr, "next ptr is now 0x%lx\n", (long)next_ptr);
```
Prints the current write position to stderr after every instruction. Useful for debugging and for verifying in GDB that the code buffer is growing correctly.

---

### emit_barrier_code

This is the most technically complex function in the project. It generates a complete spin-wait synchronization barrier as raw x86_64 machine bytes directly into the code buffer. When the generated code runs, this barrier makes all threads pause until every thread has reached this point.

```c
static inline volatile char *emit_barrier_code(volatile char *tgt_addr, volatile int *barrier_counter, int n)
{
#if defined(__x86_64__) || defined(_M_X64)
```
The entire body is wrapped in a preprocessor check for 64-bit mode. The 32-bit fallback path (in the `#else` of `add_headeri`/`add_endi`) does not emit a generated barrier.

```c
    (*tgt_addr++) = 0x50;   /* push rax */
    (*tgt_addr++) = 0x51;   /* push rcx */
```
We need RAX and RCX as scratch registers for the barrier. Rather than reserving them permanently, we save them on the stack first. The x86_64 PUSH short-form opcodes for the legacy registers are `0x50` (RAX) through `0x57` (RDI). Post-increment `tgt_addr++` writes the byte and moves the pointer forward simultaneously.

```c
    (*tgt_addr++) = 0x48;   /* REX.W */
    (*tgt_addr++) = 0xb8;   /* MOV RAX, imm64 */
    (*(unsigned long *)tgt_addr) = (unsigned long)barrier_counter;
    tgt_addr += 8;
```
This is `MOVABS RAX, imm64` — loads the 64-bit address of the barrier counter into RAX. The full instruction is: `REX.W` (`0x48`) + `MOV RAX, imm64` opcode (`0xB8`) + 8 bytes of address. We cast `tgt_addr` to `unsigned long *` and write the address as a raw 8-byte value. Then advance the pointer by 8.

The critical insight: the address of `barrier_counter` is a compile-time constant relative to program load, and we bake it directly into the generated code as a literal immediate. When the code executes, RAX will hold the exact physical address of the counter in the shared COMM page.

```c
    (*tgt_addr++) = 0xb9;   /* MOV ECX, imm32 */
    (*(int *)tgt_addr) = 1;
    tgt_addr += 4;
```
Loads the value `1` into ECX. This is the increment amount for the XADD below. `0xB9` is `MOV ECX, imm32` (the compact register-embedded form, no ModR/M needed).

```c
    (*tgt_addr++) = 0xf0;   /* LOCK prefix */
    (*tgt_addr++) = 0x0f;
    (*tgt_addr++) = 0xc1;
    (*tgt_addr++) = 0x08;   /* ModR/M: [RAX], ECX */
```
This is `LOCK XADD [RAX], ECX`. Broken down byte by byte:
- `0xF0` = LOCK prefix — makes the operation atomic
- `0x0F` = two-byte escape
- `0xC1` = XADD opcode (32-bit form)
- `0x08` = ModR/M byte: `00 001 000` → Mod=00 (no displacement), Reg=ECX(001), R/M=RAX(000)

This atomically does: `old = [RAX]; [RAX] = [RAX] + ECX; ECX = old`. Every thread that reaches this point increments the shared counter by 1. ECX gets the old value (which we discard). After all `n` threads have executed this, the counter equals `n`.

```c
    (*tgt_addr++) = 0x8b;   /* MOV ECX, [RAX] */
    (*tgt_addr++) = 0x08;   /* ModR/M: Mod=00, Reg=ECX, R/M=RAX */
```
Start of the spin loop. Reads the current value of the barrier counter from memory into ECX. The ModR/M byte `0x08` = `00 001 000` → Mod=00, Reg=ECX(1), R/M=RAX(0). This is a 2-byte instruction.

```c
    (*tgt_addr++) = 0x81;   /* CMP ECX, imm32 */
    (*tgt_addr++) = 0xf9;   /* ModR/M: /7 on ECX */
    (*(int *)tgt_addr) = n;
    tgt_addr += 4;
```
Compares ECX to `n` (the total thread count). `0x81` is the CMP/ADD/SUB group opcode for register-with-immediate. The ModR/M `0xF9` = `11 111 001` → Mod=11 (register), Reg=7 (selects CMP among the group), R/M=ECX(1). Then 4 bytes of the immediate value `n`. This is a 6-byte instruction.

```c
    (*tgt_addr++) = 0x7c;        /* JL rel8 */
    (*tgt_addr++) = (char)(-10); /* backward offset to the MOV ECX, [RAX] above */
```
`JL` (Jump if Less, signed) with an 8-bit relative offset. The offset is signed and measured from the byte *after* the JL instruction. We need to jump back to the `MOV ECX, [RAX]` instruction, which is 10 bytes behind the end of JL:
- MOV ECX, [RAX]: 2 bytes
- CMP ECX, n: 6 bytes
- JL rel8: 2 bytes
- Total = 10 bytes, so offset = -10

`(char)(-10)` = `0xF6`. The full JL instruction is 2 bytes. If ECX < n (not all threads have arrived), execution jumps back to the MOV and re-reads the counter. If ECX >= n, execution falls through.

```c
    (*tgt_addr++) = 0x59;   /* pop rcx */
    (*tgt_addr++) = 0x58;   /* pop rax */
```
Restore RAX and RCX from the stack. Note the reverse order: last pushed, first popped.

---

### add_headeri

```c
static inline volatile char *add_headeri(volatile char *tgt_addr, volatile char *data_base)
```
Generates the function prologue — the setup code that runs at the start of every generated code block. Takes the current write pointer and the data area base address for this thread.

```c
    (*tgt_addr++) = 0xc8;         /* ENTER */
    (*(short *)tgt_addr) = 2048;  /* allocate 2 KB of local stack space */
    tgt_addr += BYTE2_OFF;
    (*tgt_addr++) = 0x00;         /* nesting level 0 */
```
`ENTER imm16, imm8` (opcode `0xC8` followed by a 16-bit size and an 8-bit nesting level) sets up a stack frame: saves RBP, moves RSP to RBP, then subtracts the size from RSP to allocate local space. It's the functional equivalent of the compiler-generated `push rbp; mov rbp, rsp; sub rsp, 2048`. We allocate 2048 bytes (2 KB) of local stack space as a safety margin. Nesting level `0` is standard for non-nested procedures.

```c
    (*tgt_addr++) = 0x53;   /* push rbx */
```
`0x53` is the short-form PUSH RBX opcode. We must save RBX because the System V ABI designates it as callee-saved — any function that uses it must restore it before returning. Since we're going to overwrite RBX with the data area address, we save it first.

```c
    tgt_addr = build_push_reg(REG_R12, 1, tgt_addr);
    tgt_addr = build_push_reg(REG_R13, 1, tgt_addr);
    tgt_addr = build_push_reg(REG_R14, 1, tgt_addr);
    tgt_addr = build_push_reg(REG_R15, 1, tgt_addr);
```
R12–R15 are also callee-saved per the ABI. We push them all to protect them from the random instructions that might use them. The `1` argument to `build_push_reg` triggers the REX.B prefix needed to access these extended registers.

```c
    tgt_addr = build_mov_imm_to_register(ISZ_8, (long)data_base, REG_EBX, tgt_addr);
```
Emits `MOV RBX, imm64` where the immediate is the 64-bit address of the thread's data area. This bakes the address into the generated code — when it executes, RBX will always point to the shared data region. All subsequent memory instructions use `[RBX + displacement]` as their address.

```c
    tgt_addr = emit_barrier_code(tgt_addr, (volatile int *)(comm_ptr + 8), nthreads);
```
**[Extra Credit]** Emits the start synchronization barrier into the code buffer. The barrier counter is at byte offset 8 within the COMM region (offsets 0 and 4 are used by the C-level barriers). After this barrier fires in the generated code, all `nthreads` processes will be at the same point in their execution before any random instruction runs.

---

### add_endi

```c
    tgt_addr = emit_barrier_code(tgt_addr, (volatile int *)(comm_ptr + 12), nthreads);
```
**[Extra Credit]** Emits the end synchronization barrier. Counter at byte offset 12 in the COMM region. After all threads hit this, we know every thread has finished its entire random instruction stream before any thread begins tearing down its stack frame.

```c
    tgt_addr = build_pop_reg(REG_R15, 1, tgt_addr);
    tgt_addr = build_pop_reg(REG_R14, 1, tgt_addr);
    tgt_addr = build_pop_reg(REG_R13, 1, tgt_addr);
    tgt_addr = build_pop_reg(REG_R12, 1, tgt_addr);
    (*tgt_addr++) = 0x5b;   /* pop rbx */
```
Restore all callee-saved registers in the exact reverse order they were pushed. Stack is LIFO — last in, first out.

```c
    (*tgt_addr++) = 0xc9;   /* LEAVE */
    (*tgt_addr++) = 0xc3;   /* RET */
```
`LEAVE` (`0xC9`) undoes the `ENTER` stack frame: restores RSP from RBP, then pops the saved RBP. `RET` (`0xC3`) pops the return address from the stack and jumps to it — returning control to `executeit()`.

---

### bind_to_cpu

```c
static void bind_to_cpu(int thread_id)
{
    cpu_set_t mask;
```
`cpu_set_t` is a bitmask type where each bit represents one logical CPU. Defined in `<sched.h>`.

```c
    CPU_ZERO(&mask);
```
Initializes the mask to all zeros (no CPUs selected). Must be called before `CPU_SET` to ensure no stale bits.

```c
    CPU_SET(thread_id, &mask);
```
Sets bit `thread_id` in the mask — this selects CPU number `thread_id`. So thread 0 goes to CPU 0, thread 1 to CPU 1, etc.

```c
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
```
Calls the Linux kernel to bind the calling process to the CPUs in `mask`. The first argument `0` means "the calling process." `sizeof(mask)` tells the kernel the size of our mask structure. Returns 0 on success, -1 on failure (with `errno` set).

The purpose: by pinning each thread to a different physical CPU, we ensure the shared data accesses actually travel across the CPU interconnect, stressing the cache coherency hardware. If all threads ran on the same core, the "sharing" would be handled trivially in L1 cache.

---

### executeit

```c
int executeit(funct_t start_addr)
{
    volatile int rc = 0;
    rc = (*start_addr)();
    return (0);
}
```
`(*start_addr)()` dereferences the function pointer and calls the generated code. The CPU jumps to the address of the first byte in the code buffer and begins executing the bytes we wrote there. Because we emitted a proper function prologue and epilogue, the generated code behaves like a real function call — it sets up its own stack frame, executes, and returns. `rc` is declared `volatile` to prevent the optimizer from eliminating it. The return value of the generated code is captured but not checked (always returning 0).

---

### sync_barrier

```c
static void sync_barrier(volatile int *counter, int nthreads)
{
    __sync_fetch_and_add(counter, 1);
    while (*counter < nthreads) { }
}
```
A C-level spin-wait barrier using the shared COMM area.

`__sync_fetch_and_add(counter, 1)` is a GCC built-in that atomically adds 1 to `*counter` and returns the old value. It compiles to a `LOCK XADD` instruction — the same instruction we manually encode in `emit_barrier_code`. This is the C equivalent of the barrier we generate in machine code.

`while (*counter < nthreads) { }` — busy-wait loop. The `volatile` qualifier on the pointer ensures the compiler re-reads `*counter` from memory on every loop iteration instead of caching it in a register (which would create an infinite loop).

This barrier is called twice from C code:
1. Before `build_instructions` — so all forked threads start generating at the same time
2. Before `executeit` — so no thread starts running its generated code while another thread is still writing bytes into its code buffer

The generated-code barriers (at COMM offsets 8 and 12) provide additional synchronization *within* the generated code's execution phase itself.

---

### build_instructions

```c
int build_instructions(volatile char *code_ptr, int thread_id)
{
    num_inst = 0;
    next_ptr = code_ptr;
```
Resets the instruction count to zero and sets the global write pointer to the start of this thread's code slice. Child processes each call this with their own `mptr_threads[i]` slice, so they write into non-overlapping regions.

```c
    next_ptr = add_headeri(next_ptr, (volatile char *)mdptr_threads[thread_id]);
```
Emits the prologue. Passes in `mdptr_threads[thread_id]` — the shared data area base address for this thread. Since all entries in `mdptr_threads` point to the same `mdptr`, every thread's generated code uses the same data area.

```c
    while (num_inst < target_ninstrs)
        emit_random_instruction();
```
The main generation loop. Each call to `emit_random_instruction` increments `num_inst` and writes bytes into the buffer via `next_ptr`. The loop continues until the target is reached.

```c
    next_ptr = add_endi(next_ptr);
    return (num_inst);
```
Emits the epilogue and returns the total instruction count to the caller (which prints it in the completion message).

---

### main — Argument Parsing

```c
int main(int argc, char *argv[])
{
    int seed = 0;
    int ibuilt = 0;
    char *logfilename = NULL;
```
Local variables. `seed` controls the random number generator. `ibuilt` receives the instruction count from `build_instructions`. `logfilename` is a pointer to the argv string (not a copy).

```c
    if (argc > 1) seed = (int)strtoul(argv[1], NULL, 0);
```
`strtoul` with base 0 auto-detects the format: `0x` prefix means hex, `0` prefix means octal, no prefix means decimal. Allows users to pass seeds like `0xDEADBEEF`.

```c
    if (argc > 2) {
        target_ninstrs = atoi(argv[2]);
        if (target_ninstrs < 1) target_ninstrs = 1;
    }
```
Parse the instruction count. Clamp to 1 minimum to avoid generating an empty code block (which would still have the prologue/epilogue but no actual test content).

```c
    if (argc > 3) nthreads = atoi(argv[3]);
    if (argc > 4) logfilename = argv[4];
```
Parse optional thread count and log filename.

```c
    if (nthreads > MAX_THREADS) {
        fprintf(stderr, "Sorry only built for %d threads, overriding your %d\n", MAX_THREADS, nthreads);
        nthreads = MAX_THREADS;
    }
```
Safety clamp. The `pid_task[]` and `mptr_threads[]` arrays are statically sized to `MAX_THREADS`. Exceeding that would write out of bounds.

```c
    srand(seed);
```
Seeds the random number generator. All the `pick_*` functions call `rand()`, which produces a deterministic sequence once seeded. The same seed always produces the same instruction sequence — useful for reproducing bugs.

---

### main — Log File

```c
    if (logfilename) {
        logfile = fopen(logfilename, "w");
        if (!logfile)
            perror("fopen logfile");
    }
```
Opens the log file for writing in the parent process. Because `fork()` duplicates the file descriptor table, every child process inherits the same open file handle pointing to the same file. When multiple children write to it, their outputs interleave in the file — which is why the log shows entries from both threads mixed together.

---

### main — Memory Allocation

```c
    test_info[DATA].pointer_addr = (volatile unsigned long *)mmap(
        (void *)0,
        (MAX_DATA_BYTES + PAGESIZE - 1),
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_ANONYMOUS | MAP_SHARED, 0, 0);
```
`mmap` arguments:
- `(void *)0` — let the kernel choose the address
- `(MAX_DATA_BYTES + PAGESIZE - 1)` — rounds up to the nearest page boundary
- `PROT_READ | PROT_WRITE | PROT_EXEC` — readable, writable, and executable
- `MAP_ANONYMOUS` — not backed by a file (zeroed pages from the kernel)
- `MAP_SHARED` — **critical**: changes are visible to other processes that have this mapping (i.e., forked children). Without this, each child would get a private copy-on-write page and could never see each other's writes
- `0, 0` — file descriptor and offset (not used for anonymous mappings)

Only one DATA region is allocated (not `nthreads` copies). All child processes share this single region, which is the whole point of the MP stress test.

```c
    mdptr = (volatile char *)test_info[DATA].pointer_addr;
    if (mdptr == (volatile char *)MAP_FAILED) {
        perror("Couldn't mmap DATA"); exit(1);
    }
```
`MAP_FAILED` is `(void *)-1`, the error return from `mmap`. `perror` prints the string plus a human-readable description of `errno`.

```c
    test_info[CODE].pointer_addr = (volatile unsigned long *)mmap(
        (void *)0,
        (MAX_INSTR_BYTES + PAGESIZE - 1) * nthreads,
        ...
```
The CODE region is sized for `nthreads` slices. Each slice is `MAX_INSTR_BYTES` bytes. Thread `i` writes into offset `i * MAX_INSTR_BYTES`. Since each thread writes to a different, non-overlapping slice, no synchronization is needed for the code writes themselves.

```c
    /* COMM region — holds the four barrier counters (4 ints = 16 bytes used) */
    test_info[COMM].pointer_addr = (volatile unsigned long *)mmap(
        (void *)0,
        (MAX_COMM_BYTES + PAGESIZE - 1),
        ...
```
One page for the communication area. The memory is zero-initialized by the kernel (all counters start at 0, which is exactly what the barriers expect).

```c
    setbuf(stdout, (char *)NULL);
    setbuf(stderr, (char *)NULL);
```
Disables buffering on stdout and stderr. Without this, output from different processes can get garbled because one process's buffer might not flush before another process writes. Unbuffered output is slower but correct for debugging.

---

### main — Fork Loop

```c
    for (i = 0; i < nthreads; i++) {
```
Iterates `nthreads` times. The parent runs through the entire loop, forking a new child on each iteration. Each child breaks out of the loop after doing its work.

```c
        next_ptr = (mptr + (i * MAX_INSTR_BYTES));
```
Computes the start address of this thread's code slice. Thread 0 starts at `mptr`, thread 1 at `mptr + MAX_INSTR_BYTES`, etc. The slices are adjacent in virtual address space but don't overlap.

```c
        mdptr_threads[i]    = (tptrs)mdptr;
        mptr_threads[i]     = (tptrs)next_ptr;
        comm_ptr_threads[i] = (tptrs)comm_ptr;
```
Records the three pointers for thread `i`. Note that `mdptr_threads[i]` is set to `mdptr` — the same address — for every `i`. This is deliberate: all threads share the single data area.

```c
        if ((pid = fork()) == 0) {
```
`fork()` creates an exact copy of the calling process. It returns:
- `0` to the newly created child process
- The child's PID (a positive integer) to the parent
- `-1` on failure

The `if` condition is only true in the child. The child immediately falls into its work block.

```c
            bind_to_cpu(i);
```
The child pins itself to CPU `i`. This must be called after `fork()` (not before), because `fork()` creates a new process that needs its own affinity setting.

```c
            sync_barrier((volatile int *)(comm_ptr + 0), nthreads);
```
C barrier 1 at COMM offset 0. All children must arrive here before any proceeds. This ensures no child starts generating code before all siblings have been forked and set up.

```c
            ibuilt = build_instructions((volatile char *)mptr_threads[i], i);
```
Calls the instruction generator for this thread. Passes the start of this thread's code slice and the thread ID. Returns the number of instructions generated.

```c
            start_test = (funct_t)mptr_threads[i];
```
Casts the start of the code buffer to a function pointer. This is perfectly legal in C when the memory has `PROT_EXEC` and contains valid machine code.

```c
            sync_barrier((volatile int *)(comm_ptr + 4), nthreads);
```
C barrier 2 at COMM offset 4. All children finish writing their code before any starts executing. Without this, thread 0 might start executing its code while thread 3 is still emitting the epilogue bytes into a different slice — not a problem for correctness (they use separate code slices), but ensuring all code is written before execution begins is cleaner and matches the intent.

```c
            executeit(start_test);
```
Jumps into the generated code. The CPU will execute our prologue, then the random instructions, then the epilogue (with two more barriers inside the generated code), then return here.

```c
            fprintf(stderr, "T%d generation program complete, instructions generated: %d\n", i, ibuilt);
            fflush(stderr);
            if (logfile) fclose(logfile);
            exit(0);
```
Child prints a completion message, flushes stderr, closes the log file, and exits with status 0 (success). The parent's `waitpid` will collect this exit status.

```c
        } else if (pid == -1) {
            perror("fork failed");
            exit(1);
```
If `fork()` returned -1, something went wrong (too many processes, out of memory, etc.). We print the error and exit.

```c
        } else {
            pid_task[i] = pid;
            fprintf(stderr, "child T%d started pid=%d\n", i, pid);
            fflush(stderr);
        }
```
Parent path. Saves the child's PID into `pid_task[i]` and loops back to fork the next child.

---

### main — Cleanup

```c
    for (i = 0; i < nthreads; i++)
        waitpid(pid_task[i], NULL, 0);
```
Parent waits for each child to exit. `waitpid(pid, NULL, 0)` blocks until the process with the given PID exits. The second argument would receive the exit status (we don't need it). The third argument `0` means no special options. Without this, child processes would become zombies (entries remaining in the process table) until the parent exits.

```c
    if (logfile) fclose(logfile);
```
Close the log file in the parent. Even though the children already closed their copies, the parent has its own open file descriptor that needs to be properly closed.

```c
    munmap((caddr_t)mdptr,    (MAX_DATA_BYTES  + PAGESIZE - 1));
    munmap((caddr_t)mptr,     (MAX_INSTR_BYTES + PAGESIZE - 1) * nthreads);
    munmap((caddr_t)comm_ptr, (MAX_COMM_BYTES  + PAGESIZE - 1));
```
Releases all three shared memory regions. `munmap` takes the base address and the exact size that was passed to `mmap`. The sizes must match precisely. `caddr_t` is `char *` — the cast is for type compatibility with older POSIX declarations.

```c
    return 0;
}
```
Parent returns 0 (success) to the shell.

---

## COMM Area Memory Layout Summary

The shared COMM page is used by four separate barriers. Each barrier uses one 4-byte `int` counter, initialized to 0 by `mmap`:

| Byte Offset | Used By | Purpose |
|---|---|---|
| 0–3 | C code (`sync_barrier`) | Barrier before code generation — all threads alive |
| 4–7 | C code (`sync_barrier`) | Barrier before execution — all code written |
| 8–11 | Generated code (`add_headeri`) | Start barrier — all threads begin random instructions together |
| 12–15 | Generated code (`add_endi`) | End barrier — all threads finish random instructions together |

---

## How the Complete Execution Flow Works

```
Parent:
  mmap CODE, DATA, COMM
  for i in 0..nthreads:
    fork()
    [Child i]:
      bind_to_cpu(i)
      sync_barrier(COMM+0)    <-- all children alive
      build_instructions()
        add_headeri():
          ENTER, save registers, MOV RBX=data_base
          [generated code] barrier at COMM+8
        emit 25 random instructions
        add_endi():
          [generated code] barrier at COMM+12
          restore registers, LEAVE, RET
      sync_barrier(COMM+4)    <-- all code written
      executeit():
        ENTER, save regs, MOV RBX        ]
        [generated] barrier (COMM+8)     ]  all threads
        random instruction 1             ]  executing
        random instruction 2             ]  simultaneously
        ...                              ]
        random instruction 25            ]
        [generated] barrier (COMM+12)    ]  all threads done
        restore regs, LEAVE, RET         ]
      exit(0)
    [Parent continues loop]
  waitpid for all children
  munmap all regions
```
