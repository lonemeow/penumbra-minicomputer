# SDRAM Controller v2 — Design Plan

This document is the canonical design plan for the SDR SDRAM controller
rewrite (**v2**). The original `hw/rtl/io/sdram.sv` (v1) is being
replaced because of marginal hardware timing, single-domain coupling
to the system bus, and a monolithic structure that resisted retargeting
to other boards/FPGAs/SDRAM chips.

`doc/internals/sdram-optimization.md` remains relevant — it captures
optimization *levels* (open-row, BL=8, critical-word-first) that apply
to any controller. This document captures the *structure* we're
adopting so those optimizations can be added incrementally.

## Status

| Version | Location | State |
|---------|----------|-------|
| v1 | `hw/rtl/io/sdram.sv` | Will be removed once v2 is on hardware |
| v2 | `hw/rtl/io/sdram/` (new directory) + `hw/rtl/sim/sdram_model.sv` | Under development |

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

## Why we're rewriting (not optimizing v1)

1. **Direct `assign o_sdram_clk = i_clk` clock forwarding.** No phase
   compensation, no IOB-routed clock-out. Setup/hold to the SDRAM is
   placement-dependent. Any unrelated change shifts LUTs and timing
   collapses (memory: "SDRAM marginal timing", file
   `hw/rtl/io/sdram.sv` lines 142–143).
2. **Single clock domain at 12.5 MHz.** Locks SDRAM speed to the
   system bus speed; below the recommended floor for some chips.
3. **Monolithic file.** Mixes timing FSM, I/O cell handling, address
   mapping, refresh logic, and bus adapter. Hard to retarget.
4. **No headroom for bursts.** FSM treats every CPU word as full
   ACT→RD+AP→RECOVER. Adding open-row tracking requires rewriting
   the FSM, not extending it.
5. **No unit test.** `_ram_check` (`hw/rom/ram_check.s`) is a system-
   level smoke test, but the controller has no isolated correctness
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
25 MHz xtal ─► EHXPLLL ─┬─► CLKOP   (system clock, e.g. 25 MHz)             ──► SoC fabric
                        ├─► CLKOS   (SDRAM fabric clock, 100 MHz, 0°)       ──► sdram_ctrl
                        └─► CLKOS2  (SDRAM pin clock,   100 MHz, ~270°)     ──► ODDRX1F ──► sdram_clk pin
```

- `CLKOS` clocks controller logic and FPGA-side IOB flops.
- `CLKOS2` drives an `ODDRX1F` that forwards the SDRAM pin clock with
  a deliberate phase offset (~270°). Forwarding via ODDR places the
  toggling registers in the I/O cell, so propagation delay matches
  the data-path IOB timing — *not* a placement-dependent fabric route.
- Phase value is empirical: build for several values, sweep on hardware,
  pick the center of the working window. Don't guess; document the sweep.

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
S_INIT_WAIT      Power-up delay (T_POWERUP cycles, 200 µs)
S_INIT_PRE       PRECHARGE ALL
S_INIT_REF1      First AUTO REFRESH
S_INIT_REF2      Second AUTO REFRESH
S_INIT_MRS       MODE REGISTER SET
S_IDLE           Ready for requests
S_REFRESH        Periodic AUTO REFRESH (preempts IDLE)
S_ACT            ACTIVATE issued, waiting T_RCD
S_RW             READ/WRITE issued (auto-precharge today)
S_BURST          Data beats (BL=2)
S_RECOVER        Wait T_RP / T_WR before next command
```

### Future-proofing parameters (shaped holes, not implementations)

| Parameter | v1 value | Future use |
|-----------|----------|------------|
| `BURST_LEN` | `2` | BL=8 line buffer (step 6+) |
| `AUTO_PRECHARGE` | `1` (close on access) | Set `0` once open-row tracking is in |
| `OPEN_ROW_TRACKING` | `0` | Set `1` to add 4×{row,valid} state and same-row fast path |

These thread through counters and decisions but each is hard-wired to
its v1 value. Turning them on later is a localized change, not a
rewrite.

## CDC bridge (`sdram_cdc`)

Simple 4-phase handshake (one outstanding request). Two-flop synchronizers
on each direction's `valid`. Payload fields held quasi-statically and
sampled only on the synchronized `valid`.

Async FIFO is overkill until multi-outstanding requests exist (step 6+),
at which point we revisit. The boundary stays the same shape — just
deeper.

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
controller's `req`/`rsp` interface. Single-word transactions; same
external contract as `simple_mem`/`fpga_ram`/`boot_rom`. Drop-in
replacement for the v1 controller in `ulx3s_top` and `machine_sim`.

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

| Step | Deliverable | Verification |
|------|-------------|--------------|
| 1 | `sdram_pkg` + behavioral model + `sdram_ctrl` skeleton + sim PHY | Compiles + lints clean; controller unit tests pass |
| 2 | Wire into `machine_sim` via sim PHY + bus adapter | `_ram_check` passes in Verilator |
| 3 | `sdram_phy_ecp5` (IOB flops + ODDRX1F); single-domain @ 25 MHz | ULX3S boots, `_ram_check` passes |
| 4 | `sdram_cdc` + dual-domain @ 100 MHz CL2 | `_ram_check` + NetBSD boot |
| 5 | Phase-shift sweep build target + bring-up doc | Documented working window |
| 6 | (Later) open-row + auto-precharge-off → pipelined sequential reads | Re-run validation, measure Dhrystone |

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
