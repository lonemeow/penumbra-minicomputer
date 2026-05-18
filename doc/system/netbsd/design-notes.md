# NetBSD Port -- Design Notes

Architectural decisions and rationale for the Penumbra NetBSD port.
For current status, see `porting-status.md`.

## Reference Port

evbmips (ILP32 little-endian MIPS with software-managed TLB).
Shares key constraints: no hardware page table walker, software
TLB refill, no direct-mapped memory segments.  All code is
Penumbra-specific -- no `<mips/*.h>` includes.

## Machine Properties

- ILP32, little-endian, 4 KB pages
- 16 registers: R0=zero, R12=TP, R13=LR, R14=SP, R15=PC
- 2G/2G user/kernel VA split (kernel text at `0x8001_0000`)
- Software-managed TLB: 64-entry 2-way SA + 4-entry FA pinned
- Split I/D PIPT caches, write-through D-cache
- No FPU -- all floating-point via soft-float

## Virtual Memory Layout

```
0x0000_0000              unmapped null guard page
0x0000_1000              user text / data / bss
                         heap (grows up)
0x0400_0000  USRSTACK    stack top at 64 MB (grows down)
0x8000_0000              kernel VA start
0x8001_0000              kernel text (KERNEL_TEXT_BASE)
                         kernel data / bss / page tables
                         MMIO devices (pmap_map_device)
                         virtual_avail -> kernel VM pool (UVM)
0xFFFF_B000  VECTOR_VA    pinned slot 0: vector page
0xFFFF_C000  SCRATCH_VA   pinned slot 3: scratch window
0xFFFF_D000  PT_L2WIN_VA  pinned slot 2: L2 window (handler)
0xFFFF_E000  PT_L1_VA     pinned slot 1: current L1 table
0xFFFF_F000              unmapped guard
```

## Pinned Vector Page

The vector page is the single physical page at PA 0x0 that holds
everything exception entry needs to be guaranteed reachable: the
hardware vector table, all trap trampolines, the TLB miss handler,
`_trap_common` body, and the scratch cells those handlers use.
It is mapped at `VECTOR_VA = 0xFFFFB000` and pinned in TLB slot 0
(`PTLB_VECTOR`) at the very end of `locore.S`, before the MMU is
enabled.  Pinning means it never TLB-misses, which breaks what
would otherwise be an infinite "miss handler taking a miss" loop.

### Layout (4 KB)

```
offset  content                                     symbols
0x000   exception vector table (9 entries * 4B)     VA handlers at VECTOR_VA+0x80+…
0x080   real TLB miss handler (page-table walker)   _real_miss_handler
…       per-vector trap stubs                       _trap_entry_busfault, _timer, _tlbmiss, …
…       _trap_common (save trapframe, call trap())  _trap_common
…       trap_return (restore + ERET)                trap_return
…       trap scratch (R1–R4 save, ESR/EPC/… stash)  _trap_save_r1 … _trap_save_hw_fstat
…       TLB miss scratch (R1–R4 save, way counter)  _rmh_scratch, _rmh_way_counter
0xFFF   end                                         _real_handlers_end
```

All handler code and its scratch region are copied by `locore.S`
step 7 from the linked kernel image (the sources live in the
kernel `.text` between `_real_miss_handler` and `_real_handlers_end`)
to PA `0x0080`.  Because every handler accesses its scratch via
`[pc + label - .]`, the PC-relative offsets survive the copy
unchanged — handlers work correctly at either the link-time VA
(inside kernel .text) or at `VECTOR_VA + 0x80` after copy.

### Why PA 0

Physical address 0 is the architectural exception-vector location.
Exception entry fetches the handler VA from the vector table with
the MMU forced off (physical fetch of a 4-byte VA), so the table
must sit at PA 0 regardless of where the kernel is loaded.  Keeping
the handler code on the same page lets the trap stubs be reached
from the vector table with a 32-bit absolute VA stored at PAs
`0x00..0x20`, and lets TLB-miss recovery use PC-relative scratch
without needing any other mapping to be resolved.

### Mapping a vector-page PC back to the kernel binary

When an exception fires inside the pinned page you'll see an
EPC in the `0xFFFFB0xx`..`0xFFFFBFxx` range.  To find the
corresponding kernel source instruction:

```
kernel_elf_addr = addr(_real_miss_handler) + (EPC − 0xFFFFB080)
```

`_real_miss_handler` is the first handler symbol copied to
`VECTOR_VA + 0x80`.  Example: EPC `0xFFFFB264` with
`_real_miss_handler` at `0x8001039C` → kernel address
`0x80010580`.  Disassemble with
`llvm-objdump -d build/netbsd-kernel/MINIMAL/netbsd` and jump to
that address.

### When this bites you

Nested exceptions inside the pinned page are caught by
`_trap_common`'s double-fault detector (`ESR.S` set AND EPC in
`0xFFFFxxxx`) and halt via BREAK instead of looping.  On BREAK
the ISS prints R1–R14; the useful fields there are R2 (= EPC of
the inner trap, saved by `rdspr r2, epc`) and R4 (= inner cause
number, 0–9 from the vector stub).  R2 is what you feed into the
formula above.  Typical causes: kernel stack overflow (SP dropped
below the u-area and the trapframe STW TLB-missed), stale L1 in
the pinned slot after a botched `pmap_activate`, or a buggy PTE
install that left the faulting VA unmapped in the current pmap.

## Page Table Design

Always 2-level: L1 (1024 entries, 4 KB) --> L2 (1024 entries,
4 KB, covers 4 MB each).  No direct-map -- differs from MIPS
KSEG0/KSEG1, but matches the 74xx-feasible TLB design where
all memory access goes through the TLB.

The TLB miss handler is context-blind: it walks the 2-level
table via the pinned L1 (slot 1) and a transient L2 window
(slot 2).  No stack, no curproc, no metadata access needed.
This keeps the handler stateless and fast.

Physical pages without kernel VAs are accessed via the scratch
window (pinned slot 3, `SCRATCH_VA`).

### Own pmap vs MI pmap

Penumbra uses its own `pmap.c`, not the shared MI pmap from
`sys/uvm/pmap/pmap.c`.  The MI pmap is used by MIPS, RISC-V,
and PowerPC -- architectures that opted into a common
implementation with MD hooks (`pmap_md_page_syncicache`, etc.).

Reasons for a standalone pmap:

- The TLB refill handler has Penumbra-specific constraints
  (pinned slot window, 2-level walk, no KSEG0).
- Simpler to understand and debug during bring-up.
- The pinned scratch window pattern doesn't fit the MI pmap's
  assumptions about direct-mapped kernel memory.

The standalone pmap implements the full NetBSD pmap interface
directly: `pmap_enter`, `pmap_create`/`pmap_destroy`,
`pmap_activate`, `pmap_protect`/`pmap_remove`/`pmap_remove_all`,
`pmap_page_protect`, PV lists, ASID management, etc.

## ASID Management

Generational allocator: each pmap has an ASID (1--255) tagged
with a generation number.  On `pmap_activate`, a stale generation
triggers re-allocation.  When all 255 IDs are exhausted, the
generation bumps, the TLB is flushed, and allocation restarts
from 1.  Kernel always uses ASID 0 with G=1 (global) entries.

## I-Cache Coherency

Split I/D caches are not hardware-coherent.  After mapping
executable pages (demand paging, exec), data goes through
D-cache --> memory, but I-cache may hold stale lines.

Three invalidation points:
1. `pmap_enter()` flushes I-cache when `prot & VM_PROT_EXECUTE`
2. `pmap_procwr()` flushes for ptrace/exec code writes (MI hook)
3. Boot-time `wrsys` in locore.S (after vector page setup)

Hardware currently supports only full I-cache invalidation
(write to `SYSDEV_ICACHE` / `CACHE_INVAL_ALL`).  The unified
cache sysreg layout reserves `CACHE_INVAL_LINE` (reg 3) for a
future per-PA path; once it lands in `cache_vipt.sv`,
`pmap_procwr()` and exec paths can switch over without
changing the hook placement.  Write-through D-cache means
no writeback is needed before the I-cache flush.

## Trap Handler: Volatile State Stash

The trap handler has a subtle constraint: TLB misses during
trapframe allocation on the kernel stack could clobber ESR,
EPC, FAULT_ADDR, and FAULT_STATUS (hardware registers latched
on exception).  Solution: `_trap_common` (locore.S) copies these
into the pinned vector page's scratch area before any faultable
stack access.  The C `trap()` function reads from scratch, not
the hardware registers.

## Boot Stack Switch

ARM-style pattern: `penumbra_init()` runs on a 4 KB boot stack
(allocated in BSS by locore.S) and returns the new SP (lwp0's
12 KB USPACE kernel stack).  locore.S switches SP before calling
`penumbra_main()` --> `main()`.  The 4 KB boot stack overflows
at `-O0` + DIAGNOSTIC if used for `main()`.

## DDB: Minimal-Useful Subset

DDB on Penumbra is intentionally scoped to **inspect and resume**,
not single-step or breakpoint-plant.  The headline use case is
debugging stuck/parked LWPs: drop in via console BREAK or panic,
run `ps` and `bt /t <lwp>` to see *where* each LWP slept, then
`c` to continue.

What this gives up — single-step (`s`), breakpoints (`b`),
mnemonic disassembly — would each require independent work
(branch-target prediction + double-breakpoint emulation for `s`,
correct `BKPT_INST` encoding + I-cache flush after
`db_write_bytes` for `b`, ~200 mnemonics with 4 instruction
formats for the disassembler).  Skipping them keeps the MD
surface area at roughly 9 symbols
(`kdb_trap`, `cpu_Debugger`, `db_read_bytes`,
`db_write_bytes`, `db_active`, `ddb_regs`, `db_regs[]`/
`db_eregs`, `db_set_single_step`/`db_clear_single_step` as
stubs, `db_stack_trace_print`, `db_disasm` as stub).

### Stack unwinder: prologue scanning

Penumbra has no frame-pointer convention (R10 is callee-saved
but the compiler typically omits using it as FP), so walking
a stack requires identifying each function's saved-LR slot
from its prologue.  The scanner reads from
`db_search_symbol(pc)`'s function start and matches:

- `SUB r14, #imm`  — Format L, fixed bits `0xFFC00000` masked
  against match `0x41800000`.  Imm gives the frame size.
- `STW rN, [r14, #off]`  — Format M, fixed bits `0xF83C0000`
  masked against `0x90380000`.  Rd field gives the saved
  register; `off` gives its frame slot.  Only N in
  {5..10, 13} is treated as a prologue store; other
  registers terminate the scan.

The saved LR offset (N = 13) is the load-bearing one — it's
where the return address lives.  Without a `SUB r14`
instruction in the function, the function is a leaf and only
unwindable as the topmost frame (using the live LR from the
seed trapframe).

### Per-LWP seed

`bt /t <lwp_addr>` reads the LWP's `pcb_context` (the
`label_t` saved by `cpu_switchto`):

- `PC` seed  = `pcb_context.val[_JB_R13]` — the saved LR is
  already the return address into `mi_switch`, so we start
  "above" the asm boundary with no special case.
- `SP` seed  = `pcb_context.val[_JB_R14]`.
- LR is not live (set `lr_live = false`).

This works because `cpu_switchto` is reached via `bl`, which
writes LR — so its saved LR is a real return address, not a
synthesized one.  If the kernel ever context-switched via a
synchronous trap (it doesn't), the seed would need a
trap-frame special case.

### Termination markers

The unwinder stops with a `<...>` marker when it enters
hand-written assembly whose prologue this scanner can't
interpret: `cpu_switchto`, `lwp_trampoline`, `_trap_common`,
`trap_return`, `setjmp`, `longjmp`, or any PC in the pinned
vector page region (`0xFFFFxxxx`).  Also stops on bad SP,
implausible frame size (<4 or >4096 bytes), or after 64
frames as a safety cap.

### Symtab mapping

Kernel symbols arrive via `BTINFO_SYMTAB` (the bootloader's
libsa `LOAD_SYM` populates `marks[MARK_SYM..MARK_END]` and
the bootloader converts to virtual addresses).  But locore.S
maps only `[KERN_TEXT_VA, round_page(_end))`, leaving the
post-`_end` symtab region without PTEs.  `pmap_map_kernel_tail`
installs the missing entries from `cpu_startup`, *after*
`pmap_map_device` has remapped the UART off the pinned scratch
slot — otherwise `pmap_kenter_pa`'s scratch-window usage would
clobber the UART mapping mid-print and the next `printf` would
hang polling LSR through an L2 PTE.

The symtab region is mapped RW because `ksyms_addsyms_elf`
rewrites the ELF header in place (`ksyms_hdr_init` in
`sys/kern/kern_ksyms.c`).

### Fault recovery during debugger probes

DDB needs to read arbitrary kernel VAs, some of which may be
unmapped.  Rather than guard each `db_read_bytes` with manual
TLB-probe logic, we hook the kernel-mode fault paths in
`trap.c`: `EXC_TLB_MISS`, `EXC_TLB_PROT`, and `EXC_BUSFAULT`
check `db_recover != NULL` (set by the MI command loop via
`setjmp`) before calling `panic`.  If set, `longjmp` aborts
the in-progress command and returns to the `db>` prompt.

This is the standard NetBSD pattern (SPARC, MIPS use the same
approach).  Userland faults are unaffected — they only fall
into the user-mode signal delivery branch.

### What's left for a "full" DDB

If someone wants to extend this later:

1. **Real `db_disasm`**: ~200 mnemonics across 4 instruction
   formats.  Could factor from `llvm/lib/Target/Penumbra/
   Disassembler/` but in-kernel C is cleaner.
2. **Single-step**: Penumbra has no trace-trap bit, so it
   would need software emulation — compute next PC
   (fall-through + taken-branch target) and plant BREAK
   instructions at both, unplant on re-entry.
3. **Breakpoint planting**: correct `BKPT_INST` encoding,
   I-cache flush after `db_write_bytes` (split I/D caches
   aren't snooped).
4. **Auto-traceback at panic**: `db_panic` calls
   `db_stack_trace_print(__builtin_frame_address(0), have_addr=true, "", ...)`
   — currently `addr` is ignored when there's no `/t`
   modifier, so the auto-traceback prints "trapframe pc/sp
   not in kernel range" and gives up.  Adding a seed path
   for "addr is a stack pointer, find the enclosing
   function" would make panic-time traceback work.

## UART Mapping Timing

`pmap_map_device()` for the console UART must run before
`uvm_pageboot_alloc()`, because the latter snapshots
`virtual_avail` into UVM via `pmap_virtual_space()`.  After
that, pmap's local `virtual_avail` is stale.  The UART starts
life via the TLB scratch window (early boot), then gets a
permanent mapping via `pmap_map_device()` in `pmap_bootstrap()`.

## Syscall Convention

R11 (scratch register) carries the syscall number, not R1.
This preserves R1 as the first C argument, matching the ABI.
The SYSTRAP macro in `SYS.h` loads R11 and executes `SYSCALL`.

Return: R1=rval[0], R2=rval[1], carry flag = error indicator.
The carry-flag convention (like ARM/aarch64) lets userland
distinguish error returns from valid negative values without
ambiguity.  Two-value return (R1+R2) is needed by fork (child
gets R1=0, R2=1) and pipe (two fds).

## Atomics

- **Userland:** RAS (Restartable Atomic Sequences).
  `atomic_cas_32` uses plain load/cmp/store bracketed by RAS
  labels; `.init_array` constructor registers the sequence via
  `rasctl()`.  `userret()` checks `ras_lookup()` on every
  return to user mode.
- **Kernel:** interrupt-disable CAS (`RDSPR SR` / `DI` / op /
  `WRSPR SR`).  Safe on uniprocessor.

Generic CAS-based inc/dec/add/and/or built on top.  No-op
memory barriers (uniprocessor, no store buffer reordering).

## PIC Reach Limitation

Penumbra uses GOT-based PIC: global addresses are loaded
from a per-object GOT via a 5-instruction sequence
(`MOV PC + LLI/LUI %got_pcrel + ADD + LDW`), giving full
32-bit reach.  `MKPIC=yes` is set in `bsd.own.mk`.

Hand-written assembly (brk.S, sbrk.S, cerror.S) uses `#ifdef
__PIC__` guards: GOT-indirect under PIC, absolute LLI/LUI
otherwise.

## Dynamic Linker (ld.elf_so)

MD code in `libexec/ld.elf_so/arch/penumbra/`.  RELA format
(explicit addends); `EM_PENUMBRA` is in lld's RELA architecture
list (`Driver.cpp:getIsRela`).  Self-relocating PIE code needs
`--apply-dynamic-relocs` so lld writes addends to data sections
(bias computation reads pre-relocation values).

- `rtld_start.S`: entry point computes relocbase using a
  literal pool with PC-relative cross-section differences.
  Self-relocates via `_rtld_relocate_nonplt_self`, calls
  `_rtld()`, jumps to program entry with (cleanup, obj_main,
  ps_strings).
- `mdreloc.c`: handles NONE, RELATIVE, ADDR32, GLOB_DAT,
  JUMP_SLOT, COPY, IRELATIVE, TLS_DTPMOD32, TLS_DTPOFF32,
  TLS_TPOFF32.
- Eager PLT binding only (empty `_rtld_relocate_plt_lazy`).
  `_rtld_bind_start` stub present for future lazy binding.
- Functional: ld.elf_so loads, self-relocates, resolves symbols,
  and runs dynamically-linked binaries end-to-end.

## Userland Build Integration

Uses stock `toolchains::NetBSD` in clang with Penumbra
emulation flags in `NetBSD.cpp` (no custom toolchain class for
NetBSD target).  `PenumbraToolChain` retained for bare-metal
(`penumbra-unknown-none`) only.  Both drivers pass
`--apply-dynamic-relocs` to lld for PIE self-relocation support.

Key build flags: `MKSOFTFLOAT=yes`, `MKCXX=no`, `MKPIC=yes`,
`HAVE_SSP=no`, `HAVE_LIBGCC_EH=yes` (skip libunwind),
`USE_UNWIND=no` (no `_Unwind` support), `SLOPPY_FLIST=yes`
(tolerate missing toolchain binaries).

## NetBSD Tree Modifications

Files modified outside `sys/arch/penumbra/`:

**Build system:**
- `build.sh` -- penumbra in `valid_MACHINE_ARCH`
- `share/mk/bsd.own.mk` -- TOOLCHAIN_MISSING, HAVE_LLVM,
  MACHINE_GNU_PLATFORM, HAVE_SSP, jemalloc LG_QUANTUM
- `share/mk/bsd.endian.mk` -- penumbra in little-endian list
- `tools/Makefile`, `tools/headerlist` -- tool build integration

**Common libraries:**
- `common/lib/libc/arch/penumbra/string/` -- assembly memcpy,
  memset, memcmp, strlen, strcmp, strcpy (shared kernel+userland)
- `common/lib/libc/arch/penumbra/atomic/` -- CAS, RAS init,
  generic + 64-bit atomic ops

**Userland libraries:**
- `lib/csu/arch/penumbra/` -- C startup (crt0, crti, crtn,
  crtbegin, crtend)
- `lib/libc/arch/penumbra/` -- SYS.h, cerror, syscall wrappers,
  setjmp, softfloat, gdtoa, makecontext, mulsi3
- `lib/libpthread/arch/penumbra/pthread_md.h` -- minimal stubs
- `lib/libkvm/kvm_penumbra.c` -- kvm support

**Dynamic linker:**
- `libexec/ld.elf_so/arch/penumbra/` -- rtld_start.S, mdreloc.c,
  Makefile.inc (not yet buildable, blocked on GOT-based PIC)
- `libexec/ld.elf_so/Makefile` -- penumbra in arch whitelist
- `lib/libexecinfo/Makefile` -- USE_UNWIND `?=` override

**Other:**
- `etc/etc.penumbra/` -- MAKEDEV.conf, Makefile.inc, ttys
- `crypto/external/bsd/openssl/lib/libcrypto/arch/penumbra/` --
  ec.inc (no 64-bit NIST curves), sha.inc
- `external/bsd/jemalloc/` -- LG_QUANTUM=3 type header
- `sys/kern/init_main.c` -- minor modification
- `sys/lib/libkern/arch/penumbra/Makefile.inc`
- `usr.bin/xlint/arch/penumbra/targparam.h`
