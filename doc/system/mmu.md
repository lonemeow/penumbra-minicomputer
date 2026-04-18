# Penumbra MMU

## Overview

The Penumbra MMU provides page-based virtual-to-physical address translation with per-page protection and cacheability control. It is designed around a **fully software-managed TLB**: the hardware performs fast parallel lookups and permission checks, but all management — loading entries, choosing replacement victims, invalidation, dirty tracking — is done by the OS via privileged `WRSYS`/`RDSYS` instructions.

### Quick Reference

| Property | Value |
|----------|-------|
| Page size | 4 KB (12-bit offset) |
| Main TLB | 64 slots (0–63), 2-way set-associative; each virtual page maps to exactly 2 candidate slots |
| Pinned TLB | 4 slots, fully associative; checked in parallel, pinned hit wins |
| TLB entry width | Two 32-bit sysreg words: TLB_VPN and TLB_PTE |
| Exception vectors | VEC_TLB_MISS=2 (0x08), VEC_TLB_PROT=3 (0x0C) |
| Sysreg device ID | 0 |
| Flat/bypass mode | M=0 (reset default): identity map, uncached, no checks |

---

## MMU Control Registers

Accessed via `WRSYS`/`RDSYS` with device ID 0.

| Reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | MMUCR | R/W | Control: M (enable), ASID (current address space) |
| 1 | FAULT_ADDR | R | Faulting virtual address (latched by hardware on fault) |
| 2 | FAULT_STATUS | R | Fault reason and access info |
| 3 | TLB_VPN | R/W | TLB entry upper word: VPN + ASID |
| 4 | TLB_PTE | R/W | TLB entry lower word: PPN + flags. **Write commits entry.** |
| 5 | TLB_INDEX | R/W | TLB slot selector; **bit 6 selects pinned TLB** |

### MMUCR Layout

```
 15        8 7         1 0
┌──────────┬───────────┬─┐
│ ASID (8) │(reserved) │M│
└──────────┴───────────┴─┘
```

- **M** (bit 0): 0 = flat/bypass mode (identity map). 1 = TLB active.
- **ASID** (bits 15:8): Current address space ID for TLB matching.

### FAULT_STATUS Layout

```
 11 10  9  8 7    4 3       0
┌───┬──┬──┬──┬─────┬─────────┐
│USR│ X│ W│ R│(gap)│  TYPE   │
└───┴──┴──┴──┴─────┴─────────┘
```

- **TYPE** (bits 3:0): `0001` = TLB miss, `0010` = protection violation.
- **R/W/X** (bits 8/9/10): Which access type faulted (one-hot).
- **USR** (bit 11): 1 = fault occurred in user mode.

---

## TLB Entry Format

**TLB_VPN** (reg 3) — `(VPN << 8) | ASID`:
```
 27                8 7        0
┌──────────────────┬──────────┐
│    VPN (20)      │ ASID (8) │
└──────────────────┴──────────┘
```

**TLB_PTE** (reg 4) — `(PPN << 12) | (SW << 8) | flags`:
```
 31              12 11    8 7 6 5 4 3 2 1 0
┌─────────────────┬───────┬─┬─┬─┬─┬─┬─┬─┬─┐
│    PPN (20)     │ SW(4) │G│U│X│W│R│C│—│V│
└─────────────────┴───────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

### Fields

| Field | Register | Bits | Hex | Description |
|-------|----------|------|-----|-------------|
| VPN | TLB_VPN | 27:8 | — | Virtual page number. Matched against `vaddr[31:12]`. |
| ASID | TLB_VPN | 7:0 | — | Address space ID. |
| PPN | TLB_PTE | 31:12 | — | Physical page number. |
| SW | TLB_PTE | 11:8 | — | Software-defined (4 bits). Hardware ignores. |
| G | TLB_PTE | 7 | 0x80 | Global — skip ASID match. |
| U | TLB_PTE | 6 | 0x40 | User-accessible. |
| X | TLB_PTE | 5 | 0x20 | Execute permission. |
| W | TLB_PTE | 4 | 0x10 | Write permission. |
| R | TLB_PTE | 3 | 0x08 | Read permission. |
| C | TLB_PTE | 2 | 0x04 | Cacheable. |
| V | TLB_PTE | 0 | 0x01 | Valid. |

---

## TLB Operations

### TLB_INDEX

```
 6   5    4       0
┌─────┬──────────┐
│ way │   set    │
└─────┴──────────┘
```

**Constraint:** `set = vaddr[16:12]` (the low 5 bits of the VPN). Each virtual page can live in exactly two possible slots: `VPN & 0x1F` (way 0) or `(VPN & 0x1F) | 0x20` (way 1).

### Pinned TLB

The pinned TLB is a 4-entry fully-associative structure. A pinned hit takes priority. Use for miss handler code, page directory base, etc.

Accessed via `TLB_INDEX` with **bit 6** set; bits 1:0 select the slot (0–3).

---

## Software-Managed Dirty Tracking

The OS tracks dirty pages using the **write-fault mechanism**:
1. Map page with W=0.
2. Hardware raises protection fault on write.
3. Handler marks page dirty in bookkeeping and reloads with W=1.
