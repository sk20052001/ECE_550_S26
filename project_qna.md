# ECE 550 Post-Silicon Validation — Project Part 3: Viva Q&A

All answers reference exact line numbers and byte values from the actual code.

---

## x86 Encoding Fundamentals

---

**Q1. Walk me through exactly what bytes get written for `LOCK XADD [RBX+8], ECX` in 32-bit mode. Justify every byte.**

Five bytes total:

```
F0  0F  C1  4B  08
```

- `0xF0` — LOCK prefix. Emitted first because `use_lock=1`. See `build_xadd` line 387: `(*tgt_addr++) = LOCK_PREFIX`.
- `0x0F` — two-byte escape. All "extended" instructions that were added after the original 8086 ISA live behind this prefix. Line 393.
- `0xC1` — XADD opcode for 16/32/64-bit form. Line 394: `(mov_size == 1) ? 0xc0 : 0xc1`.
- `0x4B` — the ModR/M byte. Build it: Mod=01 (8-bit displacement, since 8 fits in −128..127), Reg=ECX=001, R/M=EBX=011. Binary: `01 001 011` = `0x4B`. Line 404: `((0x1 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg)`.
- `0x08` — the 8-bit signed displacement. Line 406: `(*(char *)tgt_addr) = (char)displacement`.

---

**Q2. The ModR/M byte for `[RBX+8], ECX` is `0x4B`. Decode it — what is Mod, Reg, and R/M?**

`0x4B` in binary is `0100 1011`.

```
Bits 7–6 → Mod = 01  → 8-bit signed displacement follows the ModR/M byte
Bits 5–3 → Reg = 001 → ECX (the register operand, source in this case)
Bits 2–0 → R/M = 011 → EBX (the base register for the memory address)
```

Mod=01 means the effective address is `[EBX + disp8]`. The byte immediately after the ModR/M byte is the displacement (`0x08` = 8), giving `[EBX+8]`. Mod=11 would mean register-to-register with no memory at all. Mod=00 would mean `[EBX]` with no displacement.

---

**Q3. Why does XADD need a two-byte escape (`0x0F 0xC1`) but XCHG only needs one byte (`0x87`)?**

Age. XCHG is an original 8086 instruction from 1978 — it was assigned the single-byte opcode `0x86`/`0x87` when the entire opcode space was wide open. By the time XADD was introduced on the 486 in 1989, the single-byte opcode space (0x00–0xFF) was largely exhausted. Intel had previously reserved `0x0F` as a two-byte escape prefix to extend the ISA without breaking backward compatibility. XADD was slotted into this extended space as `0x0F 0xC0`/`0x0F 0xC1`. This is why you see the pattern `(*tgt_addr++) = 0x0f; (*tgt_addr++) = 0xc1` on lines 393–394 for XADD, but just `(*tgt_addr++) = 0x87` on line 438 for XCHG.

---

**Q4. You use `Mod=01` for non-zero displacements. When would you be forced to use `Mod=10` instead?**

When the displacement exceeds the range of a signed byte (−128 to +127). Our three displacements are 0, 8, and 32 — all fit comfortably in a signed byte. Mod=01 keeps the displacement to 1 byte, making the whole instruction shorter. Mod=10 uses a 4-byte signed displacement, adding 3 extra bytes to the instruction for no benefit in our case.

The code handles this in the `else` branch of every memory encoder, e.g., `build_xadd` lines 410–414:
```c
(*(char *)tgt_addr) = ((0x2 << MODRM_SHIFT) + (src_reg << REG_SHIFT) + base_reg);
tgt_addr += BYTE1_OFF;
(*(int *)tgt_addr) = (int)displacement;
tgt_addr += BYTE4_OFF;
```
That path would trigger for displacements like 200 or 5000.

---

**Q5. What happens if you emit a `LOCK` prefix before a `MOV` register-to-register instruction?**

The CPU raises a `#UD` (Invalid Opcode Exception). LOCK is only legal when the *destination* operand is in memory. A list of valid LOCK targets: ADD, AND, BTC, BTR, BTS, CMPXCHG, DEC, INC, NEG, NOT, OR, SBB, SUB, XOR, XADD, XCHG. A register-to-register MOV has no memory operand, so LOCK before it is explicitly illegal per the Intel SDM. The OS catches the `#UD` and delivers `SIGILL` to the process, killing it. This is why we only emit LOCK inside `build_xadd` and `build_xchg`, never inside the MOV builders.

---

**Q6. Why does `pick_mov_size_atomic()` cap at 32-bit and exclude ISZ_8?**

Two reasons. First, the professor's spec says `r/m{8,16,32}` for XADD and XCHG — 64-bit is explicitly out of scope. Second, there is a hardware alignment constraint: a 64-bit atomic operation (`LOCK XADD qword ptr [mem]`) requires the memory address to be 8-byte aligned, otherwise the CPU raises `#GP` (General Protection Fault). While our mmap base addresses happen to be page-aligned (so offsets 0, 8, and 32 would actually be 8-byte aligned), the conservative choice of capping at 32-bit avoids any alignment risk and keeps the test predictable. See `pick_mov_size_atomic` lines 114–118.

---

## REX Prefix

---

**Q7. What are the four bits in the REX prefix and what does each do?**

The REX byte is `0100 WRXB` in binary (`0x40` to `0x4F`).

- **W (bit 3, 0x8):** Promotes the operand to 64-bit width. `REX_W = 0x8` — combined as `0x40 | 0x08 = 0x48`. Used whenever we need a 64-bit MOV or other 64-bit operation.
- **R (bit 2, 0x4):** Extends the ModR/M Reg field. Allows the Reg field to address R8–R15 instead of RAX–RDI. `REX_R = 0x4`. Not used in our code directly (we don't put extended registers in the Reg field).
- **X (bit 1, 0x2):** Extends the SIB Index field. Only relevant when a SIB byte is present. `REX_X = 0x2`. Not used in our code (we don't use SIB addressing).
- **B (bit 0, 0x1):** Extends the R/M field, or the register embedded in the opcode (for PUSH/POP). `REX_B = 0x1`. We use this for pushing R12–R15 in the prologue.

We use REX.W most — emitted before every 64-bit MOV including the `MOVABS RBX, data_base` in `add_headeri` and the `MOVABS RAX, barrier_counter` in `emit_barrier_code`.

---

**Q8. You push R12 with bytes `0x41 0x54`. What is each byte, and what happens without `0x41`?**

- `0x41` = `REX_PREFIX | REX_B` = `0x40 | 0x01`. The REX.B bit extends the opcode-embedded register field.
- `0x54` = `0x50 + REG_R12` = `0x50 + 4 = 0x54`. The PUSH opcode family is `0x50` through `0x57`, with the register encoded in the low 3 bits.

Without `0x41`, the processor decodes `0x54` as `PUSH RSP` — because REG_ESP = 4 and REG_R12 = 4 have the same 3-bit encoding. REX.B is the one bit that tells the CPU "this is R12, not RSP." This is shown in `build_push_reg` lines 138–144:
```c
if (x86_64f) {
    (*(char *)tgt_addr) = (REX_PREFIX | REX_B);   // 0x41
    tgt_addr += BYTE1_OFF;
}
(*(char *)tgt_addr) = 0x50 + reg_index;           // 0x54 for R12
```
The `x86_64f=1` argument in `add_headeri` triggers the REX.B emission.

---

**Q9. For `MOV RAX, imm64` you emit `0x48 0xB8` then 8 bytes. Why does it use the same `0xB8` opcode as `MOV EAX, imm32`?**

In 32-bit mode the default operand size is 32 bits, so `0xB8` naturally encodes a 32-bit immediate. When the CPU encounters a REX.W prefix (`0x48`) before `0xB8`, it re-interprets the entire instruction: the operand width becomes 64-bit, and the immediate field grows from 4 bytes to 8 bytes. The opcode byte itself (`0xB8`) doesn't change — the REX.W prefix is the modifier. This is a key design principle of the x86_64 extension: REX prefixes augment existing encodings rather than introducing entirely new opcode bytes for every 64-bit variant.

In `build_mov_imm_to_register` lines 253–257:
```c
(*tgt_addr++) = (REX_PREFIX | REX_W);  // 0x48 — enables 64-bit mode
(*tgt_addr++) = 0xb8 + dest_reg;       // opcode, same as 32-bit form
(*(long *)tgt_addr) = imm_value;       // 8 bytes of immediate (not 4)
tgt_addr += ISZ_8;
```

---

## Barrier — The Hardest Section

---

**Q10. Explain the spin loop byte count. Why is the JL offset exactly −10?**

The JL instruction's relative offset is measured from the byte *immediately after* the JL instruction. We need to jump back to `MOV ECX, [RAX]`, which is the start of the spin loop. Count the bytes between that MOV and the end of JL:

```
Instruction           Bytes
mov ecx, [rax]        2  (0x8B 0x08)
cmp ecx, n            6  (0x81 0xF9 + 4-byte immediate)
jl  rel8              2  (0x7C + offset byte)
                     --
Total                10
```

The reference point is the byte after JL (byte 0). The MOV starts 10 bytes before that, so offset = −10. In two's complement signed byte: −10 = `0xF6`, stored as `(char)(-10)`. This is on line 393: `(*tgt_addr++) = (char)(-10)`.

If the offset were −8 instead, the jump would land 2 bytes into the CMP instruction — the CPU would try to decode the middle of the 4-byte immediate as a new opcode, producing garbage. If it were −12, the jump would land 2 bytes before the MOV, hitting some earlier byte in the prologue.

---

**Q11. The barrier LOCK XADD uses ECX (32-bit). The counter is a 32-bit `int`. Is there a correctness risk on a 64-bit system?**

No, and here's why. When you write to ECX, the CPU automatically zero-extends the result into the upper 32 bits of RCX (this is a mandatory behavior on x86_64 — any write to a 32-bit register zeroes bits 63:32). The counter itself is declared as `volatile int *` — a 32-bit value — and we only ever read and write it as 32-bit. The LOCK XADD with ECX is operating on exactly 32 bits of the shared counter, which is correct.

The spin loop then reads with `MOV ECX, [RAX]` — also 32-bit — and compares with `CMP ECX, n` — also 32-bit. The upper half of RCX is never involved and never matters. There is no truncation or sign-extension issue.

---

**Q12. The 64-bit counter address is baked into the generated code as a literal. What breaks if ASLR moves it?**

Nothing, because the address cannot move after it is baked. The sequence is:

1. `mmap()` is called in `main()` — the kernel assigns a virtual address to the COMM region (e.g., `0x7ffff7fa9000`). This address is fixed for the lifetime of the process.
2. `comm_ptr` is set to that address.
3. `fork()` copies the parent's page table — children share the same virtual-to-physical mapping. The COMM region sits at the same virtual address in every child.
4. `build_instructions()` runs in each child, calling `emit_barrier_code` with `comm_ptr + 8`. At this point we compute `(unsigned long)barrier_counter` and write it as a literal: line 363 `(*(unsigned long *)tgt_addr) = (unsigned long)barrier_counter`.

ASLR randomizes addresses at *program load time* — before `main()` runs. Once `mmap()` returns, the address is stable. There is no window where ASLR could change it after we've baked it. The baked address is always correct.

---

**Q13. After a barrier releases, the counter stays at `nthreads`. Could you reuse the same counter for a second barrier?**

No. After the first barrier releases, the counter is permanently stuck at `nthreads`. If you reused it, every thread would immediately see `counter >= nthreads` on entry, skip the LOCK XADD (well, they'd still do it, incrementing beyond `nthreads`), and the spin loop would exit instantly — no synchronization at all.

This is exactly why we use four separate counters at COMM offsets 0, 4, 8, and 12. Each starts at 0 (guaranteed by `mmap`'s zero-initialization), gets incremented to `nthreads`, and is then never touched again. The layout is documented in `main()` lines 669–672 and in `add_headeri`/`add_endi`. The end barrier counter at `comm_ptr+12` is a completely separate `int` from the start barrier counter at `comm_ptr+8`.

---

**Q14. Why do you `push RAX` and `push RCX` at the start of the barrier instead of just using different registers?**

The barrier is injected into the middle of a running function (the generated code's prologue). At the point `emit_barrier_code` runs, RAX and RCX are live — they may contain values the surrounding code depends on. In fact, `executeit` calls the generated code as a normal function, so RAX is used for the return value and the ABI says the caller owns RAX and RCX.

We can't just pick "other" registers because all the general-purpose registers are either caller-saved (so the caller might be using them) or callee-saved (and we haven't saved them yet at the point of the start barrier). The safest approach is to save exactly what we use, restore it, and leave everything else untouched. Pushes and pops are cheap (they go to the L1 cache), and the barrier already involves spinning for potentially millions of cycles — two extra push/pop pairs are negligible.

Lines 357–358: `(*tgt_addr++) = 0x50; (*tgt_addr++) = 0x51;` — save RAX and RCX.
Lines 396–397: `(*tgt_addr++) = 0x59; (*tgt_addr++) = 0x58;` — restore RCX then RAX (reverse order, LIFO).

---

**Q15. The spin loop reads the counter with a plain `MOV ECX, [RAX]`, not `LOCK MOV`. Is this safe? Can a thread ever spin forever?**

It is safe, and a thread cannot spin forever, for two reasons.

**Memory model:** x86 has a strongly ordered memory model (TSO — Total Store Order). Once a LOCK XADD commits a store to the cache line, that store is globally visible. All other cores will see the updated value in finite time (hardware cache coherency — MESI protocol). A plain load (`MOV`) is sufficient to observe it; no LOCK is needed on the reader side.

**Compiler model:** The counter pointer is declared `volatile int *`. The `volatile` qualifier forces the compiler to re-issue a real memory read on every loop iteration instead of hoisting `*counter` into a register and checking the same cached value forever. Without `volatile`, a smart optimizer could turn `while (*counter < n) {}` into `if (*counter < n) while(1) {}` — an infinite loop. The `volatile` prevents that.

So: the hardware guarantees eventual visibility, and `volatile` guarantees the compiler doesn't cache the load. A thread will always eventually observe `counter == nthreads` and exit.

---

## Multi-Process and Shared Memory

---

**Q16. Why is `MAP_SHARED` critical for the DATA region? What would happen with `MAP_PRIVATE`?**

`MAP_SHARED` maps the *same physical page frames* into every process's virtual address space. A store from child 0 physically updates those page frames, and child 1's virtual address immediately reads the same physical bytes — because it's literally the same RAM.

`MAP_PRIVATE` uses copy-on-write. The first time a child writes to a page, the kernel intercepts the fault, duplicates that physical page, and remaps the child's virtual address to the new private copy. From that point on, the child has its own isolated copy. Other children never see those writes.

With `MAP_PRIVATE`, every thread would be talking to its own private data area. Thread 0's `MOV [RBX+0], EAX` would write to thread 0's private copy; thread 1 would never see it. The "multi-process" test would degrade to N independent single-process tests. The cache coherency hardware — which is exactly what we're trying to stress — would never be exercised across cores. This is noted in the comment at `main()` line 685: *"All threads share the SAME data area (mdptr) — intentional."*

---

**Q17. All threads share the same `mdptr`. Thread 0 does `MOV [RBX+0], EAX` and thread 1 does `MOV [RBX+0], ECX` simultaneously with no locking. Is this a bug?**

No — it is the entire purpose of the tool. This is intentional and correct from a post-silicon validation perspective.

A **software** program doing this would have undefined behavior and a data race. But this is not a software application — it is a **hardware stress test**. The goal is to generate exactly the kind of racy, concurrent memory access patterns that the hardware must handle correctly at the microarchitectural level. If the CPU's cache coherency protocol, store buffer, or write-combining logic has a silicon bug, the way to expose it is to have multiple cores hitting the same cache line with unsynchronized reads and writes.

The final values in memory are meaningless and are never inspected. What matters is that the CPU executed each instruction correctly (right address, right bytes committed to the right cache line) — not what the final memory state is. This is also why we add XADD and XCHG: to exercise both the locked (coherent) and unlocked (potentially racing) paths through the memory subsystem.

---

**Q18. Why call `bind_to_cpu()` in the child after `fork()`, not in the parent before forking?**

Two reasons.

**Affinity is per-process:** CPU affinity is a kernel attribute attached to a specific process ID. The parent calling `sched_setaffinity` with its own PID (`0`) sets affinity only for itself. The child created by `fork()` starts with its own PID and inherits the *default* system affinity (unrestricted), not the parent's custom affinity. To pin a child to a CPU, the child must call `sched_setaffinity` itself after it exists.

**We need different CPUs per child:** Thread 0 should go to CPU 0, thread 1 to CPU 1, etc. In the fork loop, each iteration creates one child. After `fork()` returns 0 (child path), `i` still holds the loop index — that's the thread ID. So `bind_to_cpu(i)` correctly pins that specific child to CPU `i`. If done in the parent, there's no way to pre-configure different CPUs for children that don't exist yet. See lines 697–699 in `encodeit.c`.

---

**Q19. You have four barrier counters at COMM offsets 0, 4, 8, 12. What is each one for, and why four instead of one?**

| Offset | Used by | Synchronizes |
|---|---|---|
| 0 | `sync_barrier` (C code) | All children alive and ready to start generating (currently commented out) |
| 4 | `sync_barrier` (C code) | All children finished writing their code buffer before anyone calls `executeit` |
| 8 | `emit_barrier_code` in `add_headeri` (generated x86) | All threads reach the start of the random instruction stream together |
| 12 | `emit_barrier_code` in `add_endi` (generated x86) | All threads finish the random instruction stream before any unwinds the stack |

Four are needed because each barrier counter is a one-shot: it starts at 0, climbs to `nthreads`, and stays there forever. Once a counter reaches `nthreads`, it is "saturated" — any thread checking it will immediately see `counter >= nthreads` and pass through without waiting. Reusing a saturated counter for a second barrier would make the second barrier a no-op. Four separate counters (four separate `int`s in the zero-initialized COMM page) allow four independent synchronization points.

---

**Q20. `sync_barrier` uses `__sync_fetch_and_add`. What instruction does that compile to, and how is it different from the manually encoded `LOCK XADD`?**

`__sync_fetch_and_add(counter, 1)` compiles to `LOCK XADD [mem], reg` — exactly the same instruction we manually encode in `emit_barrier_code`. They are functionally identical at the machine-code level.

The difference is purely syntactic and toolchain-level:
- `__sync_fetch_and_add` is a GCC compiler built-in. The compiler selects the target register, generates the ModR/M byte, and emits the bytes. You write C; the compiler does the encoding.
- `emit_barrier_code` writes the raw bytes manually. We pick `0xF0 0x0F 0xC1 0x08` ourselves, bake the counter address as a 64-bit immediate into the instruction stream, and compute the spin loop offset by hand.

We use the built-in in `sync_barrier` (line 532) because it's cleaner C code. We use raw bytes in `emit_barrier_code` because we're generating machine code into a buffer at runtime — the C compiler can't write those bytes for us, so we have to do it ourselves.

---

## Design Choices and Trade-offs

---

**Q21. Why is RBX permanently reserved? What happens if a random instruction overwrites it?**

Every memory instruction in the random stream hardcodes `REG_EBX` as the base register — see `emit_random_instruction` lines 219, 225, 233, 243. The generated code uses `[RBX + displacement]` as the memory address for every load and store.

If a random `MOV RBX, 0x7F000000` ran (from `pick_immediate`), then the very next `MOV [RBX+8], EAX` would attempt to write to address `0x7F000008` — which is likely unmapped. The kernel would deliver `SIGSEGV` and kill the child.

The fix is at `pick_general_reg` (lines 131–132): neither the `regs_word_dword_qword` array nor the `regs_byte` array includes `REG_EBX`. It is never chosen as a source or destination. There is also a redundant guard in `pick_dest_reg` (line 148): `while ((dest_reg == source_reg) || (dest_reg == REG_EBX))` — even if `pick_general_reg` were changed to include EBX, the dest picker would retry.

---

**Q22. For 8-bit operations you exclude some registers. Why can't you freely use all 8 in byte mode without a REX prefix?**

In 32-bit mode (no REX prefix), the 3-bit register field 0–7 maps to *different physical registers* depending on operand size:

```
Encoding    32/64-bit    8-bit (no REX)
   0         EAX / RAX      AL
   1         ECX / RCX      CL
   2         EDX / RDX      DL
   3         EBX / RBX      BL
   4         ESP / RSP      AH  ← HIGH byte of AX, NOT SPL
   5         EBP / RBP      CH  ← HIGH byte of CX
   6         ESI / RSI      DH  ← HIGH byte of DX
   7         EDI / RDI      BH  ← HIGH byte of BX
```

Without a REX prefix, encodings 4–7 in byte mode access the *high-byte* registers (AH, CH, DH, BH), not the low-byte extensions (SPL, BPL, SIL, DIL). Accessing SPL, BPL, SIL, DIL in byte mode requires a REX prefix (even a bare `0x40` with no bits set).

Our `regs_byte` pool on line 132 uses only `{EAX, ECX, EDX, ESI}` (encodings 0, 1, 2, 6). Encoding 6 in byte mode is DH — a quirky but valid byte register that doesn't crash. Encodings 4 (AH) and 7 (BH) are excluded to keep the pool simple, and encoding 3 (BL) is excluded because that's EBX which we reserve.

---

**Q23. MFENCE, SFENCE, and LFENCE all start with `0x0F 0xAE`. What does the third byte do?**

The `0x0F 0xAE` two-byte escape is a *group* that contains multiple instructions. The third byte acts as an opcode extension, encoded in the form of a fixed ModR/M byte (Mod=11 = register form, no memory operand) with the Reg field selecting the specific instruction:

```
0x0F 0xAE 0xE8  → LFENCE: 11 101 000 → Reg=5 → "L"oad fence
0x0F 0xAE 0xF0  → MFENCE: 11 110 000 → Reg=6 → "M"emory fence
0x0F 0xAE 0xF8  → SFENCE: 11 111 000 → Reg=7 → "S"tore fence
```

They live behind `0x0F 0xAE` because Intel placed all "memory control" type instructions in that group when SFENCE was introduced with SSE (Pentium III, 1999) and MFENCE/LFENCE with SSE2 (Pentium 4, 2000). The `0xAE` slot was unallocated in the original `0x0F xx` extension space, making it an available home for new system-level memory instructions. See `build_mfence`, `build_sfence`, `build_lfence` at lines 474–496.

---

**Q24. Memory-form XCHG has an implicit LOCK per the Intel manual. So what is the effect of randomly emitting `0xF0` before it?**

According to Intel SDM, a memory-form XCHG always asserts the bus lock signal, with or without the explicit `0xF0` prefix. So `0xF0 0x87 ModR/M` and `0x87 ModR/M` are both atomic — the LOCK prefix is redundant.

The CPU encounters a redundant but legal prefix. It is not an error. The CPU's prefix decoder processes `0xF0`, notes the LOCK intent, and then sees that XCHG already implies it — the behavior is identical to the no-prefix form.

From a post-silicon validation perspective, the value of emitting the explicit prefix is: (1) it exercises the prefix decoder's handling of this specific redundancy — a chip bug in the "LOCK already implied" path would only surface if you send the redundant byte; (2) it matches the professor's requirement to "randomize with/without a lock prefix" for XCHG specifically. See `build_xchg` line 431 and the note in the comment at line 424.

---

**Q25. Why is it more valuable to have all threads share the same data area rather than each having a private region?**

Because the goal is to stress the CPU's **cache coherency hardware**, and cache coherency only does real work when multiple cores compete for the same cache lines.

When thread 0 writes to `[RBX+0]` and thread 1 also writes to `[RBX+0]` (the same physical address), the cache coherency protocol (MESI on x86) must:
1. Invalidate thread 1's cached copy of that cache line after thread 0 modifies it.
2. Send the updated cache line across the CPU interconnect to thread 1.
3. Handle simultaneous writes (both threads try to grab exclusive ownership of the same cache line at the same moment).

This "invalidate, transfer, re-acquire" traffic across the interconnect is exactly what exercises the hardware's coherency logic. Post-silicon bugs in snoop filters, invalidation broadcasts, or store buffer ordering show up under exactly this pattern.

If each thread had its own private data area, thread 0's writes would never touch thread 1's cache lines. Each thread would work entirely within its L1/L2 cache. The interconnect would be nearly idle. A coherency bug could exist in silicon and this test would never detect it. The shared data area is what transforms this from a single-core exercise into a genuine multi-core stress test. See the comment at line 685: *"This causes threads to read/write the same cache lines, which is exactly the MP coherency stress we're after."*

---

## That One Question Designed to Catch You Off Guard

---

**Q26. What does the program do with `NTHREADS=0`?**

With `nthreads=0`, the program:

1. Parses `argv[3]` as `0`, sets `nthreads = 0`. The bounds check on line 595 only fires if `nthreads > MAX_THREADS`, so it passes.
2. `srand(seed)` is called.
3. All three `mmap()` calls execute successfully. `mptr`, `mdptr`, `comm_ptr` are set.
4. The fork loop `for (i = 0; i < nthreads; i++)` — `nthreads` is 0, the condition is immediately false. **Zero iterations.** No children are forked. `build_instructions` is never called. `emit_barrier_code` is never called. No generated x86 code is ever written.
5. The `waitpid` loop also runs zero iterations.
6. All three `munmap` calls clean up the regions.
7. Parent returns 0 (success).

The program exits silently with no output beyond the initial `printf` line, no instructions generated, and no errors. It is a valid no-op execution.

**Bonus edge case:** If somehow `emit_barrier_code` *were* called with `n=0` (it isn't, but hypothetically): the LOCK XADD increments the counter from 0 to 1, then the spin reads `ecx = 1`. The CMP checks `1 < 0` — false, since 1 is not less than 0. The JL is not taken. The barrier exits immediately without any synchronization. It effectively becomes a single atomic increment with no waiting — not a crash, just meaningless synchronization.
