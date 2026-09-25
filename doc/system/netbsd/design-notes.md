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
- Software-managed TLB: 64-entry 2-way SA + 8-entry FA pinned
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
0xFFFF_9000  SCRATCH2_VA   pinned slot 5: second page window
0xFFFF_A000  PT_USER_L1_VA pinned slot 4: current user L1
0xFFFF_B000  VECTOR_VA    pinned slot 0: vector page
0xFFFF_C000  SCRATCH_VA   pinned slot 3: scratch window
0xFFFF_D000  PT_L2WIN_VA  pinned slot 2: L2 window (handler)
0xFFFF_E000  PT_KERN_L1_VA pinned slot 1: kernel L1 (permanent)
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
`llvm-objdump -d build/netbsd-obj/sys/arch/penumbra/compile/MINIMAL/netbsd` and jump to
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

## TLB Locality

The TLB reaches 64 entries times 4 KB, so a kernel that touches more
than 256 KB of pages refills constantly.  On a shell and filesystem
workload the refill handler is 39% of every instruction executed --
72.9M refills at 53 instructions each -- which makes where code and
data *land* a first-order concern rather than a tuning detail.

### The layout lottery

A page's TLB set is bits 16:12 of its address, so a function's set is
decided by where the linker put it.  Two hot functions 128 KB apart
share a set; with two ways, a third one evicts the others on every
visit.  Nothing in the build controls this, so an unrelated change
that shifts code re-rolls it.

Measured: adding 160 bytes to the kernel moved `memcpy` from
`0x802aaf60` to `0x802ab000`, a different page and a different set.
Its old page took 5,421 refills, its new one 385,858, and the two
benchmarks whose hot code shared that set slowed 16% and 22%.  The
change that caused it had nothing to do with `memcpy`.

This is why a change is judged on *both* the work it does and the
refill count, never on one total: the two move independently, and a
build can lose more to a re-roll than the change itself saved.

### Trap page: code variants, not parameters

The TLB miss handler carries the set count, way count and page size as
instruction immediates.  Making them parameters would cost a load per
constant on the most frequently executed path in the kernel: at five
extra instructions per refill that is 3.7% of all instructions
executed, an order of magnitude more than lowering every avoidable
call in the kernel would save.

So each supported geometry gets its own hand-written trap page image,
and boot copies the one that matches.  `locore.S` reads `MMU_CONFIG`
before installing the handler, selects the image, and refuses anything
it has no image for -- printing to the console UART, which is directly
addressable because the MMU is not yet enabled, and spinning rather
than executing `BREAK`, because the exception vectors are not
installed at that point.

The images are linked after all live text.  They are never executed
where they are linked, so their size must not decide where live code
lands.

### Structural pinned slots

Pinned slots are structural: translation, or an operation the kernel
cannot otherwise express, depends on each.  The vector page, both
page-table roots, the L2 window and two page windows are structural;
the second page window earns its slot by letting `pmap_copy_page` map
source and destination at once, which removes a 4 KB bounce buffer and
halves the bytes moved per copy.

A slot is worth more than the page it holds.  The pinned TLB is a
separate structure, so covering a page there also hands its way back
to the set it used to occupy: pinning one page whose own cost was
13,891 refills dropped its set's total from 45,960 to 14,370.

What the MMU provides beyond the structural slots is unused.  Spending
it on hot pages was tried and did not pay -- see "Ruled out" in
[kernel.md](../../TODO/kernel.md).

### What none of this reaches

A third to a half of kernel refills, depending on the workload, fall
on heap and other runtime KVA, whose addresses come from the allocator
and are not the same twice.  Static kernel data accounts for about a
tenth, which is the whole reach of a pinned data page.

No link-time arrangement reaches the rest; only sampling at run time
would, and that costs instructions on the refill path itself.

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
(write to `SYSDEV_L1_ICACHE` / `CACHE_INVAL_ALL`).  The unified
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

## Soft Interrupts

The port defines `__HAVE_FAST_SOFTINTS`: a soft interrupt runs on
its own LWP's stack, borrowing the context of the LWP it
interrupts, rather than taking the generic path's two context
switches through the scheduler.

### Delivery without a software interrupt line

A port with a hardware software-interrupt request — VAX's SIRR,
MIPS's CP0 soft bits — needs no dispatch check, since lowering the
priority level lets the hardware deliver.  Penumbra has one
interrupt-enable bit, so `softint_trigger` only sets a bit in
`ci_softints`, and the kernel looks for pending work wherever it
returns to IPL_NONE:

- `splx(IPL_NONE)` and `spl0()`, covering triggers from thread
  context and from inside a running handler;
- the end of `trap()`, when the interrupted context had interrupts
  enabled (`PSL_I` in the saved SR) — the path that delivers what
  a hardware interrupt schedules.

The `trap()` check precedes `userret()` so a wakeup or signal
raised by a handler is acted on during the same return; alpha,
aarch64, arm32 and x86 all order it that way.  Nothing re-checks
afterwards, so a soft interrupt triggered during AST processing on
the interrupt path waits for the next delivery point — a gap those
ports accept too.

### The soft level lives on the LWP

`SR.I` is one bit and cannot express IPL_SOFTCLOCK through
IPL_SOFTSERIAL.  Rather than shadow the level in a per-CPU
variable (the usual `ci_cpl`), each softint LWP carries the set of
levels it masks — its own and every lower one — in
`l_md.md_softint_mask`, which is zero for every other LWP.
Delivery skips what `curlwp` masks, so a running handler is
preempted only by a higher level.

Being per-LWP, the level cannot desync from exception entry, and
it follows a handler that blocks and resumes as an ordinary
thread.  `ci_softints` and `ci_softlwps[]` are indexed by priority
rank — a level's IPL above `IPL_SOFTCLOCK` — because the MI
`SOFTINT_*` constants are not in priority order.

`splsoft*()` still disables interrupts, like every level above
IPL_NONE; the LWP mask is the only finer distinction the port
makes.

### Dispatch and the blocking path

`softint_fast_dispatch` (locore.S) saves the interrupted LWP's
callee-saved registers into its PCB exactly as `cpu_switchto`
would, but with the resume address pointing at a cleanup
trampoline.  It then moves SP to the softint LWP's stack, sets
`curlwp` in both `cpu_info` and R12, and calls
`softint_dispatch()`.

If a handler blocks, `mi_switch()` sees `l_switchto` and switches
straight back to the interrupted LWP, which resumes at the
trampoline and finishes the interrupt exit as though the dispatch
had returned; the softint LWP completes later as an ordinary
thread.  The trampoline's one fixup is `ci_mtx_count`, which
`mi_switch()` leaves one below its locks for a tail the
interrupted LWP never runs — the same fixup alpha's
`alpha_softint_return` and VAX's `softint_cleanup` make.

The dispatch is ordinary kernel text, not vector-page code: it is
called from C before `trap()`'s final `DI`, so the vector-page
epilogue rules do not reach it.

## Boot Stack Switch

ARM-style pattern: `penumbra_init()` runs on a 4 KB boot stack
(allocated in BSS by locore.S) and returns the new SP (lwp0's
16 KB USPACE kernel stack).  locore.S switches SP before calling
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
interpret: `cpu_switchto`, `lwp_trampoline`,
`softint_fast_dispatch`, `_trap_common`, `trap_return`,
`setjmp`, `longjmp`, or any PC in the pinned
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

## curlwp in R12

The kernel has no TLS, so `curlwp` is pinned in R12, the ABI thread
pointer (`cpu.h`); `cpu_info_store.ci_curlwp` stays the canonical
copy.  Four boundaries keep R12 == curlwp whenever kernel C runs:

1. `locore.S` sets R12 to `&lwp0` before the first C call at boot.
2. `cpu_switchto` saves and restores R12 through `pcb_context`, like
   SP and LR, and writes `ci_curlwp`.
3. `cpu_lwp_fork` seeds the child's `pcb_context` R12 slot, which the
   `*pcb2 = *pcb1` copy would otherwise leave pointing at the parent.
4. `_trap_common` reloads R12 from `cpu_info_store` after saving the
   trapframe, since a user trap arrives with R12 holding the user's
   TLS pointer.

## Atomics

- **Userland:** RAS (Restartable Atomic Sequences).
  `atomic_cas_32` uses plain load/cmp/store bracketed by RAS
  labels; `.init_array` constructor registers the sequence via
  `rasctl()`.  `userret()` checks `ras_lookup()` on every
  return to user mode.
- **Kernel:** the same restartable sequence.  The kernel links the
  same `atomic_cas.S`, and `trap()` rewinds a supervisor-mode trap
  whose `EPC` lies inside the region before anything else runs
  (`kernel_ras_restart()`).  The interval is the one `ras_lookup()`
  tests, open at both ends: an interrupted store restarts because it
  has not executed, and the end marker sits past the store, where the
  sequence has finished either way.  Restart is sound only while the
  store stays last and no argument register is written inside the
  region, so its size is pinned at assembly time.  The cost is 2
  instructions on a user trap and 7 on a supervisor trap; the TLB
  refill fast path never reaches `trap()`.  Bracketing the CAS in
  `DI`/`EI` instead would put two drain-commits on every adaptive
  `mutex_enter`/`mutex_exit` and refcount on a pipelined core.

  Testing it needs a handler that writes the word the interrupted
  CAS targets (`isa/test_ras_restart`): an interrupt with no such
  handler resumes and restarts indistinguishably, so it is not a
  negative control.

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

## Audio Driver

The audio device ([devices/audio.md](../devices/audio.md)) presents one
pushed-PCM channel, plus optionally more channels that mix in hardware.
`audio(4)` drives channel 0 as a PCM stream. The remaining channels are
exposed as *voices* through a separate character device, `/dev/pmusic`,
for consumers that play sample data at pitches and gains of their own —
a tracker, a sound-effect mixer, a sampler.

### Single-stream binding

`audio(4)` drives channel 0 directly. `trigger_output` supplies a ring
and a block size and asks to be called once per block: the block size
becomes the channel's watermark, and the per-block callback is driven
from its watermark flag.

Because the block *is* the watermark, it is bounded by **half** the
channel's FIFO depth rather than by the whole of it. The watermark says
how much free space raises the interrupt, so what stays queued at that
moment — the depth less the block — is the deadline for servicing it. A
block near the full depth leaves almost nothing, and the driver clamps
in `round_blocksize` to keep the servicing margin and the block the same
size. `__AUDIO_BLK_MS` in `<machine/param.h>` is a request that this
clamp overrides on a device whose queue is shallow relative to it.

The frames a driver copies out of the ring have left it, whether or not
they have reached the converter yet, so the per-block callback owes the
framework one call per block *copied* — including any the driver primes
the queue with before starting the channel. A prime whose count is not
accounted for desynchronises the framework from the read cursor
permanently: it refills blocks the driver has already passed while the
driver reads blocks it has not yet refilled. Priming at most one block
keeps that accounting inside the block the stream starts on, without
calling back into the framework from within its own start path.

**One block per interrupt is also the ceiling, not just the floor.**
The framework produces a single block per callback and holds no deeper
lookahead — the ring's capacity sets how much space exists, never how
much of it is valid. A driver whose queue has room for several blocks
will happily read all of them, and everything past what the framework
has written is stale ring content: audible as corruption, invisible to
the device's own underrun flag, because the queue never actually ran
dry. Sizing the queue's read to the block, rather than to the free
space, is what keeps the driver behind the producer. This is also why
enlarging the ring through `round_buffersize` buys nothing here.

### Multi-voice binding

`audio(4)` models one PCM stream per open and has no representation for
the extra channels, and its `dev_ioctl` hook is not enough to add one.
The hook receives the device rather than the open file, so it cannot
tell which descriptor issued a call; `audio_poll` consults only the
descriptor's PCM tracks; and `hw_if->close` runs when the last PCM
track closes, which a descriptor that only issued ioctls never has. A
voice consumer needs all three per open — an allocation of its own, a
wakeup, and release when it exits.

So `paudio` also provides a character device, `/dev/pmusic`, named
after `sequencer(4)`'s `/dev/music`. It opens exclusively, and the open
is a *session* holding voices, a sample memory, a tick clock and an
event queue; closing it silences its voices and releases all of it. A
second open fails with `EBUSY`. One MOD already wants every channel a
four-channel device has, so concurrent sessions would contend for
voices rather than share them. No MI code changes.

A session carries three kinds of traffic, each on the mechanism made
for it:

| Traffic | Mechanism |
|---------|-----------|
| Sample bytes | `pwrite`/`pread`/`lseek` on the session's sample memory |
| Structured control | `ioctl` |
| Wakeup for more events | `poll`/kqueue (`POLLOUT`, `EVFILT_WRITE`) |

### Sample memory

A session's sample memory is a flat byte space, sized by
`PENAUDIO_SETMEM` and written at offsets with `pwrite(2)`. Events name
what to play by offset and length, as Amiga software named chip RAM
through Paula's location and length registers, so there is no
namespace of sample handles to allocate and free. A MOD file stores its
samples back to back, so one `pwrite` loads all of them and each
sample's offset is its position in that block.

The offset reaches the driver as the transfer's `uio_offset`, as it
does for `/dev/mem`: specfs accepts any non-negative offset and leaves
the bounds to the driver. `lseek` with `SEEK_SET` or `SEEK_CUR` works;
`SEEK_END` measures from the device node's size rather than the
memory's.

- The cap defaults to 1/64 of physical memory rounded up to a power of
  two — 512 KB on a 32 MB machine — and is tunable as
  `machdep.paudio.maxmem`. ProTracker's own ceiling, 31 samples of
  128 KB each, is what the knob is for.
- New memory is zeroed. `SETMEM` fails with `EBUSY` while a voice is
  sounding or events are queued, because queued events were validated
  against the old size.
- A write that runs past the end is short; one that starts past it
  fails with `ENOSPC`.
- Memory may be rewritten while it plays. A tracker effect that edits
  sample data, such as ProTracker's `EFx`, is a `pwrite`. The edit is
  heard once the frames already in the channel's FIFO have played.

`mmap` would spare the one copy made at load time, and `d_mmap` can
map the same offset space without changing anything above it. It is
not free: a device mapping holds no reference on the memory, which must
therefore outlive the descriptor while it is mapped, and the memory
must stay wired and mapped into the kernel, because the refill reads
it from interrupt context.

### Voices

A voice is one hardware channel, addressed by its index within the
session's allocation. It starts silent, at unity rate and gain.

- `SET_PLAY` discards what the channel has queued, resets its phase
  and plays `pe_play`; when that ends it continues with `pe_loop`,
  which repeats until changed. A `pe_loop` of length 0 makes the sound
  one-shot: the voice falls silent at the end of `pe_play`.
- `SET_LOOP` alone replaces what follows the current pass, and takes
  effect when that pass ends — ProTracker's instrument number without
  a note, and Paula's latched location and length.
- `SET_STOP` discards what is queued and silences the voice.
- `SET_INC` and `SET_VOL` apply on the next output frame and leave
  queued data alone, as the device's own `INC` and `VOL` do.

The driver refills each sounding voice's channel from sample memory in
the device's watermark interrupt, following `pe_play` into `pe_loop`
and converting the region's format into the channel's frame words.
Where the device advertises `CAP.MONO` or `CAP.FMT8`, the driver may
pack several frames per word; the interface does not change.

### Tick clock

Events are timed in *ticks*. A session's clock counts them from 0 once
`PENAUDIO_START` sets it running, at the period `pc_period_us` gives
until a `SET_PERIOD` event changes it. Ticks are the unit a tracker
already thinks in, and a tempo change is an event like any other.

The clock runs on `callout(9)`. Due times are kept in microseconds,
and a tick applies on the first clock interrupt at or after its due
time, so ticks land on multiples of `pi_tick_grain_us` — never early,
less than one grain late, and without cumulative drift. A period
shorter than the grain applies more than one tick in some passes.

All of one tick's events apply together in a single pass, in the order
they were submitted. Within one event the fields apply in the order of
the `SET_*` bits, so a note starts at its own pitch and gain.

**The horizon.** `ps_through` declares that every event up to that
tick has been submitted; without it, a tick that has no events is
indistinguishable from a consumer that has fallen behind. The clock
does not apply a tick past the horizon. It waits, counting each clock
interrupt spent waiting in `pst_stalls`, and resumes when the tick is
declared, with the rest of the timeline shifted later by the wait.
Voices keep sounding meanwhile; only control stops. A late consumer
therefore costs a hiccup, rather than several ticks' effects
collapsing into one instant.

**Immediate events.** An event whose tick has already been applied
applies during the `SUBMIT` call itself. A session that never starts
its clock stays at tick 0, so every event it submits applies at once —
all a sound-effect consumer needs.

`PENAUDIO_PAUSE` freezes the clock and the session's channels
together, so resuming continues exactly where it stopped.
`PENAUDIO_RESET` drops the queue, silences the voices and stops the
clock at tick 0, keeping the memory and the allocation; it is how a
consumer seeks.

`poll(2)` reports `POLLOUT`, and kqueue `EVFILT_WRITE`, while the
queue has room and either the clock is stopped or fewer than
`pc_lowat` ticks separate `pst_tick` from `pst_through`.

### Interface

```c
#define PENAUDIO_VOICE_NONE	0xFFFF

/* pe_format */
#define PENAUDIO_FMT_S8		1	/* signed 8-bit mono */
#define PENAUDIO_FMT_S16	2	/* signed 16-bit LE mono */
#define PENAUDIO_FMT_S16_STEREO	3	/* signed 16-bit LE, left then right */

/* The shortest tick period, so one clock pass applies a bounded number
 * of ticks. */
#define PENAUDIO_PERIOD_MIN_US	1000

/* pe_inc: the range every CLASS_AUDIO device supports, 1/256x to 4x. */
#define PENAUDIO_INC_MIN	0x00000100
#define PENAUDIO_INC_MAX	0x00040000

struct penaudio_info {
	uint32_t pi_rate;		/* output frames per second */
	uint16_t pi_nvoices;		/* voices the device has */
	uint16_t pi_navail;		/* voices PENAUDIO_ALLOC can grant now */
	uint32_t pi_maxmem;		/* largest sample memory, bytes */
	uint32_t pi_maxevents;		/* event queue capacity */
	uint32_t pi_tick_grain_us;	/* ticks land on multiples of this */
};

struct penaudio_region {
	uint32_t pr_offset;		/* bytes into sample memory */
	uint32_t pr_length;		/* bytes; 0 = nothing */
};

/* pe_set: which fields apply, in this order within one event */
#define PENAUDIO_SET_PERIOD	0x0001	/* tick period; voice NONE */
#define PENAUDIO_SET_INC	0x0002
#define PENAUDIO_SET_VOL	0x0004
#define PENAUDIO_SET_PLAY	0x0008	/* discard queued, play pe_play, then pe_loop */
#define PENAUDIO_SET_LOOP	0x0010	/* pe_loop follows the current pass */
#define PENAUDIO_SET_STOP	0x0020	/* discard queued, fall silent */

struct penaudio_event {
	uint32_t pe_tick;		/* tick this applies on */
	uint16_t pe_voice;		/* voice within the allocation */
	uint16_t pe_set;		/* PENAUDIO_SET_* */
	uint32_t pe_inc;		/* 16.16 source frames per output frame */
	uint16_t pe_vol_l;		/* 0x0000 silent .. 0xFFFF unity */
	uint16_t pe_vol_r;
	uint32_t pe_format;		/* for pe_play and pe_loop */
	struct penaudio_region pe_play;
	struct penaudio_region pe_loop;	/* repeats until changed; 0 = one-shot */
	uint32_t pe_period_us;		/* SET_PERIOD */
};

struct penaudio_status {
	uint32_t pst_tick;		/* tick now in effect */
	uint32_t pst_through;		/* horizon as last declared */
	uint32_t pst_room;		/* events the queue can take now */
	uint32_t pst_stalls;		/* clock interrupts spent at the horizon */
	uint32_t pst_underruns;		/* channels found empty mid-sound */
	uint32_t pst_sounding;		/* bit n: voice n has data to play */
};

struct penaudio_submit {
	const struct penaudio_event *ps_events;
	uint32_t ps_eventsize;		/* sizeof(struct penaudio_event) */
	uint32_t ps_nevents;		/* in: offered; out: accepted */
	uint32_t ps_through;		/* in: every event up to this tick is submitted */
	struct penaudio_status ps_status;	/* out */
};

struct penaudio_clock {
	uint32_t pc_period_us;		/* until a SET_PERIOD event */
	uint32_t pc_lowat;		/* POLLOUT once fewer ticks than this are queued */
};

#define PENAUDIO_GETINFO	_IOR('Y', 0, struct penaudio_info)
#define PENAUDIO_ALLOC		_IOWR('Y', 1, uint32_t)	/* voices wanted/granted; 0 releases */
#define PENAUDIO_SETMEM		_IOW('Y', 2, uint32_t)	/* sample memory size, bytes */
#define PENAUDIO_SUBMIT		_IOWR('Y', 3, struct penaudio_submit)
#define PENAUDIO_GETSTATUS	_IOR('Y', 4, struct penaudio_status)
#define PENAUDIO_START		_IOW('Y', 5, struct penaudio_clock)
#define PENAUDIO_PAUSE		_IOW('Y', 6, int)	/* 1 freezes clock and voices, 0 resumes */
#define PENAUDIO_RESET		_IO('Y', 7)		/* drop queue, silence voices, clock to 0 */
```

`PENAUDIO_ALLOC` grants at most the number asked for; a consumer that
needs more mixes the remainder itself or gives up. Changing the
allocation fails with `EBUSY` while events are queued or a voice is
sounding. `PENAUDIO_SETMEM` fails with `EINVAL` above `pi_maxmem`, and
`PENAUDIO_START` with `EINVAL` below `PENAUDIO_PERIOD_MIN_US` and with
`EBUSY` on a clock already started.

`PENAUDIO_SUBMIT` takes the longest prefix of the offered events that
fits the queue and is acceptable, and reports its length in
`ps_nevents`. An event is refused when it:

- sets an unknown `SET_*` bit, or combines `SET_STOP` with `SET_PLAY`
  or `SET_LOOP`;
- sets `SET_PERIOD` together with a voice field, with a voice other
  than `PENAUDIO_VOICE_NONE`, or with a period below
  `PENAUDIO_PERIOD_MIN_US`;
- names a voice outside the allocation;
- names a region outside sample memory, or one not aligned to its
  format's frame size;
- sets an increment outside the range every `CLASS_AUDIO` device
  supports, `PENAUDIO_INC_MIN` through `PENAUDIO_INC_MAX`;
- precedes an event already queued, unless its tick has already been
  applied.

If the first event offered is refused, the call fails with `EINVAL`
and changes nothing. Otherwise it succeeds even when it stops short,
because `ioctl(2)` copies the argument back only on success, and an
error could not report the count. A consumer resubmits from where the
count ends: behind a full queue it waits for room, and behind a bad
event the resubmission fails on that event alone, so `EINVAL` always
names the first event of the batch.

When the call stops short, the horizon moves only to the tick before
the first event not taken, since that tick is not yet wholly
submitted.

`ps_eventsize` must equal `sizeof(struct penaudio_event)`:
the command number encodes the size of `struct penaudio_submit` but not
of the records it points to, so without the field a consumer built
against a different record layout would be misread rather than
refused.

`pst_underruns` counts channels found empty while their voice still had
data to play; a one-shot sound running out is not an underrun.

### Why the interface has this shape

**The vocabulary is sample playback, not a file format.** Events carry
regions, an increment and a gain rather than notes, effects or
patterns. An interface shaped like tracker events would place one
format's model in the kernel, where the next format would have to
relitigate it; sample playback serves a tracker, a sound-effect mixer
and a software synthesizer without preferring any of them.

**Voices are allocation-relative.** A consumer granted four voices
addresses them 0-3 whatever hardware channels it received, so the
driver stays free to pack allocations around whatever `audio(4)`
holds, and a consumer needs no policy for a fragmented device.

**Simultaneity comes from ticks.** The hardware starts channels through
a single register write so that onsets are not staggered by how fast
the consumer runs. A tick's events are applied in one pass for the same
reason, and without depending on when the consumer is scheduled.

**The consumer runs ahead; the kernel keeps time.** A tracker's next
tick depends only on the song, so it can be computed before it is due.
Submitting ahead lets a consumer wake a few times a second instead of
on every tick, and keeps its scheduling latency out of the result: the
timing a listener hears is the clock's, not the consumer's.

**Bytes and records travel differently.** Sample data is bulk and
unstructured, which is what `write(2)` is for. Events are structured,
which is what `ioctl(2)` is for — including the refusal when a
structure's size changes.

**Data crosses once.** Sample data enters the kernel at load; after
that, the per-frame cost is the refill — one store to a channel's data
port per frame word, from the watermark interrupt. Four voices at
ProTracker's highest note, period 113, consume about 125,000 words a
second.

**Allocation is arbitrated against `audio(4)`.** Voices claimed by
`PENAUDIO_ALLOC` are unavailable to a subsequent `audio(4)` open and
the reverse, so a stream and a voice can never interleave frames on
one channel. Closing the descriptor releases the allocation, so a
consumer that exits unexpectedly does not strand the device.

**The frameworks that already allocate voices are a different model.**
`midisyn` is expressed in MIDI note numbers and centibel attenuations,
has no path for loading arbitrary sample data, and its
`repitchv`/`relevelv` methods — continuous pitch and level change on a
sounding voice — are unimplemented. `sequencer(4)`, behind
`/dev/music`, sequences MIDI events onto such devices and inherits the
same model. Sample playback and instrument synthesis are different
models rather than two spellings of one.

### Group letter

The ioctl group letter is `'Y'`. NetBSD's registry of group letters is
the tagged list in `ioctl(9)`, and `'Y'` appears neither there nor in any
header in the tree.

The constraint that binds is the one `ioctl(9)` states: only one
number per group, among groups that can reach one descriptor.
Duplicate entries in the registry are normal — `'A'` is listed four
times and `'P'` six, disambiguated by arch or subsystem — because the
namespace is per descriptor rather than global. A `/dev/pmusic`
descriptor is reached only by this group and the generic file ioctls.
The letters a sound interface would otherwise reach for are taken —
`'A'` by `audio(4)`, `'M'` by the OSS mixer, `'H'` by `hdaudio`, and
`'P'` by *"compat/ossaudio and soundcard.h"* — and a letter no sound
interface uses makes a call issued against the wrong node fail rather
than land on a command that happens to share its number.

The registry lags the tree, so absence from it is not on its own
evidence a letter is free: `'T'`, `'K'`, `'L'`, `'D'`, `'N'` and `'X'`
are all in use without an entry, `'T'` in fourteen headers. A candidate
has to be checked against both.

This reservation is recorded here rather than added to `ioctl(9)`.
A port-local driver's entry would never be upstreamed, so it would be a
permanent local diff against base for no benefit. The exposure is
bounded and one-directional: were `'Y'` claimed upstream later, only a
Penumbra kernel would ever have both groups present, and a number
collision within the group would be needed on top of that.

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
