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
| 1 | SYS | Machine identification (read-only) |
| 2–15 | — | Reserved for future devices (interrupt controller, timer, DMA) |

---

## Device 0: MMU

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | MMUCR | R/W | Control register |
| 1 | FAULT_ADDR | R | Faulting virtual address (latched on fault) |
| 2 | FAULT_STATUS | R | Fault type and access info (latched on fault) |
| 3 | TLB_VPN | R/W | Staged TLB upper word (VPN + ASID) |
| 4 | TLB_PTE | R/W | TLB lower word (PPN + flags); **write commits entry** |
| 5 | TLB_INDEX | R/W | Selects TLB slot for indexed read/write |
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

The hardware requires that an entry for virtual address VA lives in set `VA[16:12]`.
Software chooses which of the two ways (0 or 1) to use for replacement.

```asm
; Select way 1 of set 5
LLI  R1, #0x25          ; (1 << 5) | 5 = 0x25
WRSYS R1, #0, #5
```

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

---

## Device 1: SYS (System Identification)

Read-only. Writes are ignored.

| reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | MACHINE_ID | R | Hardware identification |
| 1–15 | — | — | Reserved (reads as 0); future: feature flags, TLB geometry, cache config |

### MACHINE_ID (reg 0)

Returns a monotonically increasing hardware revision number. Software uses
this to identify the platform and adapt at boot time.

| Value | Meaning |
|-------|---------|
| 1 | Penumbra/1 — first hardware revision (FPGA simulation) |

```asm
; Boot-time hardware check
RDSYS R1, #1, #0          ; R1 = MACHINE_ID
CMPI  R1, #1
BNE   unsupported_hw
```

Future revisions increment this value. Capability registers (TLB geometry,
cache properties, optional features) will be added at registers 1+ when needed.
