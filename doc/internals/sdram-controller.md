# SDRAM Controller v2 — Design Plan

This document is the canonical design plan for the SDR SDRAM controller
rewrite (**v2**). The original SDR controller (formerly at
`hw/rtl/io/sdram.sv`) was replaced because of marginal hardware timing,
single-domain coupling to the system bus, and a monolithic structure
that resisted retargeting to other boards/FPGAs/SDRAM chips.  v2 has
been verified on hardware at 100 MHz CL2 (steps 1–4); v1 has been
deleted.

`doc/internals/sdram-optimization.md` remains relevant — it captures
optimization *levels* (open-row, BL=8, critical-word-first) that apply
to any controller. This document captures the *structure* we're
adopting so those optimizations can be added incrementally.

## Status

| Version | Location | State |
|---------|----------|-------|
| v1 | (deleted) | Removed once step 4 passed `_ram_check` on hardware. |
| v2 | `hw/rtl/io/sdram/` + `hw/rtl/sim/sdram_model.sv` | Steps 1–4 verified on hardware (100 MHz CL2); step 5 (phase sweep + bring-up doc) in progress; step 6 (open-row + auto-precharge-off) landed; layer-pipelining follow-on (depth-2 CDC + speculative-prefetch bus adapter) also landed. |

## Locked design decisions

These were aligned with the user before implementation began. Changing
any of them later means re-architecting, not patching.

| # | Decision | Implication |
|---|----------|-------------|
| 1 | **100 MHz CL2** SDRAM clock | Universal-safe across all ULX3S SDRAM variants (W9825G6KH, IS42S16160G, AS4C16M16SA). Comfortable margin from the minimum-frequency floor that some chips reportedly require (≥50 MHz). |
| 2 | **Bus stays single-word.** No `req_len` on the bus. | Burst awareness is a controller-internal optimization (open-row tracking + auto-precharge-off), driven by detecting sequential addresses with `req` held — *not* a bus-protocol feature. Keeps every other device on the bus simple forever. |
| 3 | **First iteration: correctness only.** | Open-row / pipelined sequential reads deferred to a later step. The FSM is *shaped* to add them without rearchitecting (parameter `OPEN_ROW_TRACKING`, etc.). |
| 4 | **Per-FPGA-family PHY.** | `sdram_phy_ecp5.sv` is shared across ULX3S and any other ECP5 board. Pin naming lives in the board top, not the PHY. Other FPGA families get their own PHY file. |
| 5 | **Single-file chip presets** in `sdram_pkg.sv`. | Each new chip / clock combination adds a `localparam` block of timing values. No per-chip include files. |

## Why we rewrote (rather than optimizing v1)

Recorded in past tense for posterity — these are the failure modes
that drove the rewrite, not problems with the current controller.

1. **Direct `assign o_sdram_clk = i_clk` clock forwarding.** No phase
   compensation, no IOB-routed clock-out. Setup/hold to the SDRAM
   was placement-dependent — any unrelated change shifted LUTs and
   timing collapsed (memory: "SDRAM marginal timing").
2. **Single clock domain at 12.5 MHz.** Locked SDRAM speed to the
   system bus speed; below the recommended floor for some chips.
3. **Monolithic file.** Mixed timing FSM, I/O cell handling, address
   mapping, refresh logic, and bus adapter. Hard to retarget.
4. **No headroom for bursts.** FSM treated every CPU word as full
   ACT→RD+AP→RECOVER. Adding open-row tracking would have required
   rewriting the FSM, not extending it.
5. **No unit test.** `_ram_check` (`hw/rom/ram_check.s`) is a system-
   level smoke test, but the controller had no isolated correctness
   verification.

## Architecture

### Two clock domains

```
                 ┌──────── system clock domain ────────┐  ┌──── SDRAM clock domain ────┐
                 │                                     │  │                            │
  CPU ─bus──► sdram_bus_adapter ──► sdram_cdc ─async──► sdram_ctrl ──phy── sdram_phy_*
                 │   (sync re/we/busy ↔                │  │   (FSM,                    │
                 │    one-word req/ack)                │  │   timing, init, refresh)   │
                 └─────────────────────────────────────┘  └────────────────────────────┘
                                                                          │
                                                                          ▼
                                                           ┌── SDRAM pin domain ──┐
                                                           │  ODDRX1F clock fwd   │
                                                           │  IOB-registered I/O  │
                                                           └──────────────────────┘
```

Why two domains? The Penumbra Bus is *async* by spec (4-phase
handshake) — the FPGA SoC's synchronous internal bus is just a
simplification. Keeping the SDRAM in its own domain:

- Lets the system clock change as long-path optimization continues
  without retuning the SDRAM PLL.
- Lets the SDRAM stay near-spec (100 MHz) regardless of what the
  bus runs at.
- Matches the canonical async shape we'll need anyway when this
  project goes to discrete 74xx logic.

The CDC overhead (~4–5 SDRAM cycles round-trip) is negligible against
T_RCD + CL + burst latencies.

### Module decomposition

```
hw/rtl/io/sdram/
├── sdram_pkg.sv              Command encodings + chip parameter presets
├── sdram_ctrl.sv             Generic SDR controller core (timing FSM, init, refresh)
├── sdram_phy_ecp5.sv         ECP5 PHY: IOB flops, ODDRX1F clock fwd, tristate DQ
├── sdram_phy_sim.sv          Plain-SystemVerilog PHY for Verilator
├── sdram_cdc.sv              Async req/ack CDC bridge (added in step 4)
└── sdram_bus_adapter.sv      Sync bus i_re/i_we/o_busy ↔ controller req/ack

hw/rtl/sim/
└── sdram_model.sv            Behavioral SDR DRAM chip model (for tests)
```

| Layer | Cares about | Changes when... |
|-------|-------------|-----------------|
| `sdram_bus_adapter` | system bus protocol | system bus changes |
| `sdram_cdc` | only carries an opaque payload | rarely (or never) |
| `sdram_ctrl` | SDR command set + timing parameters | swap to DDR/DDR3 → different controller core |
| `sdram_phy_*` | I/O cells, clock forwarding, tristate | swap FPGA family or simulation |

## Clocking on ECP5

PLL plan (proposed, easy to retune):

```
25 MHz xtal ─► EHXPLLL ─┬─► CLKOP   (system clock; today 12.5 MHz, rises with CPU optimisation) ──► SoC fabric
                        ├─► CLKOS   (SDRAM fabric clock, 100 MHz, 0°)                            ──► sdram_ctrl
                        └─► CLKOS2  (SDRAM pin clock,   100 MHz, ~270°)                          ──► ODDRX1F ──► sdram_clk pin
```

- `CLKOS` clocks controller logic and FPGA-side IOB flops.
- `CLKOS2` drives an `ODDRX1F` that forwards the SDRAM pin clock with
  a deliberate phase offset (~270°). Forwarding via ODDR places the
  toggling registers in the I/O cell, so propagation delay matches
  the data-path IOB timing — *not* a placement-dependent fabric route.
- Phase value is empirical: build for several values, sweep on hardware,
  pick the center of the working window. Don't guess; document the sweep.

In step 3 we don't yet split clocks — `CLKOS`/`CLKOS2` are not enabled
and `ODDRX1F.SCLK` ties directly to `CLKOP`. Step 4 turns on the
extra PLL outputs and re-points the PHY's `i_clk_sdram` at `CLKOS2`.

All SDRAM signals (cmd, A, BA, DQM, DQ-out, DQ-in) are placed in IOB
flops. Without IOB placement, fabric routing delay becomes part of
the I/O timing path — exactly the marginal-timing trap v1 fell into.

## Controller core (`sdram_ctrl`)

### Interface (in the SDRAM clock domain)

```
input   req_valid, req_we
input   [31:0] req_addr, [31:0] req_wdata
input   [3:0]  req_byte_en
output  req_ready

output  rsp_valid
output  [31:0] rsp_data
input   rsp_ready
```

One word in, one word out. No `req_len`. No `rsp_last`.

### FSM states

```
S_INIT_WAIT             Power-up delay (T_POWERUP cycles, 200 µs)
S_INIT_PRE              PRECHARGE ALL
S_INIT_REF1             First AUTO REFRESH
S_INIT_REF2             Second AUTO REFRESH
S_INIT_MRS              MODE REGISTER SET
S_IDLE                  Ready for requests
S_REFRESH               Periodic AUTO REFRESH (preempts IDLE)
S_ACT                   ACTIVATE issued (or skipped on row hit), waiting T_RCD
S_RW                    READ/WRITE issued (no auto-precharge — row stays open)
S_BURST                 Data beats (BL=2)
S_RECOVER               Wait T_WR after writes (reads return immediately —
                        row stays open)
S_PRECHARGE_TO_ACT      PRECHARGE issued for row conflict; will ACTIVATE
                        the new row when T_RP elapses
S_PRECHARGE_TO_REFRESH  PRECHARGE issued because refresh is due and a
                        row is still open; will issue REFRESH when T_RP
                        elapses
```

### Refresh timing guarantee

SDR SDRAM requires every row to be refreshed every `tREF` (typically
64 ms), which the controller approximates by issuing one AUTO REFRESH
every `T_REFI` controller cycles (preset at 750 cycles ≈ 7.5 µs at
100 MHz, slightly faster than the W9825's 7.8125 µs spec, providing
margin). The FSM must dispatch each AUTO REFRESH within a bounded
delay of when it becomes due, otherwise the chip's stored data
decays.

**The mechanism, in three rules:**

1. **The timer never stops** (post-init). `refresh_cnt` increments
   every cycle in *all* steady-state states — `S_IDLE`, `S_ACT`,
   `S_RW`, `S_RECOVER`, `S_REFRESH`, `S_PRECHARGE_TO_ACT`,
   `S_PRECHARGE_TO_REFRESH`. When it reaches `T_REFI` it sets
   `refresh_pending` and wraps to 0. So even during a long-running
   request or a refresh itself, the next refresh is being timed.
2. **Refresh has priority over new requests in `S_IDLE`.** The check
   for `refresh_pending` is evaluated before `i_req_valid`, so any
   request arriving while a refresh is due waits one full refresh
   sequence. Back-to-back masters cannot starve refresh.
3. **A request in flight always drains to `S_IDLE` in bounded time.**
   Every non-idle path eventually returns to `S_IDLE` through a
   single transit through `S_RW` and `S_RECOVER`. There is no loop
   that re-enters `S_ACT` or `S_RW` without passing through `S_IDLE`,
   so `refresh_pending` is checked once per request at most.

**Worst-case latency from due-to-issued.** The longest path between
`refresh_pending` becoming `1` and AUTO REFRESH actually issuing is:

```
in-flight request drain  ≤ T_RP + T_RCD + max(CL+1, 1+T_WR) cycles
+ S_IDLE check                                          1 cycle
+ S_PRECHARGE_TO_REFRESH (close any open row)           T_RP cycles
─────────────────────────────────────────────────────────────────
total                                                  ≈ 2·T_RP + T_RCD + max(CL+1, 1+T_WR) + 1
```

With the W9825 preset (T_RP=2, T_RCD=2, T_WR=2, CL=2): worst case
≈ 2·2 + 2 + 3 + 1 = 10 controller cycles ≈ 100 ns at 100 MHz. Out of
the `tREFI` slack (~7.8 µs between refresh deadlines), that's ~1%.
Even if a write to one row immediately conflicts with a write to
another in the same bank (the longest single-request path), and
refresh becomes due the instant we leave `S_IDLE`, the controller
issues AUTO REFRESH about two orders of magnitude inside the chip's
deadline.

**Open-row interaction.** Because `S_RW` no longer auto-precharges,
a row may still be open when refresh becomes due. AUTO REFRESH
requires all banks idle, so the FSM detects this in `S_IDLE` (or in
the path returning to it) and routes through `S_PRECHARGE_TO_REFRESH`
which issues PRECHARGE-ALL, waits `T_RP`, then issues REFRESH. This
adds the trailing `T_RP` term in the worst-case bound above. With
`open_valid = 0` (no row open), the FSM goes straight to `S_REFRESH`
and skips this step.

### Future-proofing parameters (shaped holes, not implementations)

| Parameter | Today's value | Status / future use |
|-----------|---------------|---------------------|
| `BURST_LEN` | `2` | BL=8 line buffer (step 7) |
| `AUTO_PRECHARGE` | (off) | Removed — open-row tracking always active |
| `OPEN_ROW_TRACKING` | (single-row, on) | Per-bank tracking would extend `open_valid`/`open_row` to arrays of `2^BA_BITS` |

## CDC bridge (`sdram_cdc`)

Depth-2 async FIFO with 2-bit Gray-coded read/write pointers crossing
between domains via 2-FF synchronizers. Up to 2 outstanding requests at
any time; order-preserving so responses arrive in issue order. Per-slot
wide payloads (we/addr/wdata/byte_en sys→sd, rsp_data sd→sys) are
written *before* the pointer advance, so the receiving side reads
already-stable data — the standard data-before-valid discipline, just
with the "valid" carried by the synchronized Gray pointer instead of a
hand-rolled toggle.

Sized for the speculative-prefetch bus adapter, which queues a
real + speculation pair on every accepted read; depth-2 lets both fly
concurrently without serializing them through a single CDC slot.
Latency per transaction is unchanged from the depth-1 predecessor —
deepening removes the serial dependency between successive
transactions, not the per-transaction round-trip.

## PHY

### `sdram_phy_ecp5` (real hardware)

- Every command/addr/BA/DQM/DQ-out/DQ-in signal in IOB flops.
- `sdram_clk` driven via `ODDRX1F` clocked from CLKOS2.
- DQ tristate via `dq_oe` with explicit IOB flops (Yosys' tristate
  inference is unreliable — see project memory on Yosys tri-state
  limitations).
- Pin port widths parameterized; pin *names* belong to the board top.

### `sdram_phy_sim` (Verilator)

- Same module shape, no FPGA primitives.
- DQ tristate via plain mux.
- Lets `sdram_ctrl` be unit-tested without ECP5 stubs.

## Behavioral SDRAM model (`hw/rtl/sim/sdram_model.sv`)

A faithful behavioral SDR DRAM chip — accepts cmd/addr, runs internal
per-bank state, drives DQ on reads with proper CAS latency, honors
DQM masking. Used by:

1. Controller unit tests (drive controller, observe via model).
2. `machine_sim` for end-to-end SoC simulation (replaces `simple_mem`
   for SDRAM-mapped RAM).

Reusable for any controller variant we build later (DDR successor,
etc., would need their own models).

## Bus adapter (`sdram_bus_adapter`)

Maps the existing internal sync bus (`i_re`/`i_we`/`o_busy`) onto the
controller's `req`/`rsp` interface. Single-word transactions on the bus
side, same external contract as `simple_mem`/`fpga_ram`/`boot_rom` —
drop-in replacement at the SDRAM region.

Internally pipelined for cache fills:

- **Speculative prefetch.** On every accepted read the adapter pushes
  a second request for `addr + 4` to the CDC. The depth-2 CDC carries
  the real fetch and the speculation in flight simultaneously. When
  the cache later asks for that next address (the typical case during
  a 4-word line fill) the speculation buffer satisfies it without a
  fresh CDC round-trip. Mispredicts abandon the in-flight spec — its
  response is discarded when it arrives — and push the new real
  request normally. Reads only; writes don't speculate.
- **1-bit tag FIFO (depth 2).** Each push to the CDC is paired with a
  tag (0 = real → cache, 1 = spec → buffer/discard). Each `rsp_valid`
  pulse pops the head tag and routes `rsp_data` accordingly. Order is
  preserved end-to-end because the CDC's SD side processes requests
  serially in push order.
- **Write-accept release.** Writes don't enter `WAIT_RSP`: once the
  CDC accepts a write, ordering is locked (FIFO + serial controller),
  so the bus drops `o_busy` immediately rather than waiting for an
  rsp that the controller doesn't generate for writes. The CDC also
  gates `rsp_valid` pulses to reads, so write completion can't
  spuriously feed the response handler.

The adapter still presents a single-word contract upward, so the cache
and other masters see no protocol change — burst-fill speedup comes
entirely from the spec-buffer hits the cache observes as faster
single-word completions.

## Validation strategy

Three-level test pyramid:

1. **Controller unit test** (Verilator, sim PHY + behavioral model).
   Drive `sdram_ctrl` with sequenced requests, plug behavioral model
   on the PHY side. Cover: init sequence, read, write, byte enables,
   refresh preemption, write→read→write across banks. *This is what
   v1 lacks.*
2. **Full SoC sim** (`machine_sim`). Run the existing `_ram_check`
   ROM diagnostic and normal ROM/kernel boot. The diagnostic in
   `hw/rom/ram_check.s` is exactly the right battery for adapter
   bugs (round-trip, sequential, sub-word integrity).
3. **On hardware** (ULX3S). Build, run `_ram_check` from cold boot,
   then NetBSD boot. Add a `make fpga PHASE=N` sweep build for the
   bring-up procedure.

## Phased rollout

| Step | Deliverable | Verification | State |
|------|-------------|--------------|-------|
| 1 | `sdram_pkg` + behavioral model + `sdram_ctrl` skeleton + sim PHY | Compiles + lints clean; controller unit tests pass | ✅ landed |
| 2 | Wire into `machine_sim` via sim PHY + bus adapter | `_ram_check` passes in Verilator | ✅ landed |
| 3 | `sdram_phy_ecp5` (IOB flops + ODDRX1F); single-domain at the system clock | ULX3S boots, `_ram_check` passes | ✅ landed (subsumed by step 4 on hardware) |
| 4 | `sdram_cdc` + dual-domain @ 100 MHz CL2 | `_ram_check` + NetBSD boot | ✅ landed (boots + `_ram_check` passes; PHASE_DEG=270° baseline) |
| 5 | Phase-shift sweep build target + bring-up doc | Documented working window | ⚙ in progress |
| 6 | Open-row tracking + auto-precharge-off (Level 1 of `sdram-optimization.md`) | `tb_sdram_test` cold-vs-hit cycle counts; `_ram_check` + membench unchanged on hardware | ✅ landed |
| 7 | Layer-pipelining: depth-2 CDC FIFO + speculative `addr+4` prefetch in bus adapter | `tb_sdram_cdc` + `make benchmark-rtl` membench miss-LDW improvement | ✅ landed |
| 8 | (Later) BL=8 line buffer + critical-word-first (Levels 2–3 of `sdram-optimization.md`) | Re-run validation, measure Dhrystone | deferred |

### Step 4 wiring notes

The CDC bridge (`sdram_cdc.sv`) is a depth-2 async FIFO with 2-bit
Gray-coded read/write pointers crossing between domains via 2-FF
synchronizers (step 7 deepened this from the original depth-1 toggle
handshake; the boundary shape is the same):

- **Sys → SDRAM (request):** per-slot wide payload (we, addr, wdata,
  byte_en) is written into `slot[wptr_bin[0]]` *before* the sys side
  bumps `wptr_bin`. The Gray-coded pointer crosses to the SDRAM side
  via the 2-FF synchronizer; once the SDRAM side observes empty=0,
  it samples the slot — by which time the payload has been stable
  for many cycles (data-before-valid).
- **SDRAM → sys (done/rsp):** the SDRAM side writes `rsp_data` into
  `slot[rptr_bin[0]]` and bumps `rptr_bin`. The sys side's
  synchronized Gray pointer advances ~3 sys cycles later; on each
  step of advance the sys side pulses `o_rsp_valid` for the retired
  slot, in order.

CDC latency: ~3 SDRAM cycles in the request direction, ~3 sys cycles
back, per transaction.  Negligible against `T_RCD + CL + T_RP + 2
burst beats` at 100 MHz.  Depth 2 lets the speculative-prefetch bus
adapter keep both a real fetch and a speculation in flight without
serializing through a single CDC slot — see "Bus adapter" above.

The PLL at the board top now exports three outputs from one 600 MHz
VCO: CLKOP @ 12.5 MHz (system bus), CLKOS @ 100 MHz / 0° (SDRAM
controller fabric and IOB flops), CLKOS2 @ 100 MHz / 270° (forwarded
out the SDRAM clock pin via ODDRX1F).  CPHASE/FPHASE convention:
**0° = CPHASE = (DIV − 1), FPHASE = 0**; each FPHASE step is 1/8 VCO
cycle; phase shift φ from 0° subtracts (φ × DIV / 360°) VCO cycles.
For DIV = 6 and φ = 270°: shift = 4.5 VCO cycles → CPHASE = 0,
FPHASE = 4.

The 270° starting point was an educated guess that booted on the
ULX3S used during step 4 bring-up; the step-5 sweep below pins down
the centred working window for posterity and for any future board
that ships with a different SDRAM variant.  In sim (`sdram_sim.sv`)
we tie both CDC clocks to the same testbench clock; the CDC
FSM/handshake is exercised end-to-end but actual two-domain skew
only happens on hardware.

### Step-5 phase sweep

The PLL output that drives the SDRAM clock pin (CLKOS2) is phase-
shifted relative to the controller's IOB-flop clock (CLKOS) so the
SDRAM samples our drives near the centre of the data window.  The
shift is empirical: routing delays through the FPGA, board traces,
and the SDRAM input flops vary across boards, SDRAM variants, and
even ULX3S revisions.  To pin down the centre rather than guess:

#### Build target

```sh
make fpga PHASE_DEG=N TOP=ulx3s_top    # build only
make flash PHASE_DEG=N TOP=ulx3s_top   # build + flash
```

`PHASE_DEG` accepts the 8 cardinal points: **0, 45, 90, 135, 180,
225, 270, 315**.  The default is **270°**.  An invalid value silently
falls back to (CPHASE=0, FPHASE=0); always use one of the listed
values or extend the lookup table in `ulx3s_top.sv` first.

The phase value is baked into the bitstream — re-flash after every
phase change.  The Makefile invalidates downstream artefacts via a
`build/.phase-N` stamp so the right files rebuild automatically; you
do *not* need `make clean` between sweep iterations.

#### Procedure

For each of the 8 phase values:

1. `make flash PHASE_DEG=N TOP=ulx3s_top`
2. Power-cycle (or press btn[1]) and capture the ROM monitor output
   over `/dev/ttyUSB0` at 115200 8N1.
3. Record the `_ram_check` result and the detected RAM size.
4. (Optional) `boot sd:0,0/PENBOOT.ELF` and confirm the kernel
   reaches single-user shell — passes only if the cache-on /
   burst-fill path works, which is a stronger gate than ROM
   `_ram_check` alone.

The working phases form a contiguous arc.  Pick its **centre**, not
an edge — edge-of-window settings are one PVT corner away from
breaking on a hot day or after a netlist reshuffle.

#### Per-board results

| Board / SDRAM variant            | Working phases  | Chosen phase | Date       |
|----------------------------------|-----------------|--------------|------------|
| ULX3S v3.1.8 / Winbond W9825G6KH | TBD (step 5)    | 270° (provisional) | 2026-04-30 |

Add a new row when sweeping a new board/variant.  When changing the
chosen phase, also change the default in `ulx3s_top.sv`'s
`` `define SDRAM_PHASE_DEG `` so unflagged builds match.

#### What to do if every phase fails

Every cardinal phase failing means the timing problem is *not* the
sample-window centre — it's somewhere else.  Look for:

- **Wrong CL parameter** vs. what the chip actually supports
  (e.g., a -7 grade chip might need CL=3 at 100 MHz).
- **PHY IOB flops not actually placed in IOB cells** — check the
  Yosys / nextpnr packing report; the `(* iob = "true", keep *)`
  attribute is informational, not enforced.
- **VCO out of range** — re-derive `CLKOP_DIV / CLKOS_DIV /
  CLKOS2_DIV` if you change the system clock.
- **Drive strength / slew-rate** on the SDRAM pins — the LPF can
  pin LVCMOS33 with explicit drive; weak drive at 100 MHz is a
  classic source of intermittent failures.

### Step 3 controller wiring notes

The ECP5 PHY adds one IOB flop on every output and one IOB flop on
the DQ input.  The controller absorbs that pipeline through two new
parameters, `PHY_OUT_LATENCY` and `PHY_IN_LATENCY`, which are summed
into the read-data sample countdown:

```
cl_cnt <= 4'(PHY_OUT_LATENCY + CAS_LATENCY + PHY_IN_LATENCY)
```

Both default to `0`, so the sim PHY (combinational) keeps the
existing `cl_cnt = CAS_LATENCY` behaviour without code changes.
The ECP5 PHY sets both to `1` at the instantiation site in
`ulx3s_top`.  Step 4's CDC bridge will add another two terms to the
same sum (one synchronizer chain in each direction); no further
controller surgery should be needed.

Steps 1–2 are pure simulation. Steps 3–4 are the architectural payoff.
Steps 5–6 are durability + perf.

## Burst optimization (deferred to step 6)

The user's bus decision: *no `req_len` on the bus.* Burst-ness is
detected internally by the controller from sequential addresses with
`req` held — exactly the protocol shape Penumbra Bus already specifies
in `doc/system/bus.md` § Burst Transfers.

Step 6 work, all internal to the controller:

1. Add 4-bank state: `{row, valid}` per bank (4 × 14 bits).
2. On request at `S_IDLE`: if `(addr.bank, addr.row)` matches an open
   row, skip `S_ACT` and jump straight to `S_RW`.
3. Issue READ/WRITE *without* auto-precharge (A10=0).
4. Precharge only on row miss (the right bank) or refresh.
5. Precharge before refresh (PRECHARGE ALL).

No bus protocol changes. No client-side changes. The cache's existing
sequential-fill loop becomes pipelined automatically.

### Measuring step 6

`benchmark/membench/` is the canonical before/after probe.  The
**cached LDW MB/s** number is the most relevant: a 64 KiB working
set with sequential access produces one cache-line miss every four
LDWs, and every miss is exactly the "next sequential cache line"
case that step 6 targets (same bank, same row, ascending column).
After step 6, those misses should skip `S_ACT` and shave roughly
`T_RCD + T_RP` cycles per line — a 2-3× speedup at the SDRAM-side
of the path.  Whether the system-level number moves that much
depends on how CPU-bound the loop is at the current clock; the
**uncached LDW ns/op** number isolates the SDRAM round-trip from
loop overhead and should track the SDRAM-side improvement directly.
Run `make benchmark` (or flash and load `MEMBENCH.ELF` from SD)
before and after the step-6 commit and record both numbers in the
commit message for posterity.

## Design tradeoffs considered

### Async clock vs sync multiple

**Chosen: async (two clock domains).** The system bus clock will
change as long-path optimization continues. A fixed-multiple structure
means re-tuning the SDRAM PLL on every system-clock change, and may
push us out of "near-spec" territory for the SDRAM. Async pays a
small one-time cost (handshake CDC, ~4–5 cycles) for permanent
decoupling, and matches the canonical bus shape we'll need anyway
for the discrete-logic build.

### `req_len` on bus vs detect-by-sequential-addresses

**Chosen: detect-by-sequential.** A bus with `req_len` forces every
slave device — UART, SPI, GPIO, future expansion cards — to handle
multi-beat transfers, even though only the SDRAM benefits. The
detect-by-sequential approach matches Wishbone classic and the
existing Penumbra Bus spec, keeping every other device dead simple.
The cost (only the SDRAM sees the burst hint) is exactly where it
should be: in the device that benefits.

### Per-FPGA PHY vs per-board PHY

**Chosen: per-FPGA-family.** Most I/O cell work is family-specific
(ODDRX1F is ECP5; Xilinx uses ODDR; iCE40 has neither). Boards on
the same family share the PHY and just differ in chip parameters
and pin names (which live in the board top). A new ECP5 board with
different SDRAM gets a new chip preset, not a new PHY.

### Where chip parameters live

**Chosen: single `sdram_pkg.sv` with named `localparam` presets.**
Each new chip/clock combination is a self-contained block of timing
values. Verbose at the instantiation site (one `.T_RCD(...)` line per
parameter), but the verbosity lives in the *board top* — a file that
gets touched rarely and benefits from being explicit.

## References

- W9825G6KH datasheet (default ULX3S SDRAM variant)
- JEDEC JESD21-C, section 3.11 (SDR SDRAM)
- `doc/system/bus.md` § Burst Transfers
- `doc/hardware/bus-protocol.md` § Reset Timing
- `doc/internals/sdram-optimization.md` — optimization levels
- Memory: `feedback_*.md` SDRAM marginal timing notes
