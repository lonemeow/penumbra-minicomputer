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
trip** on the ULX3S FPGA at 25 MHz (measured 2026-05-15).  That's two
orders of magnitude beyond what the workload should cost, and it makes
the system painful to use: `/etc/rc` runs many short commands
sequentially, single-user boot stalls visibly, and any shell pipeline
is sluggish.  For
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

## Kernel: count TLB refills

Nothing counts refills.  `_real_miss_handler` resolves them without
entering `trap()`, so neither `vmstat -s` nor an evcnt sees them, and
the CPU perfctrs count the handler's ~59 instructions as instructions
of whatever was running.  The TLB's shape, and why its misses move
with layout, are in "The layout lottery" in
[design-notes.md](../system/netbsd/design-notes.md).

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

### What pinning can reach (2026-09-23)

A four-workload sweep on `GENERIC` -- shell and filesystem work, the
pbench syscall and libc rows, and Doom -- bounds the pinned-slot
lever.  Kernel refills divide as text 46-59%, static data ~11%, heap
and other runtime KVA 30-43%, varying by workload; user VA takes a
further third of all refills.

They are spread far too thin for a handful of slots to matter.  Kernel
text spans 259 pages that take at least one refill and kernel data
2,919.  The hottest single text page is 1.5% of all refills, the
hottest data page 1.6%, and the top twenty text pages together reach
16.5%.  Two slots are free after the structural ones, which bounds the
whole exercise at low single digits -- before the partial-evacuation
problem, which is that moving one routine off a page removes only the
entries it accounted for while the page keeps faulting for whatever
else lives there.

Almost every hot data page sits past `_end`, where no link-time
arrangement reaches it.

This corroborates rather than adds: the shadow-TLB models in "What the
TLB's shape costs, and which parts are reachable" already put
associativity an order of magnitude ahead of placement.  Placement
earns its keep because it is free and because a bad roll is expensive,
not because it can close the gap.

## Kernel: a TLB refill can split a TLB sysreg sequence

Programming a TLB slot is a sequence of sysreg writes — `TLB_INDEX`,
then `TLB_VPN`, then `TLB_PTE`, which commits the entry — and a lookup
reads a slot back after selecting it.  The miss handler writes
`TLB_INDEX` and `TLB_VPN` for its own refill and does not restore them,
so a miss taken inside a sequence redirects the rest of it.  A
`pmap_pin_slot()` cut before its `TLB_PTE` write commits the pinned
mapping into the main-TLB slot the refill just filled, under the
refilled page's VPN.  `splhigh()` around the sequences masks
interrupts, but a TLB miss cannot be masked.

Kernel text is mapped through the main TLB and the sequences make no
data accesses, so the one miss that can land inside a sequence is the
instruction fetch crossing into the next page.  Whether a sequence
straddles a page is decided by the link.  A scan of the 2026-09-27
`GENERIC.DEBUG` kernel found 28 `TLB_INDEX` → `TLB_PTE` sequences in
`.text` — the inlined `pmap_pin_slot()` in about a dozen pmap
functions, and `tlb_subr.S` — and none crossing a page, but a change to
any code linked before one can move it across, and the corruption then
comes and goes with unrelated changes.  The trap page's handlers are
not exposed: they run from the pinned vector page.

Open, either of:

- A build-time check that fails the link when a sequence straddles a
  page: the scan above, over the kernel's disassembly, pairs each
  `wrsys …, #MMU_TLB_INDEX` with the next `wrsys …, #MMU_TLB_PTE`
  (about 20 lines of Python).
- Sequences immune to the refill: the handler saving and restoring
  `TLB_INDEX` and `TLB_VPN` costs two `RDSYS` and two `WRSYS`, ~17
  cycles on a ~137-cycle refill, the hottest kernel path; running the
  sequences from the vector page covers `tlb_subr.S` but not the
  inlined `pmap_pin_slot()`.

## Kernel: networking — first NIC through the USB host controller

Decision: the first network adapter is a **USB NIC on the CLASS_USBHC
port**, not a dedicated NIC device. Rationale: the HCD is already owed
for the local-console keyboard, and once it exists the whole MI USB
stack plus every in-tree USB Ethernet driver (`axe`, `aue`, `cdce`, …)
comes along — no new RTL, no device firmware, no from-scratch NIC
driver. A board with an Ethernet PHY of its own takes a MAC device
instead, the preferred path there (`hw-devices.md`, "Hardware +
kernel: Ethernet MAC for an onboard PHY"). A Wiznet W5500 on the
existing SPI class stays possible (driver-only, needs external
hardware), with a lower throughput ceiling than software-driven
full-speed bulk. The ESP32 bridge is ruled out ("Ruled out").

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

`__HAVE_ATOMIC_AS_MEMBAR` is defined as of `1966179`.  It had been
declined here on the reading that the flag would take away the
compiler barrier an out-of-line `membar_*()` call supplies.  That is
not what it takes away.  MI code places each barrier against an
atomic, and the atomic is itself an out-of-line call, so the ordering
the compiler must not break is already fenced where it matters.
`kern_rwlock.c` has the shape: the release barrier sits ahead of a
`rw_cas()` loop and the acquire barrier behind a `rw_cas()`, both
reaching `atomic_cas_ulong_ni` by `bl`.

The valuation was wrong as well, counting call sites — 35 down to 7
across eight hot files — rather than what they cost.  Each call is a
branch to a bare return, but they also held a page in the TLB that the
hot paths never otherwise enter: removing them took 88% of 4.4M
dynamic calls, 4.5% of TLB refills and 1.75% of instructions on a
shell workload, nearly all of it refill-handler time.

What the flag does require is that the atomic stay a compiler barrier
— an out-of-line call, or inline asm with a memory clobber.  Assembly
stubs reached by `bl` keep that; inlining a read-modify-write into C
would not.  The condition is recorded at the definition in
`machine/types.h`.

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

## Kernel: physical memory setup keeps off the vector page by placement

`penumbra_physmem_init()` (`startup.c`) hands UVM every `MEMORY`
region, less the part of the kernel's own region below the end of
the kernel image and message buffer. That exclusion is the only thing
keeping UVM off page 0, where the architecture fixes the exception
vector table, and it covers page 0 only because PENBOOT loads the
kernel into the region that starts at 0. A kernel loaded into any
other region — base RAM too small for it, the kernel in expansion
RAM — would have page 0 handed out as a free page while its vectors
live there. The same exclusion also strands everything below the
kernel (the ROM's scratch buffer, PENBOOT and its heap) for the life
of the system; bootinfo is not at risk, since `locore.S` copies it
into `bootinfo_store` first.

Open: reserve the vector table's page explicitly, and release the
rest of the RAM below the kernel, which holds nothing the kernel
reads once bootinfo is copied.

## NetBSD: PENBOOT's stack sits at a fixed address

PENBOOT's `crt0.S` starts the stack at `0xB000`, "just below where
loader is loaded", and `boot.c` reserves a fixed `0`–`0xC000` for
"vectors, boot data, CRT stack" before placing its heap and the
kernel. Neither comes from the boot data, contrary to the handoff
contract ([boot-protocol.md: ROM to Loaded
Program](../system/boot-protocol.md#rom-to-loaded-program)). `0xB000`
is right only by the arithmetic of the ROM's placement: a 35,224-byte
`PENBOOT.ELF` is read into a scratch buffer at `0x2000`–`0xA998`, and
the image goes on the next page, `0xB000`, so the stack grows down
into the dead scratch buffer. A file under 32 KB loads at `0xA000`
instead, and the stack then starts inside PENBOOT's own image and
overwrites it as it grows, silently.

Open: give PENBOOT its stack in `.bss`, and reserve only the vector
table, the boot data (`total_size` bytes at R1) and its own image in
place of the fixed low range.

## Ruled out

- **Refill way pick: set bits mixed into the counter bit, or an empty
  way first.**  The refill handler takes its way from bit 5 of
  `CPU_CYCLES`.  XOR-ing set bits into it, to spread adjacent sets'
  misses across both ways, only relabels each set's two ways, so no
  hit or miss can change; shadow TLBs on the ISS matched the plain
  pick to the last refill on a fork/exec loop, `pbench kernel` and the
  Doom timedemo.  Taking an empty way first moved the same workloads
  by under 0.01%: past boot, a set almost never has one.  What does
  move refills is replacement that sees hits, which software cannot:
  on those workloads LRU took 22-31% fewer refills than the kernel's
  pick.

- **Pinning hot kernel pages.**  Hot text and data were gathered into
  fixed regions, linked first and pinned into the slots the MMU leaves
  spare.  Five hardware runs could not separate it from noise: builds
  differing only by a few hundred bytes of shift moved `getpid` by up
  to 28%, and once the region's successor was aligned so tenant edits
  moved nothing else, changing the tenant list made no measurable
  difference at all.  The lever is bounded in any case -- two spare
  slots against 259 kernel text and 2,919 data pages that take
  refills, the hottest single page 1.5% of the total ("Kernel: count
  TLB refills", "What pinning can reach").  Reverted along with
  `-ffunction-sections`, which existed only to name individual
  routines for it.

- **Uncached MMIO `STW` as a hardware write-path asymmetry.**  The
  write path is symmetric: RTL-sim microbenches put an uncached `STW`
  and `LDW` at the same ~4 cycles.  An observed 2.4× gap came from
  the kernel's `-fno-strict-aliasing`: with TBAA off, the compiler
  cannot prove a volatile MMIO store does not alias an in-memory
  `bus_space_handle_t`, so it reloads the handle before every write.
  Drivers avoid it with the `bus_space_*_multi` primitives, which take
  the handle by value (`9ddfcf4aa941`).

- **The ESP32 bridge as a NIC.**  The ESP32 carries Ethernet frames
  to and from WiFi over a SLIP-framed UART.  It needs custom firmware
  built and flashed onto the ESP32, and the kernel sees the WiFi link
  only through a private protocol and its own configuration tool.
  USB Ethernet and a MAC for an onboard PHY each give a standard
  Ethernet interface under stock drivers.
