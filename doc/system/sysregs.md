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
| 7 | TIMER | Programmable interval timer |

> **Note:** I/O peripherals (UART, SPI, GPIO, Ethernet) are **not** on the sysreg bus.
> They are memory-mapped at `0xFF00_0000`+ and accessed via `LDW`/`STW`.

---

## Device 0: MMU

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | MMUCR | R/W | Control register |
| 1 | FAULT_ADDR | R | Faulting virtual address |
| 2 | FAULT_STATUS | R | Fault type and access info |
| 3 | TLB_VPN | R/W | Staged TLB upper word (VPN + ASID) |
| 4 | TLB_PTE | R/W | TLB lower word (PPN + flags); **write commits entry** |
| 5 | TLB_INDEX | R/W | Selects TLB slot; **bit 6 = pinned TLB** |

### Building TLB Values in Assembly

**TLB_VPN word** — `(VPN << 8) | ASID`:
```asm
; VPN = 5, ASID = 0
LLI  R2, #0x0500         ; (5 << 8) | 0 = 0x0500
WRSYS R2, #0, #3
```

**TLB_PTE word** — `(PPN << 12) | (SW << 8) | flags`:
```asm
; PPN = 1, flags = V|R|G (read-only kernel page)
LLI  R3, #0x1089
WRSYS R3, #0, #4         ; commits entry
```

---

## Device 1: SYS (System Identification)

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | CPU_ISA | R | ISA version and CPU feature flags |
| 1 | MACH_FEAT | R | Machine/board feature flags |
| 2–5 | CPU_NAME0–3 | R | CPU name string (16 bytes, LE) |
| 6–9 | MACH_NAME0–3 | R | Machine name string (16 bytes, LE) |
| 10 | CPU_FREQ | R | CPU clock frequency in Hz |

---

## Devices 2–3: DCACHE / ICACHE

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | INFO | R | Cache geometry and type |
| 1 | CTRL | R/W | Control register |
| 2 | INVAL | W | Invalidation trigger (write any value to flush all) |

---

## Device 4: BUS (Bus Controller)

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | BUSCTL | R/W | Bus control register |

### BUSCTL Fields
| Bit | Name | Description |
|-----|------|-------------|
| 0 | RST | Assert/deassert `rst` on the bus (sticky). |
| 1 | CFG_EN | Enable config mode and config address range (`0xFE00_0000`). |

---

## Device 7: TIMER (Programmable Interval Timer)

16-bit countdown timer. Tick frequency is reported via `TMFREQ`.

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | TMFREQ | R | Timer tick frequency in Hz |
| 1 | TMCR | R/W | Control register |
| 2 | TMCOUNT | R/W | Current 16-bit counter value |
| 3 | TMRELOAD | R/W | 16-bit reload value |
| 4 | TMSTATUS | R/W1C | Status register (bit 0 = Underflow) |

### Timer Interrupt
The timer drives **VEC_TIMER** (vector 1) directly. The handler must clear the underflow bit by writing 1 to `TMSTATUS` bit 0.
