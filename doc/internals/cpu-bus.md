# Penumbra CPU-Internal Bus

This document specifies the interfaces *inside* the CPU, between the
core, the MMU, the caches, and the CPU-private sysreg devices. These
contracts are private to the core's microarchitecture — no off-chip
counterpart exists, and they are not visible to software except through
the documented effects of `RDSYS`/`WRSYS` and load/store instructions.

## Scope

**In scope:**
- Core ↔ MMU (virtual-to-physical translation interface).
- Core (post-MMU) ↔ split I/D caches (instruction fetch and data
  load/store interface).
- Core ↔ CPU-private sysreg devices (device IDs 0–3): MMU, CPU
  identity/perfctrs, D-cache control, I-cache control.

**Out of scope** (with cross-references):
- Anything past the **CPU memory port** (`cpu_core.sv`'s `o_mem_*`
  / `i_mem_*`).  That's the **system bus**, governed by the
  **Penumbra Bus** protocol specified in
  [`doc/hardware/bus-protocol.md`](../hardware/bus-protocol.md).
  Inside the FPGA the system bus speaks the **sync form**; the
  **async form** is the future discrete-build target.  Both are the
  same protocol — see
  [Sync-Bus Mapping](../hardware/bus-protocol.md#sync-bus-mapping)
  for the formal correspondence.
- Sysreg traffic to *external* devices (device IDs ≥ 4: BUS, TIMER,
  MACH, …). Those devices live on the same `o_sys_dev`/`o_sys_reg`
  signal set, but the wires leave the core through the sysreg bus
  port and the contract there is governed by the system bus, not by
  this document. See [`doc/system/sysregs.md`](../system/sysregs.md)
  for the programmer's view and the device map.

The dividing line: if a module is instantiated *inside* `cpu_core.sv`,
this document applies. If it sits at machine-level (in `machine_sim.sv`
or `ulx3s_top.sv`), it does not.

## Topology

```
┌──────────────────────────── cpu_core ────────────────────────────┐
│                                                                  │
│   PC / MAR ──┐                                                   │
│              ▼                                                   │
│           ┌─────┐  paddr   ┌───────────┐  fetch    ┌──────────┐  │
│           │ MMU ├─────────►│  I-cache  ├──────────►│          │  │
│           │     │          └───────────┘           │  memory  │  │
│           │     │  paddr   ┌───────────┐  data     │   port   │──┼──► (Penumbra Bus)
│           │     ├─────────►│  D-cache  ├──────────►│   mux    │  │
│           └─────┘          └───────────┘           └──────────┘  │
│             ▲                                                    │
│             │ sys_dev/reg/wdata, sys_rdata                       │
│   ┌─────────┴─────────────────────────────┐                      │
│   │ Internal sysreg fan-in (devices 0–3): │                      │
│   │   MMU  CPU(id+perfctr)  DCACHE  ICACHE│                      │
│   └───────────────────────────────────────┘                      │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
                  ── boundary of this document ──
                              │
                              ▼
              External sysreg + memory devices
              (BUS, TIMER, MACH, UART, SPI, RAM, …)
              — governed by Penumbra Bus protocol, not by this doc
```

The three CPU-internal interfaces — MMU, cache (×2), and internal
sysreg fan-in — each have a different timing contract. The rest of
this document specifies them.

## MMU Translation Interface

**Signals** (`cpu_core` → `mmu`, `mmu` → `cpu_core`):

| Direction | Signal | Description |
|---|---|---|
| → MMU | `i_vaddr[31:0]` | Virtual address |
| → MMU | `i_access_type[2:0]` | One-hot: read / write / exec |
| → MMU | `i_user_mode` | `!SR.S` |
| → MMU | `i_req` | Translation requested this cycle |
| → MMU | `i_force_bypass` | Identity-map (vector fetch) |
| → MMU | `i_mem_size[1:0]` | For alignment check |
| → MMU | `i_bus_fault` | Latched-in past-cache fault |
| ← MMU | `o_paddr[31:0]` | Translated physical address |
| ← MMU | `o_cacheable` | PTE.C |
| ← MMU | `o_hit` | TLB hit |
| ← MMU | `o_fault` | Any access violation |
| ← MMU | `o_align` | Fault is alignment (vector select) |

**Contract:**

1. **Combinational response.** Translation outputs (`o_paddr`,
   `o_cacheable`, `o_hit`, `o_fault`, `o_align`) are pure
   combinational functions of the current cycle's inputs and the
   registered TLB state. No clocked stage may be inserted on the
   request → response path.
2. **No busy signal.** A TLB miss does not stall the core. It is
   reported via `o_fault` with `o_hit=0`, and the core takes a
   `VEC_TLB_MISS` exception. Software refills the TLB and returns.
3. **Bypass-mode mux.** When MMUCR disables translation, `o_paddr =
   i_vaddr` and faults are limited to alignment plus `i_bus_fault`.
4. **Internal state updates** (TLB writes, fault status latching,
   MMUCR writes) are clocked, but they reach the translation outputs
   only through the registered-state read ports, not as additional
   stages on the request path.

**Why it's structured this way.** The translation must complete in the
same cycle as the cache lookup, otherwise the cache would need to
register the address and the fetch path would gain a cycle. The CPU
spends fetch cycles assuming MMU latency = 0; that's the load-bearing
property.

## Cache Interface

**Signals** (`cpu_core` → `cache`, `cache` → `cpu_core`), per cache:

| Direction | Signal | Description |
|---|---|---|
| → cache | `i_paddr[31:0]` | Physical address from MMU |
| → cache | `i_wdata[31:0]` | Store data |
| → cache | `i_byte_en[3:0]` | Byte lane enables |
| → cache | `i_re`, `i_we` | Read / write enables |
| → cache | `i_cacheable` | From MMU `o_cacheable` |
| ← cache | `o_rdata[31:0]` | Read data |
| ← cache | `o_busy` | Result not yet valid this cycle |

**Contract:**

1. **Read hit is combinational.** When `i_re && i_cacheable && hit`,
   the cache must drive `o_busy=0` and place valid data on `o_rdata`
   in the *same* cycle. No clocked stage may sit between the address
   inputs and these outputs on the hit path.
2. **Read miss uses busy.** When `i_re && i_cacheable && !hit`, the
   cache asserts `o_busy=1` from the same cycle (no delay) and holds
   it until line fill completes. On the cycle `o_busy` drops,
   `o_rdata` carries the requested word.
3. **Pass-through path** (uncacheable, cache disabled, or any write).
   The cache forwards `i_re`/`i_we` to its back-side port and presents
   `i_mem_busy`/`i_mem_rdata` to the core.  The core sees the same
   contract from above; the device on the far side of the back-side
   port honors the Penumbra Bus protocol in its
   [sync form](../hardware/bus-protocol.md#sync-bus-mapping),
   which is what makes `i_mem_busy=0` coincide with valid
   `i_mem_rdata`.  This is enforced by the system bus, not by this
   document.
4. **Faults.** A fault on the request (MMU access violation,
   alignment, past-cache `bus_fault`) is signaled out of band through
   the MMU/bus_fault path. The cache does not separately fault.
5. **Single-cycle drop.** When `o_busy` transitions 1 → 0, `o_rdata`
   is valid on the *same* cycle as the drop, not one cycle later.
   The CPU's STALL sequencer latches `mem_rdata` on that cycle.

**Why it's structured this way.** The instruction fetch unit fires
`fetch_complete` on the first cycle of `S_FETCH` where `cache_busy=0`,
trusting that the data is presented combinationally on hits and
that `cache_busy=1` is asserted promptly on misses. The STALL
sequencer for D-side load/store relies on the same drop-equals-valid
property.

## Internal Sysreg Interface (devices 0–3)

**Signals** (shared with the external sysreg bus, but the fan-in for
devices 0–3 is internal to `cpu_core`):

| Direction | Signal | Description |
|---|---|---|
| → device | `i_sys_reg[3:0]` | Register index within device |
| → device | `i_sys_wdata[31:0]` | Write data |
| → device | `i_sys_we` | Write enable (decoded per-device from `dp_r_sys_dev`) |
| ← device | `o_sys_rdata[31:0]` | Read data |

The device select `o_sys_dev` is a *fan-in mux selector* on the read
side: `cpu_core` aggregates `mmu_sys_rdata`, `cpu_sys_rdata`,
`dcache_sys_rdata`, `icache_sys_rdata` and selects by `dp_r_sys_dev`.
External devices participate in the same mux at the system level
through `i_sys_rdata`.

**Devices on the internal sysreg bus:**

| ID | Constant | Module | Notes |
|---|---|---|---|
| 0 | `SYSDEV_MMU` | `mmu` | TLB, fault regs, MMUCR |
| 1 | `SYSDEV_CPU` | `cpuid` + `cpu_perfctr` | Identity (regs 0–4) and perfctrs (regs 5+); merged by reg-range |
| 2 | `SYSDEV_L1_DCACHE` | `cache` (D-side) | Unified cache reg map: INFO, CTRL, INVAL_ALL, INVAL_LINE, FLUSH_*, STATUS |
| 3 | `SYSDEV_L1_ICACHE` | `cache` (I-side) | Same layout as L1_DCACHE |

**Contract:**

1. **Single-cycle response.** `o_sys_rdata` must be valid in the same
   cycle as `i_sys_reg` (and the implicit device-select). No busy
   signal exists on this bus; the CPU's `RDSYS` and `WRSYS`
   micro-routines are single-cycle.
2. **Read is non-destructive.** `i_sys_we=0` may not change device
   state, regardless of `i_sys_reg` value.
3. **Write side-effects fire on the rising edge** that follows
   `i_sys_we=1`. Devices typically use a registered write decode and
   may apply effects (e.g., TLB writes, cache invalidate) one cycle
   later; the read-back on the *next* `RDSYS` reflects the new state.
4. **Reg-index decoding is per-device.** A device sees only its
   `i_sys_reg[3:0]` and is free to assign meanings; the device-level
   register map lives in the device's own documentation
   (e.g. [`doc/system/sysregs.md`](../system/sysregs.md) for the
   programmer's view).

**Why it's structured this way.** Sysreg accesses are intended as
cheap, predictable hooks for state the kernel touches frequently
(TLB walks, cache invalidates, perfctr snapshots). Multi-cycle sysreg
reads would make every TLB miss handler and every context switch
slower.

> **Note for implementers of new internal sysreg devices.**
> If a register read genuinely cannot complete in one cycle (e.g. a
> read drains a FIFO whose pointer must be re-encoded), promote the
> register to the memory bus rather than introducing a busy signal
> on the sysreg bus. The CPU does not have STALL semantics for sysreg
> traffic and adding them would be invasive.

## What These Contracts Enable

- **Zero-cycle fetch on cache hit.** The optimized fetch path
  (`cpu_core.sv`) fires `fetch_complete` on the first cycle of
  `S_FETCH` with `cache_busy=0`. This requires MMU + cache hit-path
  combinationally present in the same cycle as the registered PC.
- **STALL-based load/store.** The micro-sequencer holds the micro-PC
  while `divmul_busy | mem_busy` is high and advances exactly on the
  cycle busy drops. This requires that the drop coincide with valid
  data, not lead it by one cycle.
- **Exception priority.** Bus fault > alignment > TLB protection >
  TLB miss > illegal > priv > BREAK > SYSCALL > IRQ. Implementing
  this priority cleanly requires that all fault signals (MMU,
  pastcache) reach the sequencer combinationally during the same
  cycle as the offending request.
- **Cheap kernel state access.** Single-cycle sysreg reads keep TLB
  miss handlers, context switches, and cache-control sequences out
  of the critical path of system call latency.

## For Implementers

If you replace any of the modules covered by this document, preserve:

- **MMU**: combinational translation outputs, no busy, faults via
  `o_fault`/`o_align`/`o_hit`. Internal state updates may be clocked
  but must not insert pipeline stages on the request → response path.
- **Cache**: hits combinational on `o_rdata` and `o_busy`; miss
  asserts `o_busy=1` immediately and drops it on the cycle data is
  valid. Faults are not separate; bus_fault arrives via the memory
  port and is propagated by `cpu_core`.
- **Internal sysreg device**: combinational `o_sys_rdata` from
  `i_sys_reg`. Writes registered. No busy.

The simulation testbench enables Verilator's `--assert` flag and
several SVA properties guard these contracts at the module
boundaries (see `cpu_core.sv` and `cache.sv`). Regressions show up
on the next `make test` run rather than after a synthesis pass.

## Discrete 74xx Feasibility

The combinational requirements on the MMU and cache hit path are
compatible with a discrete 74xx build. TLB lookup is a parallel
comparator tree (no clocked stage); cache hit detection is a tag
comparator plus a wide mux for data; the sysreg read port is a
fan-in mux. None of these need flops on the read path, so the
contract translates directly to a multi-IC implementation. See
[`mmu-internals.md`](mmu-internals.md) for the TLB IC count.

## See Also

- [`doc/hardware/bus-protocol.md`](../hardware/bus-protocol.md) —
  Penumbra Bus (system bus past the CPU memory port).  Covers both
  sync and async forms; the FPGA build uses only the sync form.
- [`doc/system/sysregs.md`](../system/sysregs.md) — Programmer's view
  of sysregs (device map, register layouts).
- [`doc/system/mmu.md`](../system/mmu.md) — Programmer's view of the
  MMU.
- [`doc/internals/datapath.md`](datapath.md) — Datapath and STALL
  semantics.
- [`doc/internals/microcode.md`](microcode.md) — Sequencer and
  micro-word format.
