# Kernel and NetBSD

NetBSD port: MD kernel code, drivers, networking.

## Kernel: block-device reads still go single-block

`pmci` handles CMD18/CMD25 multi-block natively (a per-block
FIFO-burst loop inside a single command envelope), but the block
device doesn't reach it:
`dd if=/dev/ld0 of=/dev/null bs=32k` issues 64 separate 512-byte
`bread()` calls per syscall (`DEV_BSIZE = 512`), so each turns into
a single-block read in `sdmmc_mem_read_block_subr`
(`c_datalen / c_blklen == 1` → `MMC_READ_BLOCK_SINGLE`).  CMD18
never engages on the block-device path.  This is why
`dd if=/dev/ld0` shows ~85 KB/s while
`dd if=/dev/rld0 of=/dev/null bs=32k` and FS-mediated reads see the
~190 KB/s CMD18 number.

Options if/when this matters:
1. Teach `ld_sdmmc_dobio` to coalesce adjacent `bread()` requests
   before forwarding to `sdmmc_mem_read_block`.  Touches the buf
   queue path; modest complexity.
2. Bump `DEV_BSIZE` for `ld(4)` so block-device reads come in
   larger chunks by default.  Affects every consumer of the block
   interface; broader blast radius.
3. Convert callers that care to use `/dev/rld0` (raw) instead.
   No code change, but documentation/convention only — easy to
   forget.

Most filesystem-mediated I/O already takes the larger-buffer FFS
path (16 KB FS blocks → 32-sector CMD18/CMD25), so the user-visible
impact is mostly for raw block-device tools (`dd if=/dev/ld0`,
`disklabel`, etc.).  Leave the block-device path single-block until
something on the read path notably matters.

## Kernel: IRQ-driven SPI completion deferred

The SPI FIFO data-phase plan originally included
`intr_establish_xname()` + `cv_wait` on XFER_DONE.  Implemented and
benchmarked: **3× slower**
than polled (85 KB/s → 28 KB/s).  Root cause is the cv_wait → IRQ →
cv_signal round-trip costing ~12 ms — more than 10× the 660 µs SPI
burst it's waiting for.  See also `pbench pipe_pingpong` (~19 ms for
2 context switches + 4 syscalls) and `pbench fork_exit` (~600 ms).

Polled busy-wait remains the right primitive for sub-ms device waits
on the current scheduler.  Revisit once one of:
- Context switch cost drops to ≪ 1 ms (would benefit fork, pipe, signals
  too — scratch SPRs landed in 02de545943e7 and shaved a few percent off
  trap-entry, but the cv_wait round-trip is still ~10 ms; the rest of
  the gap is trap_common / pmap_activate / copyin)
- CMD18 multi-block — landed.  Each `pmci_burst` now covers up to
  N × 660 µs of wire time on the FS-mediated path (16 KB FS blocks →
  32-sector CMD18), so re-measuring IRQ-driven completion against the
  new envelope is worth a fresh attempt

## Kernel: `bus_space_mmap()` maps device memory cacheable

`bus_space_mmap()` (`netbsd/sys/arch/penumbra/penumbra/bus_space.c`)
returns a bare page frame, relying on "the I/O region is uncached by
the address map". Nothing enforces that. The TLB's cacheable output is
the PTE's `C` bit as stored, with no address-based override, and
`PTE_MAKE` sets `PTE_C` unless the caller passes `PMAP_NOCACHE`. The
device pager enters user mappings with `pmap_mmap_flags(cookie)`,
which penumbra leaves at the MI default of 0. So every user mmap of
device memory is cacheable, contrary to `doc/system/bus.md` ("Always
uncached"). The kernel's own `bus_space_map()` passes `PMAP_NOCACHE`
and is unaffected.

The only mmap-able device is the display framebuffer
(`pdisplay_mmap()`), and there the result happens to be coherent: the
CPU is the framebuffer's only writer, and write-through with
update-on-hit keeps every cached copy current. The visible effect is in
the counters — framebuffer stores count as L1-D and L2 write misses
(the plasma profile in `doc/internals/penumbra3/memory-performance.md`).
A device with read side effects, or one that changes its own memory,
would break when mapped this way: a cacheable load miss fills a whole
16-byte line, reading the three neighbouring words too, and later loads
hit stale copies.

Fix: carry the cache attribute in the mmap cookie, as the x86 and ARM
ports do — define `pmap_mmap_flags()` and `pmap_phys_address()` to
pass flag bits in the cookie's high bits, and have `bus_space_mmap()`
set `PMAP_NOCACHE` unless the caller asks for
`BUS_SPACE_MAP_PREFETCHABLE`. Decide the framebuffer's attribute at the
same time, because uncached costs it: every framebuffer store blocks,
and under a gen3 cacheable-only store buffer each one would also drain
the buffer. A framebuffer wants uncached-but-bufferable
(write-combining) semantics, close to what the reserved PTE `S` bit is
earmarked for — see the "PTE `S` (speculatable) bit" item under
"Penumbra/2 SDRAM-backed sim + EX-frontier interrupts" below.

## Kernel: guard page for kernel stack overflow

The kernel u-area (`UPAGES = 4`, 16 KB) has no guard page, so stack
overflows corrupt whatever lives immediately below the u-area before
eventually manifesting as a confusing nested TLB miss inside
`_trap_common` (and a double-fault BREAK).  We already sized USPACE
above the worst -O0+DIAGNOSTIC frames, but a silent corruption
window still exists.

NetBSD supports per-arch opt-in guard pages via the
`__HAVE_CPU_UAREA_ROUTINES` hook.  See
`netbsd/sys/arch/x86/x86/vm_machdep.c:cpu_uarea_alloc()`: it
allocates `USPACE + PAGE_SIZE` from `kernel_map`, then
`pmap_kremove()`s the redzone page and `uvm_pagefree()`s the backing
PA so any touch bus-faults immediately.  `cpu_uarea_free()` is the
inverse.  amd64 uses UPAGES=5 (4 real + 1 redzone); i386 with
redzone enabled uses 3 (2 + 1); KASAN/KMSAN builds use more.

Implementation sketch for Penumbra:
1. Define `__HAVE_CPU_UAREA_ROUTINES` in `include/cpu.h`.
2. Add `cpu_uarea_alloc(bool system)` / `cpu_uarea_free()` to
   `penumbra/machdep.c`, mirroring the x86 pattern.  We need a
   leading guard (fault on underflow from the top of stack growing
   down) — allocate `USPACE + PAGE_SIZE`, strip the *first* page.
3. Optionally add a trailing guard too — allocate `USPACE + 2*PAGE_SIZE`
   and strip both.  amd64 does both.
4. Adjust `penumbra_lwp0_init()` in `startup.c` to use the same
   layout for lwp0's uarea so the boot stack is guarded from the
   start, not only from first `fork()`.

After this lands, kernel stack overflow produces a clean bus fault
with EPC pointing at the offending instruction, not a nested TLB
miss in the trap handler.

## Kernel: fork() is unreasonably slow

`pbench kernel fork_exit` reports **~670 ms per fork+exit+wait round
trip** on the ULX3S FPGA at 25 MHz (see
`benchmark/netbsd-bench/BASELINE.md`).  That's two orders of magnitude
beyond what the workload should cost, and it makes the system painful
to use: `/etc/rc` runs many short commands sequentially, single-user
boot stalls visibly, and any shell pipeline is sluggish.  For
comparison, `pipe_pingpong` (which exercises 2 context switches + 4
syscalls per round trip) reports ~19 ms — so context-switch cost
alone is ~30× cheaper than a fork.  The bulk of the 670 ms is
fork-specific work, not generic scheduler/syscall overhead.

### Where the time goes — full attribution

Nine MD-side phases are instrumented via the `machdep.fork_timing`
sysctl (`struct fork_timing_snapshot` in `<machine/fork_timing.h>`).
Two waves of measurement, both on the ULX3S FPGA @ 25 MHz, median
over 8 trials × 10 forks per trial:

| Phase                  | per-fork    | calls/fork | % of total |
|------------------------|-------------|-----------:|-----------:|
| `cpu_lwp_fork`         | 0.11 ms     | 1          | 0.02 %     |
| `pmap_create`          | 5.74 ms     | 1          | 0.86 %     |
| `pmap_remove_all`      | 117.4 ms    | 1          | 17.5 %     |
| `pmap_destroy`         | 4.14 ms     | 1          | 0.62 %     |
| `pmap_enter` (agg)     | 200.3 ms    | 181        | 29.9 %     |
| `pmap_protect` (agg)   | 0.86 ms     | 4          | 0.13 %     |
| `pmap_copy_page` (agg) | 28.0 ms     | 10         | 4.19 %     |
| `pmap_kenter_pa` (agg) | 0.07 ms     | 1          | 0.01 %     |
| `uvm_fault` (agg)      | 586.8 ms    | 51         | 87.6 %     |

`uvm_fault` is the *inclusive* outer bracket (the call site in
`trap.c`).  `pmap_enter` and `pmap_copy_page` are nested inside it.
Subtracting the nested leaves:

  **`uvm_fault` exclusive (MI internals) = 587 − 200 − 28 = 359 ms.**

That 359 ms is the page-fault machinery `uvm_anon_alloc`,
`uvm_pagealloc`, `amap_add`, trap-entry/exit overhead — all MI code,
unreachable from MD without forking the upstream NetBSD tree.

### What lives where

**MD-attributable share — ~54 % of total fork+wait time:**

- `pmap_enter` × 181 calls × ~28 k cyc each (29.9 %)
- `pmap_remove_all` 117 ms (17.5 %)
- `pmap_copy_page` 28 ms (4.2 %)
- `pmap_create` + `pmap_destroy` + others (~1.6 %)

**MI-bound share — ~46 % of total:**

- `uvm_fault` exclusive (359 ms): page-fault dispatch internals
- Scheduler enqueue, struct proc/lwp setup, filedesc dup, signal
  machinery, wait reaping — not separately instrumented, but
  bounded above by `total − sum(MD)` ≈ 670 − 360 = 310 ms.

### What I got wrong on the first wave

- **`pmap_protect` was hypothesised to dominate** as the "COW
  marking pass."  Measured at 0.86 ms / 4 calls / fork — NetBSD
  uses *lazy* COW, the protect-on-fork pass doesn't exist.  Dead
  end; don't optimise `pmap_protect`.
- **`pmap_kenter_pa` was hypothesised to soak uarea-allocation
  cost.**  Measured at 66 µs / 1 call / fork.  uarea pool caching
  is already doing its job; one kenter per fork is the L1 KVA
  wire from `pmap_create`, not the uarea pages (those are
  inherited from the pool warm).

### Why `pmap_enter` is so expensive per call

181 calls × ~155 µs each = 28 ms × 51 faults * 3.5 entries/fault
≈ the observed 200 ms.  Per-call 28 k cycles = 1.1 ms is a lot for
a single PTE install.  Plausible contributors, in order of suspicion:

1. **`pool_get` for the PV entry.**  Mutex acquisition + freelist
   walk + occasional pool growth path.  Could be hundreds of cycles
   even on the warm path.
2. **`pmap_pte_lookup` scratch-window reload.**  Every call does a
   WRSYS to remap pinned TLB slot 3 to the L2 page, even when the
   previous lookup was for the same L2.  At 181 calls/fork with
   most adjacent VAs sharing an L2, an L2-cache here could fold
   the WRSYS away on most calls.
3. **`pmap_tlb_invalidate`** — one WRSYS per call.
4. **`icache_invalidate`** — full-cache invalidate if the mapping is
   executable.  Many of the child's mapped pages are executable
   (libc text), so this could fire on most calls.

A second-stage instrumentation inside `pmap_enter` (bracket the four
sub-phases above) is the cleanest way to attribute the 1.1 ms.

### Why `pmap_remove_all` is suspiciously large on its own

~118 ms ≈ 2.95 M cycles for an init-sized address space (~2000 valid
pages, sparse over ~10 valid L1 slots).  That works out to ~1500
cycles per visited *valid* PTE — far more than the algorithm
describes (load+branch on invalid entries; PV-list walk + stat
decrement on valid ones).  Likely culprits: the per-L2 cached SDRAM
walk over 1024 entries each (256 cache lines × first-touch SDRAM
latency), and `pv_remove`'s SLIST traversal hitting cold lines.
Scratch-window cache (item 2 in "Quickest wins" below) would help
directly since adjacent L2 walks share PA.  Worth a closer look in
its own right after the bigger gap is closed.

### Quickest wins, ranked by leverage

Now that attribution is complete, candidates ordered by leverage on
MD-attributable time:

1. **Attribute `pmap_enter`'s 1.1 ms/call.**  Bracket the four
   sub-phases (PV pool_get, scratch-window reload, TLB invalidate,
   icache invalidate) inside `pmap_enter` and re-run the bench.
   Without that breakdown we don't know which fix lands the biggest
   share of the 200 ms (~30 % of total).  Cheap follow-up; should be
   the next move.
2. **Scratch-window caching across `pmap_l2_map`.**  A one-entry
   cache that skips `pmap_scratch_map` when the target L2's PA
   matches the previous lookup.  Applies to `pmap_enter`,
   `pmap_remove_all`, and `pmap_extract`/`pmap_clear_modify`.
   Likely worth tens of ms.
3. **`pmap_remove_all` early-out per L2.**  Stop walking once
   `pm_stats_resident` reaches zero; most empty L2 slots beyond
   that point don't need scanning.  Probably a few-ms win.
4. **Pre-populating the child's pmap with parent's R-O pages.**
   Would reduce the 51 faults / fork to a handful.  Bigger refactor
   (shared L2 or eager L1-half copy of R-O entries) and partially
   requires touching MI assumptions about `pmap_remove_all`.
5. **`icache_invalidate` granularity.**  We do a full-cache flush
   per `pmap_enter` on executable pages — if 51 page-faults all do
   this, it's a sizeable hidden cost.  A line-granularity invalidate
   would help, but probably needs RTL changes.

Previously listed as suspects but ruled out by measurement (do not
chase further without new data):

- `pmap_copy` over parent's full page table — already a no-op.
- `pmap_create` zero/copy cost — measured at 5.74 ms, fine.
- `cpu_lwp_fork` trapframe setup — 0.11 ms, negligible.
- `pmap_protect` "COW-marking pass" — 0.86 ms / 4 calls; NetBSD's
  COW is lazy, this pass doesn't exist.
- `pmap_kenter_pa` uarea cost — 66 µs / 1 call; uarea pool warms.

### What's not fixable in MD code (~46 % of total fork+wait time)

The `uvm_fault` exclusive bracket (~359 ms / fork) is MI internals:
`uvm_anon_alloc`, `uvm_pagealloc`, `amap_add`, trap-entry/exit
overhead, scheduler enqueue, struct proc/lwp setup, filedesc dup,
signal machinery, wait reaping.  Without forking the upstream
NetBSD tree, that's the floor we can't drop below.  Even an
arbitrarily-good MD layer can only halve the 670 ms or so.

## RESOLVED: "uncached MMIO STW 2.4× slower than LDW" was codegen, not hardware

The hardware write path is symmetric — RTL-sim microbenches put a
single uncached STW and LDW at the same ~4 cyc/op.  The observed 2.4×
came from `-fno-strict-aliasing` (kernel-wide, `Makefile.kern.inc`):
with TBAA off the compiler can't prove a volatile MMIO store doesn't
alias the in-memory `bus_space_handle_t sc_ioh`, so it reloaded the
handle before every write — 3 instructions per "STW" vs 1 per LDW,
3:1 ≈ the observed 2.4×.  Do not re-investigate as a hardware
write-path asymmetry.

`pmci_burst` now uses the canonical `bus_space_{write,read,set}_multi_1`
primitives (handle passed by value → base pinned in a register, immune
to the reload; committed as `9ddfcf4aa941`).  Perf-neutral by design —
per-byte cost is dominated by the MMIO store latency, and the idiomatic
m68k flat-MMIO ports reach the same rolled-loop conclusion — the value
is idiom and immunity to the reload.

## Kernel: source-level costs on the trap, syscall and driver paths

Found in the hot-path code review (2026-09-13); none of these is a
code-generation problem.

- **`copyin`/`copyout` reload curlwp.**  `GET_PCB` in `copy.S` loads
  `cpu_info_store.ci_curlwp` (`la` plus `ldw`, 3 instructions),
  although R12 holds curlwp whenever kernel code runs;
  `ldw reg, [r12 + L_PCB]` is one.  Both routines also save `r6` only
  to hold `VM_MAXUSER_ADDR` for one compare, where the unused
  caller-saved `r4` would do.
- **`trap()` builds a 168-byte, 7-register frame on every trap.**  The
  signal-delivery and fault-handling cases share the frame with the
  common return path.  Moving them into `noinline` helpers leaves the
  hot path a small frame.
- **Trap entry saves registers twice.**  The vector stubs and
  `_trap_common` park R1–R4 in PC-relative scratch memory, then copy
  them into the trapframe — roughly 10 extra memory operations per
  trap.  Whether the general path can save straight into the trapframe
  or use the scratch SPRs, as the TLB-miss handler does, is the
  question to answer first.
- **No mutex stubs.**  `mutex.h` defines `__HAVE_SIMPLE_MUTEXES` but
  not `__HAVE_MUTEX_STUBS`, so every `mutex_enter`/`mutex_exit` is a
  call into MI `kern_mutex.c`.  A stub needs an atomic compare-and-swap
  of its own, so measure the call's share before designing one.
- **com(4) register access is an indirect call.**  MI `com.c` routes
  every register read and write through the `cr_read`/`cr_write`
  pointers `com_init_regs` installs.  That is the MI design; avoiding it
  means a different UART driver, not an MD change.
- **`pdisplay` scrolling reloads bases per cell.**  The row and column
  copy and erase paths reload their base pointers for every character
  cell: 10 instructions per cell where 6 do.  Hoisting them into
  locals, as `b82fb4b0df29` did for Doom's column drawer, fixes it.

## Kernel: pack the hottest syscall-path text onto shared pages

Low priority — syscall performance is already good; this is a future
refinement, not a fix.

Kernel text is mapped through the main TLB: 32 sets of 2 ways, set =
the low 5 bits of the VPN, victim way chosen from a `CPU_CYCLES` bit.
A miss runs the software handler (about 59 instructions to `eret`)
and counts as benchmark instructions.  The trap entry already lives on
the pinned vector page, but everything after it is wherever the link
order put it.  `clock_gettime(CLOCK_MONOTONIC)` alone touches seven
text pages spread across the image — `trap` and `timer_get_timecount`
near `0x80014000`, `syscall` and `copyout` in
`0x80015000`–`0x80017000`, `nanouptime`/`binuptime` and
`sys___clock_gettime50` in `0x801a6000`–`0x801ac000`,
`clock_gettime1` at `0x801e3000`, `memcpy` at `0x802d0000` — plus
data pages (`cpu_info_store`, `sysent`,
`emul_netbsd`, `timehands`).  Any set holding three per-call pages
misses on every call, and which sets fill up moves with unrelated
layout changes.  The IRTranslator probability change showed this:
`clock_gettime` retired more instructions per call on hardware while
every function on its path came out the same length or shorter,
consistent with a TLB set filling up differently.

The idea: place the common syscall path (`trap`, `syscall`, `userret`,
`copyin`/`copyout`, `memcpy`) and the hottest handlers
(`clock_gettime`, `getpid`, `read`/`write`, the timecounter read)
contiguously, so the per-call path fits in one or two pages; likewise
their hot data.  The kernel builds without `-ffunction-sections` and
links with `kern.ldscript`, so this needs per-function sections plus
an ordering — a symbol ordering file for lld, or a `.text.hot` input
section the script places first.

Measuring it needs a TLB miss count, which nothing exposes today (see
"Kernel: count TLB refills"), and a before/after on the pbench
syscall rows.

## Kernel: count TLB refills

Nothing counts refills.  `_real_miss_handler` resolves them without
entering `trap()`, so neither `vmstat -s` nor an evcnt sees them, and
the CPU perfctrs count the handler's ~59 instructions as instructions
of whatever was running.  The TLB's shape, and why its misses move
with layout, are in "Kernel: pack the hottest syscall-path text onto
shared pages".

Doom makes the case.  Moving its lumps out of the zone removed ~1M
instructions a frame, a fifth of the total, with the game's own work
unchanged, and an unrelated change had already moved ~0.3M ("Doom:
where a frame goes").  Refills are the one per-access kernel cost
that fits.  Reference-bit emulation is ruled out:
`pmap_clear_reference` only clears the software flag.

A counter word in pinned scratch, bumped on the fast path and exported
through sysctl beside the perfctrs, settles it.  Splitting it by the
faulting VA's region (text, heap, mapped file, stack) would say which
data to move.  Simulation has now counted them (below), but nothing
counts them on hardware, where the cost in *cycles* actually lives.

### Counted in simulation (2026-09-22)

A scratch ISS patch counted Doom's timedemo ("Doom: where a frame
goes"): 9,425 refills a frame, 544k instructions in
`_real_miss_handler` — 15.5% of every instruction executed, second
only to `R_DrawColumn`.  Of the 8,624 user misses a frame, the mapped
WAD and the shared libraries take 40%, Doom's text 26%, its data and
bss 24%, the stack 5%.

They are not spread evenly: 4 of the 32 sets take 51%.  Set 31 alone
takes 17.4%, because `USRSTACK` is `0x04000000`, so every process's
stack top is VPN `0x3fff` and lands there permanently, beside hot text
pages `0x7f000`, `0x5f000` and `0x3f000`.  Set 11 takes 14.7% (text
page `0xab000` against two mapped-WAD pages).  The quietest set sees
2,250.

### What the TLB's shape costs, and which parts are reachable

The same access stream replayed through shadow TLB models, pinned
pages excluded and kernel mappings treated as global.  The machine as
built takes 1,404,402 misses over the 149 frames:

| model, 4 KB pages | misses | vs. as built |
|---|---:|---:|
| 32×2, `CPU_CYCLES`-bit victim (as built) | 1,404,402 | — |
| 32×2, LRU victim | 1,096,205 | −22% |
| 32×2, LRU, hashed set index | 942,840 | −33% |
| 16×4, LRU — the same 64 entries | 533,694 | −62% |
| 64-entry fully associative, LRU | 356,261 | −75% |
| 64×2, LRU (128 entries) | 263,977 | −81% |
| 32×4, LRU (128 entries) | 178,766 | −87% |

- **Address-space layout is worth at most a third.**  The hashed-index
  row stands in for an ideally spread user address space: it scatters
  the conflicting VPNs without adding capacity.  Moving `USRSTACK` off
  the top set, or spreading the mmap base, costs nothing and stays
  inside the ABI — but it buys far less than the set skew suggests,
  because the working set does not fit 64 entries × 4 KB either way.
  This is the only lever here that is certainly available, and
  relinking Doom's own objects at another base moved its refills 26%
  with the instructions unchanged, so the layout a program happens to
  get is worth about as much as the address space it is given.
- **Associativity is where the misses are, and it is a timing
  question, not a plan.**  Four ways at the same 64 entries more than
  halves them.  The TLB sits on the address path of every access, so
  the honest next step is to find out whether there is any room at
  all: build the main TLB 16×4 and read fmax from the full board top,
  not the bare-core probe.  If it costs clock, it is not a lever —
  expect that answer.
- **Changing the software-managed interface is out.**  A read-only
  "suggested victim way", or a write mode that lets hardware place the
  entry by VPN, would hide the geometry from software and buy the 22%
  the pseudo-random victim gives away.  Both make victim selection and
  placement hardware's job, which is a different TLB design, not a
  patch to this one.  If the TLB is ever redesigned, that is the
  conversation to have then.
- **Page size is unmeasured.**  The 16 KB-page model in the same run
  is unreliable: it invalidates a whole 16 KB entry whenever the
  kernel invalidates any 4 KB page inside it, so what superpages would
  buy is still unknown.

## Kernel/compiler: dedicate R12 (TP) to curlwp — DONE

The ABI reserves R12 as the thread pointer, and the kernel uses no TLS,
so R12 sits unused in kernel code.  Pinning `curlwp` there — the way the
NetBSD RISC-V port pins it in `tp` via
`register struct lwp *riscv_curlwp __asm("tp")` — turns every `curlwp`
read from a load of `cpu_info_store.ci_curlwp` into a register read.  It
costs no register pressure: R12 is already reserved, so the allocatable
set does not change.

### Compiler support — DONE

Two pieces, both landed:

- `register T x __asm("r12")` works end to end (`ff46f87450d8`):
  `getRegisterByName` resolves the canonical name and the ABI aliases,
  the GlobalISel legalizer lowers `G_READ_REGISTER` /
  `G_WRITE_REGISTER` to a COPY to/from the physical register, and
  naming an allocatable or unknown register is a fatal error.  clang
  already lists `r12` in `GCCRegNames`.
- The across-call spill problem is fixed (`32937b8bf9d7`): R12 is now a
  *member* of `GPR_Allocatable` (listed last, still excluded from
  allocation by `getReservedRegs` — the RISC-V `tp` idiom), so the
  register coalescer substitutes R12 into cross-call `%v = COPY $r12`
  values and each use gets a fresh `mov rX, r12` instead of a
  callee-saved copy or stack spill.

### Kernel wiring — DONE (`b625d49e43af`)

curlwp is read from R12, scoped to `_KERNEL` in `cpu.h` (`_KMEMUSER`
keeps the memory-load macro so libkvm consumers are unaffected).  Four
boundaries keep R12 == curlwp whenever kernel C runs:

1. `locore.S` sets R12 = `&lwp0` before the first C call at boot.
2. `cpu_switchto` already saved/restored R12 via `pcb_context` (like
   SP/LR), so it threads across voluntary switches unchanged; it also
   already writes `ci_curlwp`.  No change was needed there — the
   originally-planned "additionally set R12 = newlwp" is redundant.
3. `cpu_lwp_fork` seeds the child's `pcb_context` R12 slot — the
   `*pcb2 = *pcb1` copy would otherwise leave the parent lwp there.
4. `_trap_common` reloads R12 from `cpu_info_store` after the trapframe
   save, before running C.  A userland trap arrives with R12 holding the
   user TLS pointer, which the trapframe save/restore preserves; the
   reload is unconditional (correct from both entry modes, cheaper than
   branching).

`cpu_info_store.ci_curlwp` stays the canonical copy — the trap-entry
reload and any cross-lwp reads still consult it.

### Measured benefit

A `curlwp->field` access drops from `lli`+`lui`+`ldw`+`ldw` (4) to a
single `ldw [r12+off]` — the load takes R12 as its base directly, so it
is 4→1, better than the 4→2 originally estimated — and survives calls
for free (R12 reserved).  Same-session pbench A/B on the ULX3S (min;
identical clang and rootfs, only the kernel source differing, libc rows
flat to confirm isolation): `getpid` 122.49→118.53 µs (−3.2%),
`clock_gettime` 227.33→197.90 µs (−12.9%, with much lower variance).
fork/`pipe_pingpong` are within noise — curlwp is a negligible fraction
of their millisecond-scale cost, and their swings track 1 KB
direct-mapped I-cache layout (the kernel is 8 KB smaller), not this
change.

## Kernel: networking — first NIC through the USB host controller

Decision: the first network adapter is a **USB NIC on the CLASS_USBHC
port**, not a dedicated NIC device. Rationale: the HCD is already owed
for the local-console keyboard, and once it exists the whole MI USB
stack plus every in-tree USB Ethernet driver (`axe`, `aue`, `cdce`, …)
comes along — no new RTL, no device firmware, no from-scratch NIC
driver. The alternatives stay viable later: the ESP32 SLIP bridge
([`system/devices/esp32-nic.md`](../system/devices/esp32-nic.md), needs
RTL + ESP-IDF firmware, adds WiFi) and a Wiznet W5500 on the existing
SPI class (driver-only, needs external hardware). Both have a lower
throughput ceiling than software-driven full-speed bulk, so USB-first
costs nothing on performance either.

Facts the plan leans on:

- Bulk transfers need no controller support beyond the minimum
  protocol: interrupt-vs-bulk is host scheduling policy, and the
  transaction primitives (token / data / handshake, 64-byte buffer)
  are identical. Done: `usb-host.md`'s scope now says so — the three
  handshaken transfer types are the minimum; isochronous / hubs /
  DMA stay behind `CFG_ID`.
- NetBSD's `slhci` (`dev/ic/sl811hs.c`) proves the whole MI stack —
  bulk endpoints included — over exactly this controller shape:
  FS/LS, one software-driven transaction at a time. It is the
  structural template.
- The cost profile is per-transaction software round trips; bulk-IN
  polling pays for NAKs. Usable, not fast — order 100–300 KB/s.
- Device caveat: HS-only adapters may enumerate at FS but offer no
  usable function; safe first targets are USB 1.1-era chips (`aue`,
  `axe`, `url`, `kue`) or a CDC-ECM gadget (`cdce`), which is also
  the cleanest personality to model in simulation.

Work items, in order:

- **ISS/device-model: complete enumeration.** Done: `UsbDeviceSim`
  serves configuration and string descriptors, latches
  `SET_CONFIGURATION`, and terminates exact-multiple control reads
  with the required ZLP; pinned by the grown `test_usbhc_enum` on
  both simulators (`bd53ce1`).
- **Device personalities, in order:** first **mass storage** —
  Bulk-Only Transport plus the small SCSI subset (`INQUIRY`,
  `READ CAPACITY`, `READ(10)`, `WRITE(10)`, `TEST UNIT READY`,
  `REQUEST SENSE`) backed by a host file image, the `+sdcard=`
  pattern. It exercises bulk in both directions with hard data
  integrity (mount a filesystem through it) and needs no host-side
  network plumbing. Done: the wire layer grew a device-function
  seam — `UsbFunctionSim` carries the config descriptor, class
  control requests, packet-level bulk-out, and pull-model bulk-in,
  while `UsbDeviceSim` keeps toggles, chunking, ZLP termination,
  and halt state — and `UsbMassStorageSim`
  (`hw/sim/usb_msc_sim.h`) implements the transport + SCSI subset
  over it, attached in the ISS via `+usbdev=disk:<img>`/`USBDEV=` — the
  generic device-spec argument personalities share — and
  hot-pluggable at runtime with `Ctrl-A U` (kernel detach and
  re-enumeration verified). The
  CDC-ECM NIC personality (bridged to a host TAP device) follows —
  raw packet access is the only part of the network path that is
  host-environment-dependent.
- **NetBSD HCD** — the `CLASS_USBHC` driver from the kernel section
  above, iterated on the ISS. Done: the bus-methods layer —
  `usbd_bus_methods`, software root hub under `usbroothub`, port
  power/reset recipes, the connect-change latch, and the root-hub
  interrupt pipe; `uhub0` explores the port, resets, and walks up to
  the device transfer pipes (`37cc42c`). Done: the transfer pipes —
  a one-transaction-at-a-time engine over TOKEN/XFER_CTRL/DATA with
  a per-pipe state machine (control SETUP→DATA→STATUS, chunked
  data stages, software toggles with RXTOGGLE retransmission
  discard, forced-ZLP OUT endings, three-strikes error retry) and a
  ready-queue scheduler: interrupt endpoints launch at `bInterval`
  on gated SOF interrupts — every launch, not only NAK retries —
  control/bulk NAKs retry round-robin
  (degenerating to immediate relaunch when alone); completions
  post from the hard interrupt to the soft interrupt under the MI
  `usbd_xfer_trycomplete` discipline, and aborts detach at any
  engine state, orphaning an in-flight transaction (safe: all data
  stages through the controller's DATA buffer). `uhub0` now
  enumerates the sim device end to end — descriptors, SET_ADDRESS,
  strings — to a clean "not configured" report on the ISS.
  Done: the kernel config attaches `umass`+`scsibus`+`sd`
  (`files.scsipi` include, `sd` device majors); an FFS filesystem
  on the simulated USB disk mounts and round-trips file data on
  the ISS — bulk verified in both directions with hard integrity.
  Networking landed through `ure(4)` on a real RTL8152 rather than
  the `cdce` simulation path: `ifconfig` plus the INET stack carry
  sustained traffic without loss.  HID landed as `uhidev`/`uhid`.
- **Hardware: US2 wiring** — Done (tracked in the console section
  above): VBUS sourcing confirmed on the board, and commercial
  devices — mass storage, HID keyboards, a Realtek RTL8152 NIC,
  and a cascaded hub — all work on the physical port.

## Kernel: bus_space stream methods when a consumer driver arrives

The MD bus_space layer implements exactly the operations the current
driver set uses; unimplemented ops fail at link (orphan
`bus_proto.h` prototypes), never silently. One conditional MI code
path exists beyond that contract: drivers in the `wdc` family
(`ata_wdc.c`, `atapi_wdc.c`, `wdc_pcmcia.c`, `siisata`) and several
NICs test `__BUS_SPACE_HAS_STREAM_METHODS` and fall back to the
non-stream `multi_N` ops when it is absent. On a little-endian-only
machine the fallback is semantically identical, so today's absence
is benign — but if such a driver ever enters the config, define the
`*_stream_N` variants as trivial aliases of the non-stream ops plus
`__BUS_SPACE_HAS_STREAM_METHODS 1` in `bus_funcs.h`, so the driver
takes its intended path explicitly rather than by fallback. Same
trigger discipline as the display console's `region_2`/`copy_region_2`/
`set_region_2` additions: implement when the consumer exists to
test against, not speculatively.

## NetBSD: ure(4) RX latency quantized at ~100 ms on RTL8152B

A lone received packet (LAN ping reply) is delivered ~100 ms after
it reaches the adapter; sustained traffic is unaffected.  The
suspected mechanism is the chip's RX aggregation flush policy: a
bulk-IN transfer only completes on a short packet, and the device
holds a partial aggregation buffer until an internal flush
threshold.  The driver programs `URE_USB_RX_BUF_TH` with
`URE_RX_THR_HIGH` and never touches the flush timing.  An
experiment writing `URE_RX_THR_SUPER` was inconclusive because the
transport was unreliable at the time.  It no longer is: the
receive-framing, packet-fusion, response-timeout, and pull-down
defects that produced dead sessions and sporadic transaction errors
are fixed, and the link now runs thousands of packets without loss,
so the latency quantum is a clean target.  Progress needs the
RTL8152B register documentation for the threshold field.  Any
resulting driver fix belongs upstream in the MI driver, not carried
as a local patch.

## NetBSD: outbound ping sees ~3x the latency of inbound replies

Pinging the board from another host gets replies in ~25 ms; pinging
out from the board reports ~80 ms round trips on the same link.  The
inbound reply is generated entirely in the kernel (softint echoes
the ICMP request), while the outbound measurement runs through
ping(8): a userland send, a process sleep, and a wakeup plus
copyout on the reply.  The delta therefore bounds the
kernel-to-userland round trip, and it is in the same league as the
measured cv_wait round-trip cost, pointing at scheduling/wakeup
latency rather than the network path.  Worth separating the two
suspects — wakeup latency versus receive-path copy cost — with the
syscall microbenchmarks before touching anything; a cheap
discriminator is timing a local UDP echo against localhost, which
keeps the USB adapter out of the loop entirely.

The USB debug and correctness campaign (line capture, input
synchronizers, arbiter drain and fairness, SOF-delivery counter)
consumed the gen1 top's historical timing slack: seed sweeps that
used to reach 28–30 MHz now plateau at ~25–27, with the CPU's
single-cycle cone still the critical path — the USB logic pressures
its placement rather than appearing in it.  Per the
floorplan-after-features policy, give the USB subsystem the
deliberate treatment: keep_hierarchy over the usbhc/PHY cluster
(the capture module already has it) and, if needed, a REGION
constraint pinning the USB complex away from the CPU cone, then
re-sweep for a pin at or above the 27 MHz floor.

## Kernel: soft interrupts take the scheduler path

The port does not define `__HAVE_FAST_SOFTINTS`, so every soft
interrupt goes through the generic implementation in
`sys/kern/kern_softint.c`.  There `softint_trigger` only flags a
reschedule; the handler runs in its own thread when the scheduler
next switches, and each dispatch costs two full switches
(`mi_switch` plus `cpu_switchto`), into the softint thread and back
out.  When that happens depends on what the CPU was doing:

- **Idle:** promptly, after those two switches.
- **Running a user process:** at its next return to user mode,
  where the pending reschedule preempts it onto the run queue.
- **Running kernel code:** not until that code sleeps, reaches a
  preemption point, or returns to user mode.  There is no kernel
  preemption, and `kern_softint.c` refuses `__HAVE_PREEMPTION`
  without fast softints.

With fast softints the handler runs as the interrupt exits, on a
context borrowed from the interrupted LWP: one stack switch and a
callee-saved register save, with a full switch only if the handler
blocks.  That removes two switches per soft interrupt, and bounds
the dispatch delay by how long interrupts stay disabled rather than
by how long a syscall runs.

The cost lands system-wide rather than on one subsystem:

- every `callout(9)`, which is every userland timer, sleep timeout
  and TCP timer;
- every USB transfer completion — `pusbhc` posts each one to a
  softint, so `ure(4)` networking and `umass` storage pay it;
- network input.  The inbound half of
  [the ping asymmetry](#netbsd-outbound-ping-sees-3x-the-latency-of-inbound-replies)
  runs on this path.

None of it is measured.  On the board, `vmstat -e | grep softint`
gives dispatches per level and `vmstat 1` the switch rate; two
switches per softint at the measured switch cost is the saving.  A
consumer that needs periodic timing, such as a tracker tick driven
from `callout(9)`, also wants the delay from the clock interrupt to
the callout, measured under load.

### Why the port does not have it

The fast path is opt-in: the generic one needs no machine-dependent
code, bring-up used it, and nothing has needed more.  Two properties
of the port make the fast path more than a copy of the reference
port's:

- **No software interrupt line.**  MIPS raises pending softints
  through CP0's software interrupt bits, which the hardware delivers
  once the interrupt mask allows them, so its dispatch needs no
  software check.  Penumbra has one interrupt-enable bit, so pending
  softints have to be checked wherever interrupts come back on: at
  the end of `trap()` when the interrupted context had them enabled,
  and in `splx(IPL_NONE)` and `spl0()`.  RISC-V dispatches this way
  (`cpu_dosoftints()`, `riscv/riscv/softint_machdep.c`) and is the
  closer template.
- **Binary IPL.**  Every level above `IPL_NONE` is `DI`, and the
  current level is read from `SR.I` rather than kept in a variable,
  because exception entry would desynchronize a variable from the
  hardware.  The dispatcher has to know which soft level is running,
  so that the next interrupt exit neither re-enters a softclock
  handler nor preempts it with a lower level.  That takes a
  variable, but only for the soft level: the dispatcher alone writes
  it and the hardware never changes it, so the desync cannot arise.

### What it takes

All machine-dependent, with no MI changes:

- `__HAVE_FAST_SOFTINTS` in `types.h`; per-CPU softint LWPs, a
  pending mask and the current soft level in `cpu_info`.
- `softint_init_md` and `softint_trigger`, a few lines each.
- A dispatch loop in C, shaped like RISC-V's `softint_deliver`.
- An assembly switch (MIPS's `softint_fast_dispatch` is about 110
  lines): save the pinned LWP's callee-saved registers into its PCB
  with the return address at a cleanup trampoline, move `SP` to the
  softint LWP's stack, set `curlwp` in both `cpu_info` and `R12`,
  call `softint_dispatch(pinned, s)`, and switch back.
- The trampoline: if a handler blocks, `cpu_switchto` later resumes
  the pinned LWP there, and it finishes the interrupt exit as if the
  dispatch had returned.
- A pending check in every `splx` that re-enables interrupts.

At run time that check is the whole cost; the real cost is risk.  The
dispatch sits on the trap-exit path, under the vector-page epilogue
rules, and mistakes there show up as hangs rather than panics.

## Kernel: interlocked atomics are not atomic against interrupts (RESOLVED)

`common/lib/libc/arch/penumbra/atomic/atomic_cas.S` is a bare
load-compare-store, and every interlocked name resolves to it —
`atomic_cas_32`, `_uint`, `_ulong` and `_ptr` alongside their `_ni`
forms.  atomic_ops(9) promises the interlocked forms are atomic against
a handler on the same CPU, and everything built on the CAS
(`atomic_inc_32_cas.c` and its siblings) inherits what it gets.
Userland restarted the sequence through `rasctl(2)` and `userret()`;
the kernel links the same code and had nothing, so an interrupt between
the `ldw` and the `stw` let the store clobber whatever the handler
wrote.  Fast soft interrupts made that matter more, since completion
work now runs at interrupt exit and can itself be interrupted.

`trap()` now rewinds a supervisor-mode trap whose `EPC` lies inside the
region, before anything else runs (`kernel_ras_restart()`).  The
interval is the one `ras_lookup()` tests, open at both ends: an
interrupted store restarts because it has not executed, and the end
marker sits *past* the store, where the sequence has finished either
way — the store retired, or the compare branched there.  The region's
size is pinned at assembly time, since restart is sound only while the
store stays last and no argument register is written inside it.

Bracketing the CAS in `DI`/`EI` was the alternative, declined on cost:
both are drain-commit on gen2, which would put two pipeline drains on
every adaptive `mutex_enter`/`mutex_exit` and every refcount.  The
restart adds nothing to the atomic.  On the trap it costs 2
instructions for a user trap, whose compare reuses the `tf_sr` load
`usermode` already needs, and 7 for a supervisor trap; the TLB refill
fast path never reaches `trap()` at all.

### Verification

- **Suite test.**  `isa/test_ras_restart` runs a CAS-increment loop
  against a timer handler writing the same word.  With the restart,
  `W == N + M`; without it, `W == N + M - INSIDE` — an exact count, so
  the control measures the race rather than smoke-testing it.
  Coverage is asserted per landing point, and the timer period is
  calibrated against `TM_COUNT`, since tick rate, prescaler and CPI are
  all implementation-defined.  Passes on the ISS, gen1 and gen2;
  removing the restart fails it, and so does treating the end marker as
  inside.
- **Kernel boundary.**  Injecting with the ISS's `+irq_at_pc` into the
  booting kernel, every other arrival: at the compare, 37,031 restarts;
  at the store, 37,073; at the end marker, none.  Every run booted to a
  shell.
- **Hardware.**  `vmstat -e` showed 22 restarts over 2h40m of mod
  playback alongside framebuffer rendering, with no audible glitch —
  and audio is the most latency-sensitive thing on the machine, so that
  bounds the trap-path cost as well.  An uninjected boot to single-user
  took one.

### What testing established

A spurious injected interrupt is not a negative control.  With the
restart compiled out, 37,352 interrupts into the region still booted
cleanly: `intr_dispatch()` with no claimer never writes the word the
interrupted CAS targets, so resuming and restarting are
indistinguishable.  Only a handler that writes the same word makes the
difference observable, which is what the suite test supplies.

The lower bound cannot be tested, and does not need to be.  An `EPC` at
the region's start means nothing has run, so rewinding there and
resuming are the same act, and treating it as inside passes every test
— correctly.  Only the upper bound can corrupt, and it is the one the
suite test's coverage and mutations are aimed at.

The planned DIAGNOSTIC assertion that the rewritten PC equals the
region start was dropped as tautological.  The evcnt is the check that
can fail, by staying at zero.

## Kernel: inline lock fast paths

`kern_mutex.c:274-283` aliases `mutex_enter`, `mutex_exit`,
`mutex_spin_enter` and `mutex_spin_exit` straight to the C vector
routines, because the port defines neither `__HAVE_MUTEX_STUBS` nor
`__HAVE_SPIN_MUTEX_STUBS`.  Every comparable port defines them (mips,
riscv, vax and alpha among others, alpha with the spin variant
commented out — a hint that it is the fiddlier half), supplying
assembly that handles the uncontended case inline and tail-calls the
vector routine otherwise.  These are on nearly every kernel path, so
this is the largest throughput item on the opt-in list.

Two things make it cheaper here than the line count suggests.
`MUTEX_SPIN_SPLRAISE` plus the port's `splraiseipl()` already reduce to
`RDSPR` and `DI`, so an uncontended `mutex_spin_enter` is a handful of
instructions with no call frame.  And `kern_mutex.c:269-272` undefines
both macros under `LOCKDEBUG`, so a debug kernel keeps the C path —
a safe way to stage the work.

The coupling to record: an inline fast path is another non-atomic
read-modify-write sequence, and needs the restart [the CAS
got](#kernel-interlocked-atomics-are-not-atomic-against-interrupts-resolved).
`kernel_ras_restart()` knows one region, so each fast path added
naively is another pair of bounds on every supervisor trap.  Linking
the regions contiguously keeps that flat: one range check rejects an
ordinary PC before any per-region test runs.  Each region also wants
its own assembly-time size guard, and the suite test's pattern — a
handler writing the same word, an exact count, coverage asserted per
landing point — carries over to each new sequence shape.

Inlining is also why `__HAVE_ATOMIC_AS_MEMBAR` was declined.  The flag
asserts that the port's atomics carry barrier semantics, which holds
today only because they are out-of-line calls and therefore compiler
barriers — the hardware fences are already no-ops
(`membar_ops_nop.c`), so the flag would remove nothing but that.  Inline
fast paths remove the compiler barrier, and a DMA engine would remove
the uniprocessor argument for no-op fences altogether.  Measured worth
at the time: 35 membar calls down to 7 across eight hot files.

## Kernel: per-CPU data layout and cache-line tuning

`cpu_info_store` is the hottest data structure in the kernel: every
exception entry reads `ci_curlwp` to establish curlwp, every exception
exit reads `ci_softints` to look for soft interrupts, every hardware
interrupt touches `ci_idepth`, and every spin-mutex acquire and release
touches `ci_mtx_count`.  Its layout has never been designed, only
accreted, and the MI half of it is padded for a machine we are not.

### Cache-line constants: measure what the smaller padding bought

`COHERENCY_UNIT` is 16, the true line size, and `CACHE_LINE_SIZE` is 32,
the smallest value MI code supports (`netinet/tcp_vtw.c` packs a fat
pointer's tags into one line and asserts 32, 64 or 128).  Both were at
the generic 64, padding structures apart against false sharing a
uniprocessor cannot have.

Per-structure effect, measured by compiling a probe: `struct filedesc`
652 → 460 bytes, `struct proc` 640 → 560, `struct kauth_cred` 164 → 116,
`FDFILE_SIZE` 64 → 32 (per open descriptor), `struct pool_cache` 448 →
416.  `MIN_LWP_ALIGNMENT` follows `COHERENCY_UNIT` down from 64 to 32,
which `sys/param.h` gives as the real requirement.

What is not measured is the whole-kernel effect: compare the
`.data.cacheline_aligned` section size and total BSS against a kernel
built with the generic values, and take a syscall loop's L1d miss count
from `machdep.cache.*` for the behavioural half.

### The port's own fields fit in padding that already exists

`struct cpu_info` is 896 bytes, of which 768 are the MI `struct
cpu_data` and 72 are padding.  `ci_data` is 64-byte aligned because of
`lwp_t *cpu_idlelwp __aligned(64)` inside it, so offsets 4-63 are a
hole — and that hole is larger than everything the port declares:

| Bytes | Fields |
|---|---|
| 8 | `ci_curlwp`, `ci_softints` |
| 16 | `ci_onproc`, `ci_cpuid`, `ci_dev`, `ci_want_resched` |
| 16 | `ci_idepth`, `ci_cpl`, `ci_mtx_count`, `ci_mtx_oldspl` |
| 16 | `ci_softlwps[4]` |

56 bytes total.  Moving all of them ahead of `ci_data` puts the entire
machine-dependent half in the first four lines, costs nothing, and
drops `cpu_info` to 832 bytes.  `ci_softints` was moved there when the
fast softint path landed, for the trap-exit read; the rest still sits
past the 768-byte MI structure, where nothing groups it.

Worth doing in the same pass: `ci_cpl` is declared and never read or
written.  The SPL model derives the level from `SR.I` and the soft
level from the LWP, so the field is four dead bytes and an invitation
to assume a software `cpl` exists.

### Pinning the hot page

Kernel data is mapped by ordinary 4 KB PTEs through the 64-slot,
2-way main TLB, so `cpu_info_store`'s page competes for one of 32 sets
against everything else the kernel touches.  Three of the eight pinned
slots are free (`sysreg.h` assigns 0-4).  A pinned page holding the
per-CPU structure — and whatever else the trap path dereferences on
every exception — would take that competition off the table entirely,
the way the vector page already does for the miss handler.

Open questions before it is worth building: whether the page actually
misses often enough to matter, which needs a measurement rather than
an argument; what else deserves to share it, since 4 KB for an 832-byte
structure wastes most of a page and a pinned slot is scarce; and the
mechanics, which are a linker section plus a pin at boot in
`pmap_bootstrap`, alongside the existing pinned-slot assignments.

## NetBSD: MD inline bswap existed but was never wired in — RESOLVED

`SHA256_Transform` (libc `sha2.c`, which NetBSD's libcrypto also links
as `libc-sha256`) called out-of-line `__bswap32` once per message word
— a call plus spill traffic where an inline shift/or expansion is
straight-line ALU; `htonl`/`ntohl` across the network stack (kernel
and userland) paid the same call.  Root cause was a wiring omission,
not a missing implementation: `penumbra/byte_swap.h` has had correct
`__BYTE_SWAP_U16/U32_VARIABLE` inlines since port bring-up, but
`penumbra/bswap.h` never included it, so `sys/bswap.h` fell back to
the libc functions.  Fixed by including it as
`<machine/byte_swap.h>`, `_LOCORE`-guarded — the i386/vax single-arch
convention, not riscv's `<riscv/...>` arch-name form: only kernel and
userland builds provide the arch-name include alias, while stand
(bootloader) builds alias `machine/` alone, so the arch-name form
breaks `stand/boot`.  `byte_swap.h` also switched
from `sys/types.h` to `sys/cdefs.h`+`sys/stdint.h` — the heavyweight
include recursed through `sys/endian.h`, whose inline codecs need
`bswap32` before `sys/bswap.h`'s declarations exist when the bswap
header is the entry point.  Probe-verified both include orders inline
to ~13 instructions with constants folding; remaining validation is a
world rebuild confirming `sha2.o` loses the `bl __bswap32`.
