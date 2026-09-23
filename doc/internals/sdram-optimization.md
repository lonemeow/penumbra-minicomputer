# SDRAM Controller Optimization Notes

> **Applies to:** all generations · shared hardware.

The SDRAM controller (`hw/rtl/io/sdram/sdram_ctrl.sv`) advances through
a sequence of optimisation levels.  Level 1 (open-row tracking, no
auto-precharge) is live, Level 2 is a deferred design plan, and
Level 3 is declined.
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

The line crosses the CDC as one 128-bit transfer into a line buffer
in the bus adapter.  The read that triggers the burst completes when
the line lands; the line's other reads are served from the buffer at
the bus's pace — two cycles each, the presentation and a registered
`ack` ([sync burst](../hardware/bus-protocol.md#sync-burst)).

**Cache line fill cost:**
- SDRAM side, open row: READ + CL(2) + 8 beats = 10 SDRAM cycles,
  six more than a BL=2 read; a row miss adds ACTIVATE + tRCD as
  before.
- One CDC crossing per line, where the speculative adapter makes a
  round trip for word 0 and again for each of words 2 and 3.
- Request to last word, in CPU cycles (estimated from the measured
  chain A in
  [`penumbra3/memory-performance.md`](penumbra3/memory-performance.md#sdram-side-whole-line-fetch)):
  about 16 at a 4:1 clock ratio and 19 at 2:1, against 28 and 36
  for the speculative adapter.  Six of those cycles are words 1–3
  crossing the bus after the line lands, a floor no SDRAM-side
  change moves.

**Complexity:** Medium.  128-bit (4 × 32) line buffer + tag + valid,
matched against incoming addresses — essentially a 1-entry read cache
in the adapter.  It replaces the speculative prefetch: with one master
and blocking reads, no read is in flight that the master has not
asked for, so any write simply invalidates the buffer and nothing
needs abandon tracking.  Writes stay single-word: the mode register's
write-burst bit (A9) makes writes single-location while reads burst,
so a 32-bit write is two WRITE commands on consecutive clocks.

**Bus interface:** unchanged.  The bus carries no burst length or
line shape, so the line still crosses it as four word accesses; a
burst signal from the cache or a 128-bit bus would change the bus
contract.

### Level 3: Critical Word First — declined

Starting the burst at the requested word and wrapping buys nothing
here: every cache fills atomically and releases the CPU only when the
whole line is in, so the order the words arrive in never shows (see
[the gen2 fill path](penumbra2/memory-interface.md#the-fill-path)).
Recorded with the other partial-line delivery ideas under Ruled out
in `doc/TODO/hw-core.md`.

## Bus Protocol Support

The Penumbra Bus already supports burst transfers (see
[Sync burst](../hardware/bus-protocol.md#sync-burst)).  During a
cache line fill, the master holds `re` asserted and drives sequential
addresses.  With a slave that has every word ready, the four words
take eight cycles — the registered `ack` never comes in a word's
presentation cycle:

| Cycle | `re` | `addr` | `ack` | `rdata` |
|------:|:----:|:------:|:-----:|:-------:|
| 0 | 1 | A+0 | 0 | — |
| 1 | 1 | A+0 | 1 | W0 |
| 2 | 1 | A+4 | 0 | — |
| 3 | 1 | A+4 | 1 | W1 |
| 4 | 1 | A+8 | 0 | — |
| 5 | 1 | A+8 | 1 | W2 |
| 6 | 1 | A+C | 0 | — |
| 7 | 1 | A+C | 1 | W3 |

The bus spec explicitly notes that slave devices may detect sequential
addresses and optimize internally.  No protocol changes needed — the
SDRAM controller just needs to compare incoming row+bank against the
currently open row before deciding whether to ACTIVATE.

The cache keeps `re` asserted and advances the address in the cycle
after each `ack`, so the SDRAM side sees the next request arrive
immediately after the previous one completes.

## Recommended Path

1. ✅ **Ship baseline** (BL=2, auto-precharge) — correct, simple
2. ✅ **Level 1 (open-row)** — low effort, meaningful speedup for
   sequential access, no bus interface changes
3. ✅ **Layer-pipelining** (depth-2 CDC + speculative-prefetch bus
   adapter) — orthogonal to the levels here, hides bus-traversal
   latency between cache and SDRAM
4. **Level 2 (BL=8 line buffer)** — one CDC crossing per line instead
   of three round trips, matches cache line size exactly, and roughly
   halves the SDRAM side of a line fill (~28–36 → ~16–19 cycles)
5. **Level 3** — declined; fills are atomic, so word order never shows

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
(BL=8 line buffer) the natural next optimisation: it replaces two of
a line fill's three adapter-and-CDC round trips with six more beats
of one burst.

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
