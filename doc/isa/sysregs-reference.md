# System Registers — Programmer's Reference

System registers are accessed via two privileged Format R instructions:

```
WRSYS Rd, #dev, #reg    ; Write Rd → device[dev].register[reg]
RDSYS Rd, #dev, #reg    ; Read  device[dev].register[reg] → Rd
```

Both are privileged — executing in user mode triggers a privilege fault.
WRSYS is serializing: its effects are visible before the next instruction fetch.

The `dev` field (4 bits) selects one of 16 devices; `reg` (4 bits) selects
one of 16 registers within that device, for 256 total system registers.

---

## Device Map

| dev | Name | Description |
|-----|------|-------------|
| 0 | MMU | TLB management, fault registers, address translation control |
| 1 | SYS | CPU and machine identification (read-only) |
| 2 | DCACHE | D-cache control, geometry info, invalidation |
| 3 | ICACHE | I-cache control, geometry info, invalidation |
| 4 | BUS | Bus controller (autoconfig, bus reset) |
| 5–6 | — | Reserved for future cache levels (L2, L3) |
| 7 | TIMER | Programmable interval timer |
| 8 | INTC | Interrupt controller (future) |
| 9–15 | — | Reserved for future devices (DMA, etc.) |

> **Note:** I/O peripherals (UART, SPI, GPIO, Ethernet) are **not** on the sysreg bus.
> They are memory-mapped at `0xFF00_0000`+ and accessed via `LDW`/`STW`.
> See `doc/bus/bus-overview.md` for the I/O peripheral map.

---

## Device 0: MMU

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | MMUCR | R/W | Control register |
| 1 | FAULT_ADDR | R | Faulting virtual address (latched on fault) |
| 2 | FAULT_STATUS | R | Fault type and access info (latched on fault) |
| 3 | TLB_VPN | R/W | Staged TLB upper word (VPN + ASID) |
| 4 | TLB_PTE | R/W | TLB lower word (PPN + flags); **write commits entry** |
| 5 | TLB_INDEX | R/W | Selects TLB slot; **bit 6 = pinned TLB** (see below) |
| 6–15 | — | — | Reserved (reads as 0) |

### MMUCR (reg 0)

```
 31              16 15        8 7         1 0
┌──────────────────┬──────────┬───────────┬─┐
│    (reserved)    │ ASID (8) │(reserved) │M│
└──────────────────┴──────────┴───────────┘
```

- **M** (bit 0): 0 = bypass mode (identity map, uncached, no faults). 1 = TLB active.
- **ASID** (bits 15:8): Current address space ID for TLB matching.

```asm
; Enable MMU with ASID 3
LLI  R1, #0x0301       ; ASID=3, M=1
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
┌─────────────────────────┬─────┬──────────┐
│        (zero)           │ way │   set    │
└─────────────────────────┴─────┴──────────┘
```

- **set** (bits 4:0): Set index, 0–31.
- **way** (bit 5): Way within the set, 0 or 1.

The hardware requires that a main TLB entry for virtual address VA lives in set `VA[16:12]`.
Software chooses which of the two ways (0 or 1) to use for replacement.

```asm
; Select way 1 of set 5
LLI  R1, #0x25          ; (1 << 5) | 5 = 0x25
WRSYS R1, #0, #5
```

### Pinned TLB (via TLB_INDEX bit 6)

The pinned TLB is a 4-entry fully-associative structure checked in
parallel with the main TLB.  A pinned hit takes priority.  Use it for
entries that must never cause TLB misses (TLB miss handler code, page
global directory).

Setting **bit 6** of TLB_INDEX selects the pinned TLB.  Bits 1:0
then select the pinned slot (0–3).  The same TLB_VPN/TLB_PTE
registers and 3-write protocol apply — no separate registers needed.

```
 TLB_INDEX bit 6 = 0:  main TLB     {way=bit5, set=bits4:0}
 TLB_INDEX bit 6 = 1:  pinned TLB   {slot=bits1:0}
```

```asm
; Pin slot 0: map VPN 0 → PPN 0 with kernel flags
LLI  R1, #0x40         ; bit 6 = pinned, slot 0
WRSYS R1, #0, #5       ; TLB_INDEX
LLI  R1, #0
WRSYS R1, #0, #3       ; TLB_VPN = {VPN=0, ASID=0}
LLI  R2, #0xBD         ; V|C|R|W|X|G
WRSYS R2, #0, #4       ; TLB_PTE — commits to pinned slot 0
```

No set constraint applies — any virtual page can be placed in any
pinned slot.  Reads via RDSYS also follow TLB_INDEX bit 6 to select
which TLB to read from.

### TLB_VPN (reg 3) and TLB_PTE (reg 4) — Sysreg Packing

These two 32-bit registers are the programmer-visible interface for reading
and writing TLB entries. Their bit layout does **not** match the 64-bit
logical diagram in `doc/mmu/mmu-overview.md` — that diagram shows the
hardware storage; these are the values software actually reads and writes.

**TLB_VPN** — staged, not committed until TLB_PTE is written:

```
 31  28 27                8 7        0
┌──────┬──────────────────┬──────────┐
│ 0000 │    VPN (20)      │ ASID (8) │
└──────┴──────────────────┴──────────┘
```

**TLB_PTE** — writing this register commits both VPN and PTE to the TLB:

```
 31              12 11    8 7 6 5 4 3 2 1 0
┌─────────────────┬───────┬─┬─┬─┬─┬─┬─┬─┬─┐
│    PPN (20)     │ SW(4) │G│U│X│W│R│C│—│V│
└─────────────────┴───────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

Flag bits:

| Bit | Name | Hex | Description |
|-----|------|-----|-------------|
| 0 | V | 0x01 | Valid — entry participates in lookup |
| 2 | C | 0x04 | Cacheable (0 for MMIO) |
| 3 | R | 0x08 | Read permission |
| 4 | W | 0x10 | Write permission |
| 5 | X | 0x20 | Execute permission |
| 6 | U | 0x40 | User-mode accessible |
| 7 | G | 0x80 | Global (skip ASID match) |

### Building TLB Values in Assembly

**TLB_VPN word** — `(VPN << 8) | ASID`:

```asm
; VPN = 5 (page at 0x5000), ASID = 0
LLI  R2, #0x0500         ; (5 << 8) | 0 = 0x0500
WRSYS R2, #0, #3
```

**TLB_PTE word** — `(PPN << 12) | (SW << 8) | flags`:

Note that PPN starts at bit 12, which straddles the 16-bit LLI/LUI
boundary. For PPN values 0–15, a single LLI suffices. Larger PPN
values need LLI + LUI.

```asm
; PPN = 1, flags = V|R|G (read-only kernel page)
; PTE = (1 << 12) | 0x89 = 0x1089
LLI  R3, #0x1089
WRSYS R3, #0, #4         ; commits entry

; PPN = 0x12345, flags = V|R|W|X|G (kernel code+data)
; PTE = (0x12345 << 12) | 0xB9 = 0x123450B9
LLI  R3, #0x50B9         ; lower 16 bits
LUI  R3, #0x1234         ; upper 16 bits
WRSYS R3, #0, #4
```

### Common Recipes

**Load a TLB entry:**

```asm
; Map virtual page V to physical page P with flags F, ASID A
; Target: way 0 of the required set
;   set = V & 0x1F  (low 5 bits of VPN)
;   TLB_INDEX = set  (way 0)
;   TLB_VPN = (V << 8) | A
;   TLB_PTE = (P << 12) | F
WRSYS R_idx, #0, #5       ; select slot
WRSYS R_vpn, #0, #3       ; stage VPN + ASID
WRSYS R_pte, #0, #4       ; commit PPN + flags
```

**Invalidate a TLB entry** (clear V bit):

```asm
WRSYS R_idx, #0, #5       ; select slot
LLI  R1, #0
WRSYS R1, #0, #3          ; VPN = 0 (don't care)
WRSYS R1, #0, #4          ; PTE = 0 (V=0 → invalid)
```

**Read a TLB entry:**

```asm
WRSYS R_idx, #0, #5       ; select slot
RDSYS R2, #0, #3          ; R2 = TLB_VPN
RDSYS R3, #0, #4          ; R3 = TLB_PTE
```

**Load a pinned TLB entry** (bit 6 set in TLB_INDEX):

```asm
; R_idx has bit 6 set + slot number (e.g., 0x40 = pinned slot 0)
WRSYS R_idx, #0, #5       ; select pinned slot
WRSYS R_vpn, #0, #3       ; stage VPN + ASID
WRSYS R_pte, #0, #4       ; commit PPN + flags (to pinned TLB)
```

**Invalidate a pinned TLB entry:**

```asm
WRSYS R_idx, #0, #5       ; select pinned slot (bit 6 set)
LLI  R1, #0
WRSYS R1, #0, #4          ; TLB_PTE = 0 (V=0 → invalid)
```

---

## Device 1: SYS (System Identification)

Read-only. Writes are ignored. CPU core and machine/board identification are
separate so the same core can be instantiated on different platforms.

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | CPU_ISA | R | ISA version and CPU feature flags |
| 1 | MACH_FEAT | R | Machine/board feature flags |
| 2–5 | CPU_NAME0–3 | R | CPU name string (16 bytes, packed LE, null-padded) |
| 6–9 | MACH_NAME0–3 | R | Machine name string (16 bytes, packed LE, null-padded) |
| 10–15 | — | — | Reserved (reads as 0) |

### CPU_ISA (reg 0)

CPU core identification: ISA version in the low nibble, optional feature
flags in the upper 28 bits. The meaning of the feature flags depends on the
ISA version, so features that become mandatory in a future ISA revision can
have their bits reclaimed.

```
 31                          4 3       0
┌────────────────────────────┬─────────┐
│     Feature flags (28)     │ ISA (4) │
└────────────────────────────┴─────────┘
```

**ISA version** (bits 3:0):

| Value | Meaning |
|-------|---------|
| 1 | ISA v1 — base Penumbra instruction set |

**Feature flags** (bits 31:4, ISA v1 definitions):

| Bit | Name | Description |
|-----|------|-------------|
| 4 | HW_MUL | Hardware multiply present |
| 5 | HW_DIV | Hardware divide present |
| 6 | FPU | Floating-point unit present |
| 7–31 | — | Reserved (0) |

```asm
; Boot-time ISA check
RDSYS R1, #SYS, #CPU_ISA
ANDI  R2, R1, #0x0F       ; R2 = ISA version
CMPI  R2, #1
BNE   unsupported_isa
; Check for optional FPU
ANDI  R2, R1, #0x40       ; bit 6
BNE   has_fpu
```

### MACH_FEAT (reg 1)

Machine-level feature flags describing the board/platform.

```
 31                                    0
┌──────────────────────────────────────┐
│         Machine features (32)        │
└──────────────────────────────────────┘
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | BOOT_UART | Boot console is UART |
| 1 | BOOT_DISPLAY | Boot console is display + keyboard |
| 2–31 | — | Reserved (0) |

### Name Strings (regs 2–5, 6–9)

Each name is a 16-byte null-padded ASCII string packed little-endian into
4 consecutive 32-bit registers. The first character occupies bits [7:0] of
the first register (NAME0), the second character bits [15:8], and so on.

Software reads regs sequentially and extracts bytes to build the printable
string. Reading stops at the first null byte.

```asm
; Print CPU name (simplified — assumes UART ready)
LLI  R4, #2               ; start at reg 2 (CPU_NAME0)
LLI  R5, #6               ; stop at reg 6
name_loop:
    RDSYS R2, #SYS, R4    ; read next name word
    ; extract and print 4 bytes from R2 ...
    ADDI R4, #1
    CMP  R4, R5
    BNE  name_loop
```

**Default values:**

| String | Default | Description |
|--------|---------|-------------|
| CPU name | `"Penumbra/1"` | Core type and revision |
| Machine name | (per platform) | e.g. `"Simulator"`, `"ULX3S"` |

The `sysid` module accepts parameters to override both names and feature
registers per machine integration (e.g. `machine_sim.sv` sets the machine
name to `"Simulator"`).

---

## Devices 2–3: DCACHE / ICACHE

Devices 2 (D-cache) and 3 (I-cache) share the same register layout.
Each is an independent instance of the parameterized `cache.sv` module.
Cache is disabled at reset; the kernel enables it after setting up TLB mappings.

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | INFO | R | Cache geometry and type (compile-time constant) |
| 1 | CTRL | R/W | Control register |
| 2 | INVAL | W | Invalidation trigger |
| 3–15 | — | — | Reserved (reads as 0) |

### INFO (reg 0) — Read-only

Packed cache geometry for software discovery:

```
Bits [3:0]   — LINE_WORDS  (words per cache line, e.g. 4)
Bits [13:4]  — NUM_SETS    (number of sets, e.g. 64)
Bits [17:14] — NUM_WAYS    (associativity, e.g. 1 = direct-mapped)
Bits [21:18] — CACHE_TYPE  (0 = write-through/write-no-allocate)
Bits [31:22] — Reserved (0)
```

```asm
; Discover D-cache line size at boot
RDSYS R1, #2, #0          ; R1 = DCACHE INFO
LLI   R2, #0x0F
AND   R1, R2              ; R1 = LINE_WORDS
```

### CTRL (reg 1)

| Bit | Name | Reset | Description |
|-----|------|-------|-------------|
| 0 | ENABLE | 0 | Cache enable. When 0, all accesses pass through to memory. |
| 31:1 | — | 0 | Reserved |

```asm
; Enable D-cache after TLB setup
LLI   R1, #1
WRSYS R1, #2, #1          ; DCACHE CTRL.ENABLE = 1
```

### INVAL (reg 2) — Write-only

Writing any value invalidates all cache lines (clears all valid bits).
The written value is accepted as an address for future per-line invalidate
support, but the current implementation ignores it and invalidates all.

```asm
; Invalidate I-cache after loading new code
WRSYS R0, #3, #2          ; ICACHE INVAL (invalidate all)
```

**I-cache coherence:** After copying code to RAM (e.g., `exec()`, dynamic
linking, JIT), the kernel must invalidate the I-cache before executing it.
The D-cache write-through policy ensures data reaches RAM immediately, so
only the I-cache needs invalidation.

**DMA coherence:** Before DMA read (device → memory), invalidate D-cache
lines covering the DMA buffer so the CPU reads fresh data from RAM, not
stale cached copies. Alternatively, map DMA buffers with C=0 (uncached).

---

## Device 4: BUS (Bus Controller)

Controls the Penumbra Bus `rst` and `cfg` signals for device discovery
(autoconfig) and bus reset. See `doc/bus/bus-overview.md` for the full
autoconfig protocol.

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | BUSCTL | R/W | Bus control register |
| 1–15 | — | — | Reserved (reads as 0) |

### BUSCTL (reg 0)

| Bit | Name | Reset | Description |
|-----|------|-------|-------------|
| 0 | RST | 0 | Assert/deassert `rst` on the bus (sticky — software controls timing). |
| 1 | CFG_EN | 0 | Enable config chain (`cfg` signal) and config address range (`0xFE00_0000`). |
| 31:2 | — | 0 | Reserved |

**RST** is a plain R/W bit. Writing 1 asserts `rst` on the bus, resetting all
autoconfigured devices back to their unconfigured state. Writing 0 deasserts
it. Software controls the hold time: the external bus is asynchronous, so
slow devices may need a longer reset pulse. There is no hardware auto-clear.

**CFG_EN** gates the `cfg` daisy chain and enables the config address range
on the memory bus. When clear, accesses to `0xFE00_0000` produce a bus fault.
**Important:** software must toggle CFG_EN (clear then set) after writing
`CFG_BASE` for each device. This advances the config chain to the next
unconfigured device. See `doc/bus/bus-overview.md` for full protocol details.

```asm
; Assert bus reset
LLI   R1, #1
WRSYS R1, #4, #0          ; BUS BUSCTL = RST

; Delay for slow devices on async bus
LLI   R2, #100
.delay: SUBI R2, #1
        BNE  .delay

; Deassert reset, enable config mode
LLI   R1, #2
WRSYS R1, #4, #0          ; BUS BUSCTL = CFG_EN (RST=0)

; ... read config registers, write CFG_BASE ...

; Toggle CFG_EN to advance to next device
WRSYS R0, #4, #0          ; clear CFG_EN
LLI   R1, #2
WRSYS R1, #4, #0          ; set CFG_EN — next device now active

; ... repeat for next device ...

; Disable config mode when done
WRSYS R0, #4, #0          ; BUS BUSCTL = 0
```

---

## Device 7: TIMER (Programmable Interval Timer)

16-bit countdown timer that ticks at a fixed hardware frequency independent
of CPU clock. Designed for periodic interrupts (NetBSD `hardclock` at 100 Hz)
and general-purpose timeouts. The tick frequency is a hardware parameter
(typically 1 MHz) reported via the TMFREQ register.

The timer drives the **VEC_TIMER** exception vector (vector 1) directly —
it does not go through an external interrupt controller.

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | TMFREQ | R | Timer tick frequency in Hz (hardwired constant) |
| 1 | TMCR | R/W | Control register |
| 2 | TMCOUNT | R/W | Current 16-bit counter value (counts down each tick) |
| 3 | TMRELOAD | R/W | 16-bit reload value (copied to TMCOUNT on underflow) |
| 4 | TMSTATUS | R/W1C | Status register |
| 5–15 | — | — | Reserved (reads as 0) |

### TMFREQ (reg 0) — Read-only

```
 31                                    0
┌──────────────────────────────────────┐
│     Timer tick frequency in Hz       │
└──────────────────────────────────────┘
```

Hardwired by the hardware implementation. Default: 1,000,000 (1 MHz).
The kernel reads this at boot to compute the reload value for the desired
interrupt rate: `reload = TMFREQ / desired_hz`.

Writing to TMFREQ has no effect.

### TMCR (reg 1)

```
 31                          3   2      1      0
┌────────────────────────────┬───┬──────┬──────┐
│         (reserved)         │ALD│IRQ_EN│TCK_EN│
└────────────────────────────┴───┴──────┴──────┘
```

| Bit | Name | Reset | Description |
|-----|------|-------|-------------|
| 0 | TICK_EN | 0 | Enable counting. When 1, TMCOUNT decrements on each tick. |
| 1 | IRQ_EN | 0 | Enable interrupt output. When 1, `o_irq = UDF & IRQ_EN`. |
| 2 | AUTOLOAD | 0 | Auto-reload on underflow. See behaviour below. |
| 31:3 | — | 0 | Reserved |

### TMCOUNT (reg 2) — 16-bit counter

```
 31              16 15                   0
┌─────────────────┬──────────────────────┐
│     (zero)      │    Counter (16)      │
└─────────────────┴──────────────────────┘
```

Counts down by 1 on each timer tick when TICK_EN=1. Upper 16 bits always
read as zero; writes are masked to 16 bits. Writing while the timer is
running updates the counter immediately — the next tick decrements from
the new value.

### TMRELOAD (reg 3) — 16-bit reload value

Same format as TMCOUNT. Writing TMRELOAD does not affect the running
counter. The reload value is only used on underflow (when AUTOLOAD=1).

### TMSTATUS (reg 4)

```
 31                                1   0
┌──────────────────────────────────┬───┐
│           (reserved)             │UDF│
└──────────────────────────────────┴───┘
```

| Bit | Name | Reset | Description |
|-----|------|-------|-------------|
| 0 | UDF | 0 | Underflow flag. Set by hardware on underflow. **Write-1-to-clear.** |
| 31:1 | — | 0 | Reserved |

The interrupt output is level-triggered: `o_irq = UDF & IRQ_EN`. The
handler **must** clear UDF by writing 1 to TMSTATUS before returning
(ERET), otherwise the interrupt fires again immediately.

### Timer Behaviour

When TICK_EN=1, the counter decrements by 1 on each synchronised tick edge.
When the counter is at 0 and the next tick arrives (**underflow**):

- UDF flag in TMSTATUS is set.
- If AUTOLOAD=1: TMCOUNT is loaded from TMRELOAD. Counting continues.
- If AUTOLOAD=0: TICK_EN is cleared automatically (one-shot mode).

**Periodic mode** (AUTOLOAD=1): The timer fires repeatedly with a period
of `(TMRELOAD + 1)` ticks. For 100 Hz with a 1 MHz tick:
`TMRELOAD = 1000000 / 100 - 1 = 9999`.

**One-shot mode** (AUTOLOAD=0): The timer fires once after `(TMCOUNT + 1)`
ticks, then stops. Software must re-enable TICK_EN to start another countdown.

### Clock Domain

The tick input may originate from a separate clock domain (e.g., an external
oscillator on a discrete build). The timer internally synchronises it via a
two-flip-flop synchroniser and rising-edge detector. This adds 2–3 CPU clock
cycles of latency, which is negligible for scheduling purposes.

On FPGA, the tick is generated by a prescaler dividing the CPU clock
(e.g., ÷25 for 1 MHz from 25 MHz). On a discrete 74xx build, it could be
a standalone 1 MHz crystal oscillator.

### Example: Set Up 100 Hz Periodic Timer

```asm
; Read timer frequency
RDSYS R1, #TIMER, #TM_FREQ     ; R1 = 1000000

; Compute reload = freq / 100 - 1 = 9999
; (In practice, the kernel does this division at boot.)
LI    R2, #9999
WRSYS R2, #TIMER, #TM_RELOAD
WRSYS R2, #TIMER, #TM_COUNT    ; Also set initial count

; Enable: periodic + IRQ
LLI   R3, #0x07                 ; TICK_EN | IRQ_EN | AUTOLOAD
WRSYS R3, #TIMER, #TM_CR

; Enable CPU interrupts
EI
```

### Example: Timer Interrupt Handler

```asm
timer_handler:
    ; ... handle tick (update jiffies, run scheduler, etc.) ...

    ; Acknowledge interrupt: clear UDF via write-1-to-clear
    LLI   R10, #1
    WRSYS R10, #TIMER, #TM_STATUS

    ERET
```
