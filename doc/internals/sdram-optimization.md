# SDRAM Controller Optimization Notes

The current SDRAM controller (`hw/rtl/io/sdram/sdram_ctrl.sv`) uses a
simple close-after-access policy: every 32-bit CPU word access does a
full ACTIVATE → READ/WRITE (BL=2) with auto-precharge → recovery
cycle.  This is correct and easy to debug, but wastes bandwidth on
sequential accesses like cache line fills.  The optimisations below
plug into the existing FSM via the `OPEN_ROW_TRACKING` and
`AUTO_PRECHARGE` parameter holes (see the design plan in
`sdram-controller.md` for the future-proofing scaffolding).

## Current Performance (BL=2, auto-precharge, 100 MHz)

| Operation | Cycles | Breakdown |
|-----------|--------|-----------|
| Single read | 6 | ACT(1) + READ+AP(1) + CL(2) + tRP(1) + recovery(1) |
| Single write | 5 | ACT(1) + WRITE+AP(1) + data(1) + tWR(2) |
| 4-word cache line fill | 24 | 4 × single read (same row opened/closed 4 times) |

## Optimization Levels

### Level 1: Open-Row Policy

Track the currently open row per bank (4 banks × {row, valid} state).
If a new access hits the same bank+row, skip ACTIVATE and issue
READ/WRITE directly.  Only PRECHARGE on row miss or refresh.

**Cache line fill cost:** ~19 cycles
- Word 0: ACT + READ + CL + 2 beats = 5 cycles (row miss → open row)
- Word 1-3: READ + CL + 2 beats = 4 cycles each (row hit)
- Final PRECHARGE deferred until row change or refresh

**Complexity:** Low.  4 × (13-bit row register + valid bit).
Must PRECHARGE before refresh and on row miss.
Feasible in 74xx discrete (4 × 14-bit register + comparator).

**Bus interface:** Unchanged — same word-at-a-time protocol.

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
`doc/system/bus.md` § Burst Transfers).  During a cache line
fill, the master holds `req` asserted and drives sequential addresses:

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

1. **Ship current design** (BL=2, auto-precharge) — correct, simple
2. **Level 1 (open-row)** — low effort, meaningful speedup for
   sequential access, no bus interface changes
3. **Level 2 (BL=8 line buffer)** — best bang-for-buck, matches
   cache line size exactly, halves line fill time
4. **Level 3** — only if profiling shows memory latency is the
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

At 12.5 MHz, the CPU is ~2 instructions/cycle (fetch + execute
pipeline), so SDRAM latency of 6 cycles ≈ 3 instructions.  This
is tolerable for single accesses but painful for line fills.

At higher clocks (25-50 MHz), the SDRAM timing parameters grow
(T_RCD=2-3, T_RP=2-3) but the ratio of useful beats to overhead
improves with longer bursts.  BL=8 becomes even more attractive
at higher speeds.
