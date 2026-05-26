# L2 Cache — Design Plan

This document specifies the design for an optional L2 cache that sits
between the CPU's `o_mem_*` port and the system bus.

For protocol context this document depends on — and does not duplicate —
read [`cpu-bus.md`](cpu-bus.md) (CPU-internal bus contract) and
[`bus-protocol.md`](../hardware/bus-protocol.md) (Penumbra Bus — the
contract the L2's front-side and back-side ports both honor, in the
[sync form](../hardware/bus-protocol.md#sync-bus-mapping)).

## Implementation Status

| Phase | Description | Status |
|---|---|---|
| 0 | Bus shape (`o_mem_cacheable`, build-time gate, `l2_passthrough` stub) | **DONE** (gate later removed) |
| 1 | Real L2 cache: read+write-invalidate-on-hit, INVAL_ALL | **DONE** (superseded by 1.5) |
| 1.5 | Write-through, write-no-allocate (WT-WnA): write hits update the cached line via byte-en instead of dropping it | **design** |
| 2 | Write-back, write-allocate, dirty bit, FLUSH ops | pending |
| 3 | 1-deep writeback buffer (overlapped fill+writeback) | pending |
| 4 | Perfctrs (READ_HITS / READ_MISSES / WRITE_HITS / WRITE_MISSES on `SYSDEV_L2_CACHE` regs 10-13) | **DONE** |

What's wired up today:

- `hw/rtl/soc/l2_cache.sv` — the real module.  64 KiB, 4-way,
  tree-PLRU, 2-cycle hit pipeline.  Write policy started as
  **write-invalidate-on-hit** (phase 1) and is being upgraded to
  **write-through, write-no-allocate** (phase 1.5) — under WT-WnA
  a write hit updates the cached line's data via byte-en and
  also forwards the store to memory, instead of dropping the
  line.  See the "Write Policy" and "Phasing" sections below.
  Disabled at reset; software (kernel or bare-metal harness, not
  the ROM) brings it up via `WRSYS SYSDEV_L2_CACHE CTRL=1`.
- `hw/rtl/sim/machine_sim.sv` and `hw/rtl/fpga/ulx3s_top.sv`
  always instantiate `l2_cache` directly between the CPU's memory
  port and the shared bus.  Sysreg device 9 routes to it
  unconditionally.  Software opt-out is preserved at runtime via
  `CTRL.enable=0` (the reset state) — while disabled the cache is
  a combinational pass-through, so `INFO != 0` + `CTRL.enable=0`
  is functionally indistinguishable from a no-L2 build.
- `hw/sim/tb_l2_cache.cpp` — standalone Verilator testbench
  covering the L2 contract: INFO presence + unified-layout
  decode (PIPT/WT/WnA), pass-through-when-disabled, miss→hit,
  write-hit data update (under phase 1.5; was write-invalidate
  in phase 1), INVAL_ALL walker + implicit fence, uncached
  pass-through, perfctr counts.
- Boot ROM (`hw/rom/boot_rom.c`) prints `L2 cache: 64kB
  (1024 x 16B, 4-way) unified` in the banner.  The ROM **does
  not** enable the L2 (or any cache, or the MMU) — that's the
  OS's job, by long-standing project convention.
- Benchmark harness (`benchmark/common/crt0.S`) enables the L2
  if present (`RDSYS SYSDEV_L2_CACHE INFO != 0`).  The probe is
  preserved so this same crt0 still handles a hypothetical
  future L2-less bitstream variant without code change.

Verification gates that have to stay green on every L2 commit:
`make sim MOD=l2_cache`, `make test`, `make test-modules`,
`make test-iss`, and `make fpga-lint TOP=ulx3s_top` (zero
warnings).  Hardware-side benchmark numbers are what tell us
whether L2 is actually paying off — quote those from the
benchmark output, not from this doc.

## Goals

1. **Reduce average miss penalty.**  L1 misses currently cross the
   `bus_adapter → CDC → controller` chain to async SDRAM.  Per the
   `project_memory_layer_costs` finding, that path dominates miss cost
   (~85 %) regardless of SDRAM controller optimisations.  An L2 hit
   stays on the system clock domain and never crosses the CDC bridge.
2. **Absorb L1 D-cache write-through traffic.**  L1-D is write-through
   write-no-allocate.  Every store currently round-trips to SDRAM.
   With L2 acting as a write-back layer, stores terminate in BRAM.
3. **Tolerate multitasking pressure.**  NetBSD context switches and
   kernel/user code interleaving inflate working sets and conflict
   miss rates.  The L2 must be sized and associated to absorb this,
   not just hold one userland's hot loop.
4. **Stay tunable.**  Geometry parameters (size, ways, line bytes) live
   on `l2_cache` so smaller FPGA targets can shrink the BRAM footprint
   and benchmark sweeps can vary associativity.  Runtime opt-out is
   the reset state (`CTRL.enable=0`), so any build can boot as
   "no-L2" without recompilation.

Explicitly **not goals** (see also "Future Work" below):

- Multicore / cache-coherent DMA.  Single-core only; DMA coherence is
  software-managed via cache-maintenance sysregs.
- Inclusivity guarantees with L1.  Non-inclusive non-exclusive (NINE)
  is sufficient for single-core and minimises bookkeeping.
- L3 or further levels.  All BRAM-resident; one cache level past L1
  exhausts the meaningful latency tiers on the ECP5.

## Placement

L2 sits *outside* `cpu_core`, on the synchronous side of the system
bus.  It is the first thing `o_mem_*` hits before address decoding.

```
┌── cpu_core ──────────────────────────────┐
│ L1-I            L1-D                     │
│   └────┐    ┌────┘                       │
│        │    │                            │
│      memory port mux                     │
└────────────┬─────────────────────────────┘
             │  o_mem_* (sync-wrapped Penumbra Bus)
             │  + new o_mem_cacheable hint
             ▼
        ┌─────────────┐
        │  l2_cache   │  ← new module (this plan)
        └──────┬──────┘
               │  same-shape downstream port
               ▼
        bus_devsel ──► SDRAM adapter ─► CDC ─► controller ─► SDRAM
                  ├── boot_rom
                  ├── UART
                  ├── SPI
                  └── …
```

L2 must precede `bus_devsel` (not sit underneath it) because:

- A dirty L2 line must drain before any MMIO access that races with
  it.  Sitting in front of the decoder serialises all RAM-bound
  traffic through the same cache.
- The address-cacheable decision is per-page (PTE.C), not per-device.
  A DMA buffer in RAM marked uncacheable must reach the SDRAM
  controller without L2 caching it.

## Bus Interface

L2 honours the **same protocol on both ports**.  Anything `cpu_core`
emits, `l2_cache` accepts; anything `l2_cache` emits downstream, the
SDRAM `bus_adapter` already accepts (it's the same shape currently
connected to `cpu_core.o_mem_*`).

```
        ┌───────────────────────────────┐
        │           l2_cache            │
in ─────┤ i_paddr, i_wdata, i_byte_en   │
        │ i_re, i_we                    │
        │ i_cacheable          (NEW)    │
        │ o_rdata, o_busy               │
        │                               │
        │ o_mem_addr, o_mem_wdata,      │
        │ o_mem_byte_en, o_mem_we,      │
        │ o_mem_re                      ├──── out
        │ i_mem_rdata, i_mem_busy       │
        │                               │
        │ i_sys_reg, i_sys_wdata,       │
        │ i_sys_we, o_sys_rdata         │
        └───────────────────────────────┘
```

### Cacheability hint

`cpu_core` gains a new output `o_mem_cacheable` (one bit).  It is the
L1's forwarded copy of `mmu.o_cacheable` for the access currently on
the memory port.  L2 honours it as follows:

| `i_cacheable` | L2 behaviour |
|---|---|
| 1 | Look up tag.  Hit → serve from BRAM.  Miss → allocate, fill, install. |
| 0 | Pass through immediately (do not look up, do not install).  Writes do **not** invalidate any L2 line — by convention an uncacheable region must not alias a cacheable one. |

This bit replaces the alternative of compiling cacheable address
ranges into L2.  Routing the PTE.C decision through the bus keeps L2
oblivious to the system memory map and consistent with `module_independence`.

### Timing contract

L2 inherits the L1 cache contract from [`cpu-bus.md`](cpu-bus.md):

- **Hit:** `o_busy=0` and valid `o_rdata` in the same cycle as `i_re`.
  L2 hits are pipelined (≥ 2 cycles BRAM read + tag compare), so this
  applies after the pipeline has filled — i.e., for streaming hits the
  contract is "1 hit per cycle, drop = valid", not "0 latency from
  request to result."  The CPU-internal STALL sequencer already
  tolerates the latter via `cache_busy`.
- **Miss:** `o_busy=1` from the same cycle, held until refill
  completes (line allocated, optionally writeback completed for
  evicted dirty victim).  On the cycle `o_busy` drops, `o_rdata`
  carries the requested word.
- **Pass-through (uncacheable):** `o_busy` mirrors downstream
  `i_mem_busy`.  L2 introduces no extra cycle vs. the no-L2 build.

## Internal Organisation

### Parameters

All sizes are parameters with the defaults below.  The values were
chosen for NetBSD-class workloads on ULX3S; benchmark sweeps will
inform tuning.

| Parameter | Default | Notes |
|---|---|---|
| `CACHE_BYTES` | `65536` | 64 KiB.  Sized to absorb kernel + one userland working set across context switches. |
| `LINE_BYTES` | `16` | Equal to L1 line.  Avoids sub-line tracking. |
| `NUM_WAYS` | `4` | NetBSD ⇒ multitasking ⇒ conflict-miss pressure. |
| `NUM_SETS` | `1024` (derived) | `CACHE_BYTES / (LINE_BYTES * NUM_WAYS)` |
| `HIT_LATENCY` | `2` | BRAM read (1) + tag compare/way mux (1).  Parameter-stub; reduce only after fmax study. |
| `REPLACEMENT` | `tree-PLRU` | 3 bits/set.  Miss rate is within noise of true LRU at 4-way (per `l2_cache.sv:24-26`), and the update logic is 3 bit flips per access vs LRU's 6 pairwise relations.  True LRU was the design alternative; deferred because routing cost wasn't worth the miss-rate noise. |
| `WRITE_POLICY` | `write-through, write-no-allocate` | Phase 1.5 policy: write hits update the cached line via byte-en and pass the store downstream; write misses pass-through unchanged.  `INFO` advertises WT/WnA (which was already the software-visible policy under the phase-1 write-invalidate-on-hit predecessor).  Write-back / write-allocate is the Phase 2 plan — see the "Write Policy" section below. |

`CACHE_BYTES`, `NUM_WAYS`, and `LINE_BYTES` are the primary sweep
axes.  At default geometry, tag = `32 - log2(NUM_SETS) - log2(LINE_BYTES)`
= `32 - 10 - 4` = `18` bits.

### Storage

```
Tag SRAM    : NUM_SETS rows × NUM_WAYS columns × (TAG_BITS + valid + dirty)
Data SRAM   : NUM_SETS × NUM_WAYS × LINE_BYTES bytes
LRU SRAM    : NUM_SETS rows × LRU_BITS bits
```

For the default geometry that is approximately:

- Tag: 1024 × 4 × 20 ≈ 80 Kbit (~5 EBR)
- Data: 1024 × 4 × 128 = 512 Kbit (~30 EBR)
- PLRU: 1024 × 3     = 3 Kbit  (in flops, not EBR)

Total ≈ **35 EBR / 208** on ECP5-85F, leaving ample BRAM for the rest
of the system.  At 16 KiB total this drops to ~10 EBR and at 8 KiB
to ~6 EBR — a useful operating point for the smaller ECP5-25F variant.

### Pipeline (default `HIT_LATENCY=2`)

```
Cycle 0: address in.  BRAM tag read + BRAM data read launched.
Cycle 1: tag compare.  Way mux.  o_rdata + o_busy=0 on hit.
         On miss: o_busy held high, allocate state engaged.
```

If fmax study later shows the tag-compare + way-mux + drive-out path
is critical, `HIT_LATENCY=3` adds an output flop without changing the
contract — `cpu_core` already tolerates multi-cycle misses.

### Replacement: 4-way tree-PLRU

Tree-PLRU stores 3 bits per set encoding a binary tree over the 4
ways: bit 0 picks the LRU pair (ways 0–1 vs. 2–3), bit 1 picks the
LRU way inside the (0, 1) pair, bit 2 picks the LRU way inside the
(2, 3) pair.  On every access the touched way becomes MRU and the
three bits update in combinational logic.  Encoded in flops (not
EBR) — see `hw/rtl/soc/l2_cache.sv:179` and the bit-meaning comments
at lines 360–362.

True LRU was the design alternative — it needs 6 bits/set encoding
the ordering of all four ways (`C(4,2) = 6` pairwise relations) and
matches tree-PLRU's miss rate within noise at 4-way.  Tree-PLRU
was chosen because the update logic is 3 bit flips per access vs.
LRU's 6 pairwise relations, and the routing on 1024 sets stays
cheaper.

## Write Policy

**Phase 1.5 (design): write-through, write-no-allocate (WT-WnA).**
Every store is forwarded to the downstream bus.  On a tag hit, the
L2 also updates the cached line's data in place via byte-enable,
keeping the line valid for subsequent reads.  On a tag miss, the
store passes through unchanged — no allocation, no dirty state.
Software-visibly this is `WRITE_BACK=0, WRITE_ALLOC=0` in `INFO`.

The key behavioural difference vs the phase-1 predecessor: hot
read-modify-write lines (kernel page-table updates, repeatedly-
accessed globals, ring-buffer heads/tails) now persist in L2
across writes.  A subsequent read of the line — including an L1
read-miss that finds the line in L2 — is a hit instead of a
write-induced miss.  This is the dominant first-order effect of
the phase-1 → phase-1.5 upgrade.

L1 D-cache write traffic itself is not absorbed: every store
still pays an SDRAM round-trip.  Absorbing the store traffic is
phase 2 (WB+WA).

**Phase 1 (superseded): write-invalidate-on-hit.**  The original
phase-1 policy passed writes through and *dropped* the L2 line on
hit (valid bit cleared).  Software-visibly identical to WT-WnA
(`WRITE_BACK=0, WRITE_ALLOC=0`).  The internal difference made
WT-WnA workloads — anything with read-after-write locality —
underperform, because each write evicted its own line from L2 and
the next read paid an SDRAM trip to refetch it.  The phase-1.5
upgrade removes this self-eviction without changing any
software-visible bit.

**Phase 2 (planned): write-back, write-allocate.**  The headline
performance commit.  Once phase 2 lands, this section describes
the runtime behaviour:

- **Write hit:** Update the data array, set `dirty=1`.  No bus
  traffic.
- **Write miss:** Allocate (read line from memory), then update the
  in-cache copy and set `dirty=1`.  This converts a single store into
  one read-line + dirty-mark, which is more bus traffic up front but
  amortises across subsequent writes to the same line.
- **Eviction:** If the victim line has `dirty=1`, write back to memory
  before installing the new line.  Sequence:
  1. Begin writeback of victim (16 B = 4 bus words).
  2. Begin fill of new line.
  3. Both share the downstream port; serialise (writeback first).
- **Clean eviction:** Discard.

A small write-back buffer (1–2 lines) decouples the eviction
writeback from the fill of the incoming line.  Without it, the worst-
case fill latency is `WB_LATENCY + FILL_LATENCY`; with a 1-deep
buffer, an immediate fill can start while the writeback drains in
the background.  Defer the buffer to phase 3 (see Phasing) if the
naïve serial implementation hits perf targets.

## Sysreg Device

L2 uses **`SYSDEV_L2_CACHE = 9`** (next free after MACH=8) and exposes the
**unified cache device register map** documented in
[`sysregs.md`](../system/sysregs.md#cache-devices-2--l1_dcache-3--l1_icache-9--l2_cache) —
the same layout as `L1_DCACHE` and `L1_ICACHE`.  Software discovers
presence by reading `INFO`; `INFO == 0` means no L2 in this build
(e.g. a hypothetical future variant that drops the `l2_cache`
instance).  The kernel can reuse one cache-ops driver across L1 and
L2 because every register at every offset means the same thing.

`INFO` decode under the current policy:

- `ADDRESSING = PIPT` — L2 sees post-translation addresses.
- `WRITE_BACK = 0`, `WRITE_ALLOC = 0` — software-visibly write-
  through, write-no-allocate.  True for both phase 1 (write-
  invalidate-on-hit) and phase 1.5 (true WT-WnA): the difference
  between those policies is internal cache behaviour, not the
  software-visible contract.
- `NUM_SETS = 1024`, `NUM_WAYS = 4`, `LINE_WORDS = 4`.

Multi-cycle operations (`INVAL_ALL`, eventually `FLUSH_ALL`) signal
completion via `STATUS.busy`; the sysreg interface itself stays
single-cycle.

**Post-reset auto-INVAL walker.**  BRAM has no clear at reset, so
`l2_cache` runs an automatic INVAL_ALL walk after `i_rst` deasserts
to zero every valid bit before any line lookup can hit.  During the
walk, `STATUS.busy=1` and an internal `ready` flag (`l2_cache.sv:150-163`)
forces the cache to act as a pass-through regardless of `CTRL.enable`.
Software bring-up that writes `CTRL.enable=1` immediately after reset
must therefore poll `STATUS.busy=0` before relying on hit traffic — at
4096-cycle worst case (`NUM_SETS × NUM_WAYS`) this is rarely a race in
practice but is worth knowing when writing `crt0.S` / `locore.S`
sequences that probe the cache right at boot.

**`INVAL_LINE` is reserved-not-implemented** in current RTL across
all cache levels.  Rationale: L1 full-flush is a single-cycle valid-
bit clear (cache holds 64 lines max, smaller than any realistic
invalidate range); L2 is PIPT + uncached-MMIO project convention
means no current code path needs it.  Defer until a workload
(e.g. a future cached-DMA path or a JIT that keeps code lines
warm in L2) actually wants it.

The `INVAL`/`FLUSH` distinction follows ARM's c7 ops:

- **Invalidate** = drop without writeback.  Dangerous if dirty.  Use
  before reading a buffer that DMA just wrote.
- **Flush** (a.k.a. clean) = writeback dirty, line stays valid.  Use
  before starting a DMA-out so the device sees current data.

`STATUS.busy` lets the kernel poll completion of bulk ops without
blocking the sysreg bus.  The sysreg interface itself is single-cycle
(per `cpu-bus.md`); the cache machinery runs asynchronously to it and
reports completion through `STATUS`.  This is the same pattern the
implementer note in `cpu-bus.md` recommends ("if a register read
genuinely cannot complete in one cycle, expose status separately").

## Cache Maintenance Operations

Software-managed coherence flows from `WRSYS SYSDEV_L2_CACHE, *`.  Three
canonical patterns:

**DMA-out (CPU prepares a buffer, device reads RAM):**
```
for line in buffer: WRSYS SYSDEV_L2_CACHE, FLUSH_LINE, line_addr
WRSYS device, START
```

**DMA-in (device writes RAM, CPU reads):**
```
WRSYS device, START
… wait completion …
for line in buffer: WRSYS SYSDEV_L2_CACHE, INVAL_LINE, line_addr
read buffer
```

**I-cache coherence after RAM-loaded code:**
```
for line in code: WRSYS SYSDEV_L2_CACHE, FLUSH_LINE, line_addr
                  WRSYS SYSDEV_L1_DCACHE, INVAL  (already exists)
                  WRSYS SYSDEV_L1_ICACHE, INVAL  (already exists)
```

NetBSD's `pmap` and bus-DMA layer already invoke architecture-
specific cache hooks; the L2 ops slot in alongside the existing L1
hooks.

## Runtime Bypass

The L2 is always instantiated in the current bitstream, but software
can leave it inert by never setting `CTRL.enable`.  This is the reset
state — `cpu_core` boots with the L2 acting as a combinational
pass-through; CPU cycles look identical to a no-L2 build until the
kernel (or bare-metal harness) explicitly does:

```
WRSYS r1, SYSDEV_L2_CACHE, CTRL  ; r1.bit[0] = 1
```

Why this works:

- The disabled path mirrors `i_mem_busy` straight through and feeds
  back-side reads/writes directly onto the shared bus, so latency and
  bus visibility match the pre-L2 wiring exactly.
- `o_mem_cacheable` flows into `l2_cache` regardless of enable state;
  when disabled it's just ignored.
- `RDSYS SYSDEV_L2_CACHE, INFO` returns the geometry encoding even when
  disabled — software discovers "L2 is present, currently bypassed" and
  decides whether to enable.

A future FPGA target that needs to reclaim the ~half-BRAM the L2
occupies can drop the `l2_cache` instance entirely and tie the
sysreg fan-in for ID 9 to zero; software's `INFO != 0` probe in
crt0.S / locore.S handles that case unchanged.

**History note.**  An earlier `HAS_L2` build-time parameter and a
phase-0 `l2_passthrough.sv` stub used to gate this — that scaffolding
was retired once phase 1 shipped because `CTRL.enable=0` covers the
same use case at runtime with no Makefile plumbing or stamp files.

## 74xx Discrete Feasibility

The L2 design is feasible in 74xx but is the most BRAM-hungry module
in the system, and the discrete equivalent maps to off-CPU SRAM
chips, not internal logic.  Notes for that build:

- **Tag SRAM:** small (~80 Kbit at default geometry) — well within a
  single SRAM chip.  Tag compare + way mux is a few '85 magnitude
  comparators and a 4:1 mux per data byte.
- **Data SRAM:** 64 KiB at default geometry — fits in a single
  modern asynchronous SRAM, or a pair of 32 KiB chips for 32-bit
  lanes.
- **Replacement:** True 4-way LRU with 6 bits/set in a small SRAM is
  feasible.  Pseudo-LRU is even cheaper (3 bits/set).
- **Pipelining:** The discrete build can adopt the same `HIT_LATENCY`
  model — input latch, SRAM access, output latch.  Async SRAM
  imposes a different timing budget, but the protocol contract
  (drop = valid) is unchanged.
- **Write-back machinery:** A small FSM controlling read-modify-
  writeback on eviction.  No special discrete pitfalls.

The two cost knobs that change in discrete vs. FPGA: SRAM chips are
cheaper to oversize than EBRs (so a discrete build naturally favours
larger L2), but write-back FSM gates are more expensive.  Both lean
toward "keep the controller logic simple, scale capacity."

## Verification Plan

Per project convention every new behaviour is gated on a test
landing in the same commit.  Plan:

1. **Unit testbench** `tb_l2_cache`:
   - Read hit / read miss with allocation.
   - Write hit (dirty bit set, no bus traffic).
   - Write miss (allocate, dirty bit set).
   - Dirty eviction triggers writeback before fill.
   - Invalidate-line drops one set's matching way without writeback.
   - Invalidate-all clears valid bits across all sets.
   - Flush-line writes back a dirty line and keeps it valid.
   - Flush-all walks every set, writing back dirty lines.
   - Uncacheable access bypasses entirely (no tag lookup, no install).
2. **Property assertions** on the same lines as `cache.sv`:
   - Read-hit ⇒ `!o_busy` (single-cycle drop contract).
   - Writeback-before-fill on dirty eviction (no out-of-order bus
     accesses that would drop the dirty data).
3. **Integration tests** via `machine_sim`:
   - Existing program test suite passes unchanged with L2 disabled
     (reset state, exercised by every test that doesn't touch
     `SYSDEV_L2_CACHE`) and with L2 enabled (the `test_l2_*` programs).
   - `test_l2_*` programs exercise INVAL/FLUSH from C.
4. **Performance regressions:**
   - Dhrystone before/after, expect CPI to drop (read-after-write
     locality preserved under phase 1.5; write traffic absorbed
     under phase 2).
   - Membench before/after, expect cached W/H/B sweep MB/s up.
   - L2 perfctrs (`SYSDEV_L2_CACHE` regs 10-13) directly show the
     effect: phase 1.5 should drop `READ_MISSES` and grow
     `WRITE_HITS` vs phase 1; phase 2 should drop bus-side write
     traffic without changing the perfctr counts (HIT/MISS is by
     tag check, policy-invariant).  Quote those from a fresh
     benchmark run, not from this doc.

## Phasing

Land in commit-sized increments per `feedback_incremental_commits`:

1. **Phase 0 — bus shape.** *(DONE; build-time gate later retired)*
   Added `o_mem_cacheable` through cache → arbiter → cpu_core → top,
   landed `l2_passthrough.sv` and wired via a `HAS_L2` build
   parameter that defaulted to off so pre-L2 bitstreams stayed
   byte-identical.  Once phase 1 shipped, `HAS_L2` and
   `l2_passthrough.sv` were both removed — the cache is now
   always instantiated and `CTRL.enable=0` provides the same
   pass-through behaviour at runtime.

2. **Phase 1 — real read cache.** *(DONE; superseded by 1.5)*
   Implemented in `hw/rtl/soc/l2_cache.sv` (64 KiB, 4-way,
   tree-PLRU, 2-cycle hit pipeline).  Replaced `l2_passthrough`
   (deleted — `l2_cache` with CTRL.enable=0 covers the same
   behaviour).  Tag/data arrays, hit/miss/allocate, INVAL_ALL
   walker, post-reset auto-INVAL walker, sysreg device 9.  Unit
   testbench in `hw/sim/tb_l2_cache.cpp`.

   **Delta from original plan:** the original phase 1 spec was
   "write pass-through (no dirty bit, no writeback)" — but pure
   pass-through is incorrect because L1's write-through stores
   would leave the L2 holding stale data.  Two options were on
   the table to fix that: write-through-write-no-allocate
   (byte-en update of L2 on write hit) or write-invalidate-on-hit
   (drop the L2 line on write hit).  Phase 1 shipped with
   **write-invalidate-on-hit** — ~30 fewer RTL lines than WT-WnA,
   kept the BRAM data port read-only past initialisation, and
   was functionally correct.  Phase 1.5 (below) is the deferred
   upgrade to WT-WnA.

3. **Phase 1.5 — WT-WnA upgrade.** *(design)*  Replace the
   write-invalidate-on-hit handling at the stage-1 write-hit
   site: instead of clearing the victim's valid bit, byte-en
   update the cached line's data with `s1_wdata`.  Add `s1_wdata`
   and `s1_byte_en` to the stage-0 latch.  Per-way data BRAM
   gains a write port active only on stage-1 write hits.

   **What this buys:** kernel and userland read-modify-write
   patterns (page-table walks, repeatedly-touched globals, ring-
   buffer head/tail updates, fork's pmap setup) stop self-
   evicting from L2.  Concretely, every L2 write that was
   counted as a `WRITE_HIT` under phase 1 (3.2% on Dhrystone,
   8.8% on a kernel-boot trace) was a line dropped, and most of
   those were re-read shortly after — turning each into an L2
   read miss that wouldn't exist under phase 1.5.  The post-1.5
   counter reading directly answers "how much of the L2 read
   miss rate was caused by the write policy itself".

   **What this does not buy:** write throughput.  Stores still
   pay one SDRAM round-trip per store; absorbing the writes
   themselves is phase 2.

   **Verification:** existing `tb_l2_cache` write-invalidate
   test flipped to a write-update test (write hit keeps the
   line + the new bytes); `test_perfctrs` adjusted for the new
   "write-hit doesn't invalidate" expectation; FPGA re-measure
   on Dhrystone + pbench fork/pipe trials, expecting L2 read
   miss rate to drop and L2 `WRITE_HITS` to grow.

4. **Phase 2 — write-back.** *(pending)*  Add dirty bit,
   write-allocate, eviction writeback, `FLUSH` ops.  This is the
   headline perf commit — it absorbs L1's write-through traffic
   entirely instead of letting it pass through to memory.

5. **Phase 3 — write buffer.** *(pending)*  1-deep writeback
   buffer to overlap eviction and fill.  Skip if phase 2 already
   meets perf targets.

6. **Phase 4 — perfctrs.** *(DONE)*  Four free-running 32-bit
   counters on `SYSDEV_L2_CACHE` regs 10-13: READ_HITS,
   READ_MISSES, WRITE_HITS, WRITE_MISSES.  Same layout on the L1
   caches.  HIT/MISS classification by the tag check at the
   moment of access — definitions are policy-invariant across
   all current and planned write policies, so the same counter
   names carry over through phase 1.5 and phase 2.

Each phase is an independent commit with its own test addition.

## Open Questions

- **Eviction policy under simultaneous fill+writeback:** strict
  serial in phase 2; a 1-line buffer in phase 3.  Decide whether the
  buffer is worth the gates after measuring phase 2.
- **`INVAL_ALL` vs `FLUSH_ALL` cycle budget:** O(NUM_SETS × NUM_WAYS)
  internal cycles.  At default geometry that's ~4096 cycles for an
  invalidate (no bus traffic) and many more for a flush (one
  writeback per dirty line).  The kernel polls `STATUS.busy` and
  blocks; document upper bounds in the kernel-side cache-ops doc
  when implemented.
- **Snoop port:** if a future DMA engine joins the design and the
  software-managed approach proves painful, a snoop port could be
  retrofit.  Not in scope for the initial design but the sysreg
  layout and bus shape do not preclude it.

## Future Work

- **Larger lines** (32 B) — would require sub-line tracking in L1 or
  matching L1 line size.  Defer.
- **Inclusive policy** — only justified if a coherent device cache
  is added.  Defer.
- **L1 → L2 victim cache mode** — alternative to inclusive, useful
  for direct-mapped L1.  Could be a phase-N benchmark target.

## See Also

- [`cpu-bus.md`](cpu-bus.md) — CPU-internal bus contract that
  `o_mem_*` honors and that L2 transparently extends.
- [`bus-protocol.md`](../hardware/bus-protocol.md) — Penumbra Bus
  spec; the L2's downstream port speaks this protocol's sync wrapper.
- [`sdram-controller.md`](sdram-controller.md) and
  [`sdram-optimization.md`](sdram-optimization.md) — controller-side
  optimisations.  L2 reduces frequency of trips to the controller;
  controller optimisations reduce cost of each remaining trip.  Both
  axes compose multiplicatively.
- [`mmu.md`](../system/mmu.md) — origin of the `cacheable` bit (PTE.C).
- [`sysregs.md`](../system/sysregs.md) — programmer-visible device map
  the new `SYSDEV_L2_CACHE` will be added to.
