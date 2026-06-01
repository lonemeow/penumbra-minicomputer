# SDRAM Controller Optimization Notes

> **Applies to:** all generations · shared hardware.

The SDRAM controller (`hw/rtl/io/sdram/sdram_ctrl.sv`) advances through
a sequence of optimisation levels.  Level 1 (open-row tracking, no
auto-precharge) is live; Levels 2 and 3 are deferred design plans.
This file is the catalog of those levels — what each one buys, what it
costs, and how it composes with the others.

For controller-internal mechanics (FSM states, refresh-timing
guarantee, parameter table) see `sdram-controller.md`.

## Baseline (BL=2, auto-precharge, 100 MHz) — Historical

This was the v2 controller's first-iteration policy: every 32-bit CPU
word did a full ACTIVATE → READ/WRITE+AP → recovery cycle.  It is
preserved here as the reference point each level below is measured
against.

| Operation | Cycles | Breakdown |
|-----------|--------|-----------|
| Single read | 6 | ACT(1) + READ+AP(1) + CL(2) + tRP(1) + recovery(1) |
| Single write | 5 | ACT(1) + WRITE+AP(1) + data(1) + tWR(2) |
| 4-word cache line fill | 24 | 4 × single read (same row opened/closed 4 times) |

**Current controller** runs Level 1 (below): the row stays open across
accesses, so the typical hit costs less than the table above and a
4-word fill avoids 3 × (PRECHARGE + ACTIVATE).

## Optimization Levels

### Level 1: Open-Row Policy ✅ landed

Track the currently open row.  If a new access hits the same bank+row,
skip ACTIVATE and issue READ/WRITE directly.  PRECHARGE only on row
conflict (different bank or different row) or before AUTO REFRESH.

**Cache line fill cost:** ~19 cycles
- Word 0: ACT + READ + CL + 2 beats = 5 cycles (row miss → open row)
- Word 1-3: READ + CL + 2 beats = 4 cycles each (row hit)
- Final PRECHARGE deferred until row change or refresh

**Implementation note.** The landed version is single-row tracking
(one global `open_valid` + `open_bank` + `open_row` register), not
per-bank.  Single-row catches the dominant case — sequential cache
line fills sit on the same row — at a fraction of the state cost.
Per-bank tracking (4 × {row,valid}) is a localized extension if a
workload appears that benefits from it.

**Complexity:** Low.  One 13-bit row register + 2-bit bank register
+ valid bit, plus two new FSM states (`S_PRECHARGE_TO_ACT` for row
conflict and `S_PRECHARGE_TO_REFRESH` for the close-before-refresh
path).  Feasible in 74xx discrete.

**Bus interface:** Unchanged — same word-at-a-time protocol.

A separate, complementary optimisation lives one layer up in the
SDRAM bus adapter: speculative `addr+4` prefetch carried by a depth-2
CDC FIFO.  That hides the bus-traversal latency between the cache and
SDRAM rather than the SDRAM-internal command overhead targeted here.
See `sdram-controller.md` § "Bus adapter" and § "CDC bridge".

### Level 2: BL=8 Line Buffer

Change SDRAM burst length from 2 to 8.  Each SDRAM burst reads
8 × 16-bit = 4 × 32-bit words — exactly one cache line.

The SDRAM controller buffers the full 4-word line internally.
First read triggers the burst; subsequent reads to the same line
are served from the buffer with 1-cycle latency.

**Cache line fill cost:** ~10 cycles
- ACT(1) + READ+AP(1) + CL(2) + 8 beats(8) + tRP(1) = ~13 SDRAM cycles
- But only 1 bus transaction visible to cache (buffer serves rest)

**Complexity:** Medium.  128-bit (4 × 32) line buffer + tag + valid.
Need to match incoming addresses against buffer tag.
Essentially a 1-entry read cache inside the SDRAM controller.

**Bus interface options:**
- (a) Keep word-at-a-time bus, buffer internally (simplest)
- (b) Add burst signal from cache (cache says "fill 4 words")
- (c) Widen internal bus to 128-bit for single-cycle line transfer

Option (a) is simplest: controller checks if request hits the line
buffer before initiating a new SDRAM burst.

### Level 3: Open-Row + BL=8 + Critical Word First

Combine open-row tracking with BL=8 bursts.  On a miss, start the
burst from the requested word's column (critical word first), wrap
around to fill the rest of the line.  Keep the row open for the
next potential miss.

**Cache line fill cost:** ~10 cycles total, ~5 cycles to first word

**Complexity:** High.  Needs wrapping burst logic, row tracking,
and critical-word-first reordering.  Probably not worth it until
we're running at higher clock speeds where latency matters more.

## Bus Protocol Support

The Penumbra Bus already supports burst transfers (see
`doc/hardware/bus-protocol.md` § Burst Transfers — Cache Line Fill).
During a cache line fill, the master holds `req` asserted and drives
sequential addresses:

```
req:       ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\___
addr:      ---[A+0 ][A+4 ][A+8 ][A+C ]-------
ack:       ______/‾‾\/‾‾‾\/‾‾‾\/‾‾‾\________
data:      ------[W0 ][W1  ][W2  ][W3  ]-----
```

The bus spec explicitly notes that slave devices may detect sequential
addresses and optimize internally.  No protocol changes needed — the
SDRAM controller just needs to compare incoming row+bank against the
currently open row before deciding whether to ACTIVATE.

On the internal synchronous bus (FPGA fabric), the equivalent is:
the cache keeps `i_re` asserted and increments the address after each
`!o_busy` cycle.  The SDRAM controller sees the next request arrive
immediately after the previous one completes.

## Recommended Path

1. ✅ **Ship baseline** (BL=2, auto-precharge) — correct, simple
2. ✅ **Level 1 (open-row)** — low effort, meaningful speedup for
   sequential access, no bus interface changes
3. ✅ **Layer-pipelining** (depth-2 CDC + speculative-prefetch bus
   adapter) — orthogonal to the levels here, hides bus-traversal
   latency between cache and SDRAM
4. **Level 2 (BL=8 line buffer)** — best bang-for-buck for the
   remaining SDRAM-internal cost, matches cache line size exactly,
   halves line fill time
5. **Level 3** — only if profiling shows memory latency is still the
   bottleneck after higher clock speeds

## SDRAM Burst Length and Cache Line Size Relationship

| Cache LINE_WORDS | Line bytes | SDRAM BL (16-bit) | Beats | Notes |
|-------------------|------------|---------------------|-------|-------|
| 2 | 8 | 4 | 4 | Small line, more tag overhead |
| 4 | 16 | 8 | 8 | Current cache config, natural BL=8 match |
| 8 | 32 | full page | 16 | Would need burst termination |

The current LINE_WORDS=4 and 16-bit SDRAM are a natural match for
BL=8.  No wasted beats, no burst termination logic needed.

## Clock Speed Considerations

At the current ULX3S operating point (25 MHz CPU, 100 MHz SDRAM)
the CPU is single-issue microcoded — there is no fetch/execute
overlap, so a stall on a miss is a stall on the actual machine, not
a stall absorbed by a pipeline.  The dominant miss-side cost is the
L1↔SDRAM round-trip through the CDC bridge and bus adapter, not
the in-SDRAM command overhead (per the layer-cost split: roughly
85% adapter+CDC vs 15% controller-internal).  That is what motivates
the already-shipped layer-pipelining work (speculative `addr+4` +
depth-2 CDC, see § Recommended Path above) and what makes Level 2
(BL=8 line buffer) the natural next optimisation: it shortens the
SDRAM-side of the round-trip while the bus-side pipeline keeps the
CPU from stalling between word completions.

L2 (64 KiB unified, write-invalidate-on-hit today) further cuts the
average miss cost by catching the working set entirely within the
system clock domain, never crossing the CDC bridge.  For specific
CPI / DMIPS numbers on the current bitstream, run `make benchmark`
on hardware rather than quoting figures from this doc — measured
numbers move as cache / compiler / kernel changes land, and any
specific number here is guaranteed to be stale before long.

At higher CPU clocks (50 MHz+) the SDRAM-side timing budget in ns
stays roughly constant; the same overhead consumes a larger fraction
of useful work, making bursts proportionally more valuable.
