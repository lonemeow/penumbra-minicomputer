# System Registers — Programmer's Reference

System registers are accessed via two privileged Format R instructions:

```
WRSYS Rd, #dev, #reg    ; Write Rd → device[dev].register[reg]
RDSYS Rd, #dev, #reg    ; Read  device[dev].register[reg] → Rd
```

Both are privileged — executing in user mode triggers a privilege fault.
`WRSYS` is **serializing**: its effects are visible before the next
instruction fetch begins.

The `dev` field (4 bits) selects one of 16 devices; `reg` (4 bits)
selects one of 16 registers within that device, for 256 total system
registers.

---

## Device Map

| dev  | Name    | Description                                              |
|:----:|---------|----------------------------------------------------------|
| 0    | MMU     | TLB management, fault registers, translation control     |
| 1    | CPU     | CPU identity (read-only); home for CPU performance counters |
| 2    | L1_DCACHE | L1 D-cache control, geometry info, invalidation        |
| 3    | L1_ICACHE | L1 I-cache control, geometry info, invalidation        |
| 4    | BUS     | Bus controller (autoconfig, bus reset)                   |
| 5–6  | —       | Reserved                                                 |
| 7    | TIMER   | Programmable interval timer                              |
| 8    | MACH    | Machine/board identity (read-only): name, features, CPU clock |
| 9    | L2_CACHE | L2 unified cache (same register layout as L1_*CACHE)    |
| 10–14| —       | Reserved (future cache levels, DMA, etc.)                |
| 15   | DEBUG   | ISS-only debug aids (watchpoints); unmapped on hardware  |

CPU identity (slot 1) and machine identity (slot 8) are deliberately
separate so the same CPU core can be instantiated on different boards.
CPU identity is invariant for a given RTL release; machine identity
varies by integration (board name, PLL frequency, board feature word).

> **Note:** I/O peripherals (UART, SPI, GPIO, Ethernet) are **not** on
> the sysreg bus. They are memory-mapped at `0xFF00_0000`+ and accessed
> via `LDW`/`STW`. See [bus.md](./bus.md#io-peripheral-map).

---

## Device 0: MMU

| reg   | Name          | R/W | Description                                          |
|:-----:|---------------|:---:|------------------------------------------------------|
| 0     | `MMUCR`       | R/W | Control register                                     |
| 1     | `FAULT_ADDR`  | R   | Faulting virtual address (latched on fault)          |
| 2     | `FAULT_STATUS`| R   | Fault type and access info (latched on fault)        |
| 3     | `TLB_VPN`     | R/W | Staged TLB upper word (VPN + ASID)                   |
| 4     | `TLB_PTE`     | R/W | TLB lower word (PPN + flags); **write commits entry**|
| 5     | `TLB_INDEX`   | R/W | Selects TLB slot; **bit 6 = pinned TLB**             |
| 6–15  | —             | —   | Reserved (reads as 0)                                |

See [mmu.md](./mmu.md) for the programming model (load/invalidate
protocols, boot sequence, Enable MMU safely).

### MMUCR (reg 0)

```
 31              16 15        8 7         1 0
┌──────────────────┬──────────┬───────────┬─┐
│    (reserved)    │ ASID (8) │(reserved) │M│
└──────────────────┴──────────┴───────────┴─┘
```

- **M** (bit 0): 0 = bypass mode (identity map, uncached, no faults). 1 = TLB active.
- **ASID** (bits 15:8): Current address space ID for TLB matching.

```asm
; Enable MMU with ASID 3
LLI   R1, #0x0301       ; ASID=3, M=1
WRSYS R1, #0, #0
```

### FAULT_STATUS (reg 2)

```
 31    12 11 10  9  8 7    4 3       0
┌────────┬───┬──┬──┬──┬─────┬─────────┐
│(rsvd)  │USR│ X│ W│ R│(gap)│  TYPE   │
└────────┴───┴──┴──┴──┴─────┴─────────┘
```

- **TYPE** (bits 3:0): `0001` = TLB miss, `0010` = protection violation.
- **R/W/X** (bits 10:8): Which access faulted (one-hot).
- **USR** (bit 11): 1 = fault was in user mode.

### TLB_INDEX (reg 5)

```
 31                       6   5    4       0
┌─────────────────────────┬───┬─────┬──────┐
│        (zero)           │PIN│ way │ set  │
└─────────────────────────┴───┴─────┴──────┘
```

| Bits | Name | Description                                          |
|:----:|------|------------------------------------------------------|
| 4:0  | set  | Set index (0–31) for main TLB                        |
| 5    | way  | Way (0 or 1) within set for main TLB                 |
| 6    | PIN  | 0 = main TLB (way/set); 1 = pinned TLB (slot in bits 1:0) |

The hardware requires main TLB entries for VA to live in `set = VA[16:12]`.
Software chooses the way for replacement.

### Pinned TLB (TLB_INDEX bit 6)

The pinned TLB is a 4-entry fully-associative structure checked in
parallel with the main TLB. A pinned hit takes priority. Use for
entries that must never cause TLB misses (miss handler code page,
PGD). No set constraint applies.

```
 TLB_INDEX bit 6 = 0:  main TLB     {way=bit5, set=bits4:0}
 TLB_INDEX bit 6 = 1:  pinned TLB   {slot=bits1:0}
```

### TLB_VPN (reg 3) and TLB_PTE (reg 4)

The programmer-visible packing (the stored form in the TLB array
differs and is not directly accessible):

```
TLB_VPN:
 31  28 27                8 7        0
┌──────┬──────────────────┬──────────┐
│ 0000 │    VPN (20)      │ ASID (8) │
└──────┴──────────────────┴──────────┘

TLB_PTE:
 31              12 11    8 7 6 5 4 3 2 1 0
┌─────────────────┬───────┬─┬─┬─┬─┬─┬─┬─┬─┐
│    PPN (20)     │ SW(4) │G│U│X│W│R│C│—│V│
└─────────────────┴───────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

| Bit | Name | Hex  | Description                                    |
|:---:|------|------|------------------------------------------------|
| 0   | V    | 0x01 | Valid — entry participates in lookup           |
| 2   | C    | 0x04 | Cacheable (0 for MMIO / DMA buffers)           |
| 3   | R    | 0x08 | Read permission                                |
| 4   | W    | 0x10 | Write permission                               |
| 5   | X    | 0x20 | Execute permission                             |
| 6   | U    | 0x40 | User-mode accessible                           |
| 7   | G    | 0x80 | Global — skip ASID match                       |

**Commit order.** `TLB_VPN` stages data in a holding register. The
entry is committed to the TLB only when `TLB_PTE` is written. Always
write `TLB_INDEX` before `TLB_VPN`/`TLB_PTE`.

### Building TLB Values in Assembly

**TLB_VPN word** — `(VPN << 8) | ASID`:

```asm
; VPN = 5 (page at 0x5000), ASID = 0
LLI   R2, #0x0500            ; (5 << 8) | 0
WRSYS R2, #0, #3
```

**TLB_PTE word** — `(PPN << 12) | (SW << 8) | flags`. PPN starts at
bit 12, which straddles the LLI/LUI boundary. For PPN values 0–15 a
single `LLI` suffices; larger PPN values need `LLI` + `LUI`.

```asm
; PPN = 1, flags = V|R|G (read-only kernel page)
; PTE = (1 << 12) | 0x89 = 0x1089
LLI   R3, #0x1089
WRSYS R3, #0, #4             ; commits entry

; PPN = 0x12345, flags = V|R|W|X|G (kernel code+data)
; PTE = (0x12345 << 12) | 0xB9 = 0x123450B9
LLI   R3, #0x50B9
LUI   R3, #0x1234
WRSYS R3, #0, #4
```

See [mmu.md](./mmu.md#tlb-operations) for load/read/invalidate
recipes, and [mmu.md](./mmu.md#enabling-the-mmu-safely) for the MMU
enable procedure.

---

## Device 1: CPU (CPU Identity + Performance Counters)

Read-only. Writes are ignored. Describes the CPU core itself, not the
board it runs on (board info lives in [Device 8: MACH](#device-8-mach-machine-identity)).
The same CPU RTL produces the same answers regardless of which board
instantiates it. Identity registers (regs 0–4) are pure combinational
constants; performance counters (regs 5+) are clocked, free-running,
reset to 0 on system reset.

| reg    | Name                  | R/W | Description                                         |
|:------:|-----------------------|:---:|-----------------------------------------------------|
| 0      | `CPU_ISA`             | R   | ISA version and CPU feature flags                   |
| 1–4    | `CPU_NAME0–3`         | R   | CPU name string (16 bytes, packed LE, null-pad)     |
| 5      | `CPU_CYCLES`          | R   | Free-running 32-bit CPU clock cycle counter         |
| 6      | `CPU_INSNS_RETIRED`   | R   | Free-running 32-bit instruction-retired counter     |
| 7–15   | —                     | —   | Reserved for additional performance counters        |

### Performance Counters (regs 5+)

All counters are 32-bit and free-running. At 12.5 MHz the cycle counter
wraps every ~5.7 minutes; benchmarks take seconds, so software gets
reliable deltas by reading once before the measured region and once
after, then subtracting (modular subtraction handles wrap correctly).

There is **no atomic snapshot** across multiple counters. Reads of
different counters happen one cycle apart, so a `cycles`/`insns_retired`
pair reflects state ~1 cycle apart. For benchmark workloads that run
millions of cycles, the inter-counter skew is negligible. If a
fully-consistent multi-counter snapshot is ever needed, software can
sample twice and average, or hardware can be extended with a snapshot
register later.

### CPU_ISA (reg 0)

CPU core identification: ISA version in the low nibble, feature flags
above. The meaning of feature flags depends on the ISA version, so
bits can be reclaimed when features become mandatory in a future ISA
revision.

```
 31                          4 3       0
┌────────────────────────────┬─────────┐
│     Feature flags (28)     │ ISA (4) │
└────────────────────────────┴─────────┘
```

**ISA version** (bits 3:0):

| Value | Meaning                                |
|:-----:|----------------------------------------|
| 1     | ISA v1 — base Penumbra instruction set |

**Feature flags** (bits 31:4, ISA v1):

| Bit  | Name   | Description                        |
|:----:|--------|------------------------------------|
| 4    | HW_MUL | Hardware multiply present          |
| 5    | HW_DIV | Hardware divide present            |
| 6    | FPU    | Floating-point unit present        |
| 7–31 | —      | Reserved (0)                       |

```asm
; Boot-time ISA check with optional FPU detection
RDSYS R1, #CPU, #CPU_ISA
ANDI  R2, R1, #0x0F
CMPI  R2, #1
BNE   unsupported_isa
ANDI  R2, R1, #0x40           ; FPU bit
BNE   has_fpu
```

### CPU_NAME0–3 (regs 1–4)

A 16-byte null-padded ASCII string packed little-endian into four
consecutive 32-bit registers. The first character occupies bits
`[7:0]` of `CPU_NAME0`, the second `[15:8]`, and so on. Software reads
the regs sequentially and extracts bytes; reading stops at the first
null. Default value: `"Penumbra/1"`.

The `cpuid` RTL module accepts parameters to override the name for
forks of the core.

---

## Device 8: MACH (Machine Identity)

Read-only. Writes are ignored. Describes the board / machine the CPU
is mounted on. Anything whose value would change if the same CPU core
were dropped onto a different board belongs here.

| reg    | Name             | R/W | Description                                      |
|:------:|------------------|:---:|--------------------------------------------------|
| 0      | `MACH_FEAT`      | R   | Machine/board feature flags                      |
| 1–4    | `MACH_NAME0–3`   | R   | Machine name (16 bytes, packed LE, null-pad)     |
| 5      | `MACH_CPU_FREQ`  | R   | CPU clock frequency in Hz (board PLL output)     |
| 6–15   | —                | —   | Reserved (reads as 0)                            |

### MACH_FEAT (reg 0)

| Bit  | Name         | Description                            |
|:----:|--------------|----------------------------------------|
| 0    | BOOT_UART    | Boot console is UART                   |
| 1    | BOOT_DISPLAY | Boot console is display + keyboard     |
| 2–31 | —            | Reserved (0)                           |

### MACH_NAME0–3 (regs 1–4)

16-byte null-padded ASCII string, packed identically to `CPU_NAME`.
Default per platform — e.g. `"Simulator"` on the Verilator/ISS sim
and `"ULX3S"` on the FPGA board. The `machid` RTL module accepts
parameters to override the name per machine integration.

### MACH_CPU_FREQ (reg 5)

CPU clock frequency in Hz as a plain 32-bit unsigned. The CPU itself
has no way to know what frequency it's clocked at — that's a property
of the board's PLL configuration, so the value is supplied by the
board top-level. Reads as 0 if the platform does not report a
frequency. Useful for boot banner, kernel timekeeping calibration,
and benchmarks. For the ULX3S at 12.5 MHz: reads as `12,500,000`.

```asm
RDSYS R1, #MACH, #CPU_FREQ    ; R1 = CPU clock frequency in Hz
```

---

## Cache devices (2 = L1_DCACHE, 3 = L1_ICACHE, 9 = L2_CACHE)

All cache devices — L1 D-cache, L1 I-cache, L2, and any future L3 —
expose the **same register layout**. Software detects presence by
reading `INFO`: a zero result means the cache is absent (either not
instantiated in this build or the device id is unmapped). This lets
the kernel run one discovery loop across cache slots without per-level
code paths.

"Unified vs split" is not encoded explicitly; a unified L1 simply
leaves one of `L1_DCACHE`/`L1_ICACHE` reporting `INFO = 0`. L2+ are
always unified in this architecture.

**All caches are disabled at reset** — the kernel enables them
after setting up TLB mappings.

| reg   | Name         | R/W | Description                                       |
|:-----:|--------------|:---:|---------------------------------------------------|
| 0     | `INFO`       | R   | Cache geometry; `0` ⇒ absent                      |
| 1     | `CTRL`       | R/W | Control register (`[0] = ENABLE`)                 |
| 2     | `INVAL_ALL`  | W   | Drop all lines (no writeback). Written value ignored. |
| 3     | `INVAL_LINE` | W   | Drop the line covering a physical address; no-op if absent (*reserved — current RTL does not implement this*) |
| 4     | `FLUSH_ALL`  | W   | Writeback all dirty lines, keep valid (*reserved — write-back caches only*) |
| 5     | `FLUSH_LINE` | W   | Writeback the line covering a physical address (*reserved — write-back caches only*) |
| 6     | `STATUS`     | R   | `[0] = BUSY` (multi-cycle op in progress)         |
| 7–15  | —            | —   | Reserved (reads as 0; available for perfctrs)     |

`INVAL_LINE`, `FLUSH_ALL`, and `FLUSH_LINE` occupy fixed slots in
the register map so software written against them stays portable to
future hardware. They are intentionally not implemented yet: current
L1/L2 are write-through (no dirty data to flush), and the L1 caches
are small enough that full-flush cost is negligible (`INVAL_ALL`
clears all valid bits in a single cycle).  Writes to unimplemented
registers complete silently.

### INFO (reg 0)

Packed cache geometry for software discovery. A present cache always
has `INFO ≠ 0` because `LINE_WORDS`, `NUM_SETS`, and `NUM_WAYS` are
each at least 1.

| Bits  | Field        | Description                                      |
|:-----:|--------------|--------------------------------------------------|
| 5:0   | LINE_WORDS   | Words per cache line (1..63)                     |
| 20:6  | NUM_SETS     | Number of sets (1..32767)                        |
| 25:21 | NUM_WAYS     | Associativity (1..31, 1 = direct-mapped)         |
| 27:26 | ADDRESSING   | 0 = PIPT, 1 = VIPT, 2 = VIVT                     |
| 28    | WRITE_BACK   | 0 = write-through, 1 = write-back                |
| 29    | WRITE_ALLOC  | 0 = write-no-allocate, 1 = write-allocate        |
| 31:30 | —            | Reserved (0)                                     |

```asm
; Discover D-cache line size at boot
RDSYS R1, #2, #0              ; L1_DCACHE INFO
ANDI  R1, R1, #0x3F           ; LINE_WORDS

; Probe L2 presence
RDSYS R2, #9, #0              ; L2_CACHE INFO  (0 ⇒ no L2)
```

### CTRL (reg 1)

| Bit  | Name   | Reset | Description                                     |
|:----:|--------|:-----:|-------------------------------------------------|
| 0    | ENABLE | 0     | Cache enable. 0 = all accesses pass through.    |
| 31:1 | —      | 0     | Reserved                                        |

```asm
; Enable D-cache after TLB setup
LLI   R1, #1
WRSYS R1, #2, #1              ; L1_DCACHE CTRL.ENABLE = 1
```

### INVAL_ALL (reg 2) / INVAL_LINE (reg 3)

`INVAL_ALL` drops every line; the written value is ignored.
`INVAL_LINE` takes a **physical address** — hardware extracts the
matching set/way from the address, drops the line if present, no-op
if not.

```asm
; Invalidate I-cache after loading new code
WRSYS R0, #3, #2              ; L1_ICACHE INVAL_ALL

; Drop the L2 line covering one buffer page
WRSYS R1, #9, #3              ; L2_CACHE INVAL_LINE — R1 = PA
```

Multi-cycle ops (e.g. L2's set walker for `INVAL_ALL`) signal
completion via `STATUS.BUSY`; the sysreg interface itself is
single-cycle, so the kernel polls without blocking the bus.

**I-cache coherence.** After copying code to RAM (e.g., `exec()`,
dynamic linking, JIT), the kernel must invalidate the I-cache before
executing it. The D-cache write-through policy ensures data reaches
RAM immediately, so only the I-cache needs invalidation.

**DMA coherence.** Before a DMA read (device → memory), invalidate
D-cache lines covering the buffer so the CPU reads fresh data from
RAM. Alternatively, map DMA buffers with `C=0` (uncached) and avoid
the problem entirely.

---

## Device 4: BUS (Bus Controller)

Controls the Penumbra Bus `rst` and `cfg` signals for autoconfig and
bus reset. See [bus.md](./bus.md#device-discovery-autoconfig) for the
full protocol.

| reg   | Name     | R/W | Description                       |
|:-----:|----------|:---:|-----------------------------------|
| 0     | `BUSCTL` | R/W | Bus control register              |
| 1–15  | —        | —   | Reserved (reads as 0)             |

### BUSCTL (reg 0)

| Bit  | Name    | Reset | Description                                               |
|:----:|---------|:-----:|-----------------------------------------------------------|
| 0    | RST     | 0     | Assert/deassert `rst` on the bus (sticky, SW-timed pulse) |
| 1    | CFG_EN  | 0     | Enable config chain and config address range (`0xFE00_0000`) |
| 31:2 | —       | 0     | Reserved                                                  |

`RST` is a plain R/W bit — software controls the hold time because the
external bus is asynchronous. There is no hardware auto-clear.

`CFG_EN` gates the `cfg` daisy chain and enables the config address
range. When clear, accesses to `0xFE00_0000` bus-fault. Software
**must toggle** `CFG_EN` (clear then set) after writing each
device's `CFG_BASE` to advance the chain.

```asm
; Minimal autoconfig step per device (after CFG_BASE is written)
WRSYS R0, #4, #0              ; clear CFG_EN
LLI   R1, #2
WRSYS R1, #4, #0              ; set CFG_EN — next device now active
```

See [bus.md](./bus.md#autoconfig-boot-sequence) for the full loop.

---

## Device 7: TIMER (Programmable Interval Timer)

16-bit countdown timer ticking at a fixed hardware frequency
independent of the CPU clock. Designed for periodic OS scheduler ticks
(typically 100 Hz) and general-purpose timeouts. The timer drives
**`VEC_TIMER`** (vector 1) directly — no interrupt controller between
it and the CPU.

| reg   | Name       | R/W   | Description                                   |
|:-----:|------------|:-----:|-----------------------------------------------|
| 0     | `TMFREQ`   | R     | Timer tick frequency in Hz (hardwired)        |
| 1     | `TMCR`     | R/W   | Control register                              |
| 2     | `TMCOUNT`  | R/W   | Current 16-bit counter (decrements each tick) |
| 3     | `TMRELOAD` | R/W   | 16-bit reload value (copied to `TMCOUNT` on underflow) |
| 4     | `TMSTATUS` | R/W1C | Status register                               |
| 5–15  | —          | —     | Reserved (reads as 0)                         |

### TMFREQ (reg 0)

Hardwired tick frequency in Hz. Default: `1,000,000` (1 MHz). The
kernel reads this at boot to compute the reload value:
`reload = TMFREQ / desired_hz - 1`.

### TMCR (reg 1)

```
 31                          2      1      0
┌────────────────────────────┬──────┬──────┬──────┐
│         (reserved)         │ ALD  │IRQ_EN│TCK_EN│
└────────────────────────────┴──────┴──────┴──────┘
```

| Bit  | Name     | Reset | Description                                              |
|:----:|----------|:-----:|----------------------------------------------------------|
| 0    | TICK_EN  | 0     | Enable counting (`TMCOUNT` decrements on each tick)      |
| 1    | IRQ_EN   | 0     | Enable interrupt output: `o_irq = UDF & IRQ_EN`          |
| 2    | AUTOLOAD | 0     | Auto-reload `TMCOUNT` from `TMRELOAD` on underflow       |
| 31:3 | —        | 0     | Reserved                                                 |

### TMCOUNT (reg 2) and TMRELOAD (reg 3)

Both 16-bit fields in a 32-bit register. Upper 16 bits read as zero;
writes are masked. Writing `TMCOUNT` while the timer runs updates the
counter immediately — the next tick decrements from the new value.
Writing `TMRELOAD` does not affect the running count.

### TMSTATUS (reg 4)

| Bit  | Name | Reset | Description                                     |
|:----:|------|:-----:|-------------------------------------------------|
| 0    | UDF  | 0     | Underflow flag. Set by hardware. **Write-1-to-clear.** |
| 31:1 | —    | 0     | Reserved                                        |

The IRQ output is **level-triggered**: `o_irq = UDF & IRQ_EN`. The
handler **must** clear `UDF` by writing 1 before `ERET`, otherwise the
interrupt re-fires immediately.

### Timer Behaviour

When `TICK_EN=1`, the counter decrements on each synchronised tick
edge. When the counter is at 0 and the next tick arrives
(**underflow**):

- `UDF` is set.
- If `AUTOLOAD=1`: `TMCOUNT` reloads from `TMRELOAD`; counting continues.
- If `AUTOLOAD=0`: `TICK_EN` is cleared (one-shot mode).

**Periodic mode** (`AUTOLOAD=1`): period = `(TMRELOAD + 1)` ticks. For
100 Hz at a 1 MHz tick: `TMRELOAD = 9999`.

**One-shot mode** (`AUTOLOAD=0`): fires once after `(TMCOUNT + 1)`
ticks, then stops. Software must re-enable `TICK_EN` for another shot.

### Clock Domain

The tick input may come from a separate clock (e.g., an external
oscillator on a discrete build). The timer internally synchronises it
via a two-flip-flop synchroniser + rising-edge detector, adding 2–3
CPU cycles of latency — negligible for scheduling.

On FPGA, the tick is generated by a prescaler dividing the CPU clock
(e.g., ÷25 for 1 MHz from 25 MHz). On a discrete build, it could be a
standalone 1 MHz crystal.

### Example: 100 Hz Periodic Timer

```asm
; Reload = TMFREQ / 100 - 1. Assumes TMFREQ = 1,000,000.
LI    R2, #9999
WRSYS R2, #7, #3              ; TMRELOAD
WRSYS R2, #7, #2              ; TMCOUNT (initial)

; Enable: periodic + IRQ
LLI   R3, #0x07                ; TICK_EN | IRQ_EN | AUTOLOAD
WRSYS R3, #7, #1               ; TMCR

EI                             ; enable CPU interrupts
```

### Example: Timer Interrupt Handler

```asm
timer_handler:
    ; ... handle tick (update jiffies, run scheduler, etc.) ...

    ; Acknowledge: clear UDF (write-1-to-clear)
    LLI   R10, #1
    WRSYS R10, #7, #4          ; TMSTATUS
    ERET
```
