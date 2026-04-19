# Penumbra MMU — System Programmer's Reference

## Overview

The Penumbra MMU provides page-based virtual-to-physical address
translation with per-page protection and cacheability control. It is
designed around a **fully software-managed TLB**: the hardware performs
fast parallel lookups and permission checks, but all management —
loading entries, choosing replacement victims, invalidation, dirty
tracking — is done by the OS via privileged `WRSYS`/`RDSYS`
instructions.

This means:

- The hardware has **no page-table walker**, no replacement policy,
  and no dirty-bit logic.
- The OS has **complete control** over what's in the TLB and when
  entries change.
- The in-memory page table format is an **OS convention**, not a
  hardware requirement.

TLB hardware structure (2-way SA main + fully-associative pinned) and
74xx feasibility live in
[internals/mmu-internals.md](../internals/mmu-internals.md).

### Quick Reference

| Property | Value |
|----------|-------|
| Page size | 4 KB (12-bit offset) |
| Main TLB | 64 slots (0–63), 2-way set-associative; each VPN maps to exactly 2 candidate slots |
| Pinned TLB | 4 slots, fully associative; pinned hit wins over main TLB hit |
| TLB entry width | Two 32-bit sysreg words: `TLB_VPN` and `TLB_PTE` |
| Exception vectors | `VEC_TLB_MISS` = 2 (0x08), `VEC_TLB_PROT` = 3 (0x0C) |
| Sysreg device ID | 0 |
| Flat/bypass mode | `M=0` (reset default): identity map, uncached, no checks |

---

## MMU Control Registers

Accessed via `WRSYS`/`RDSYS` with device ID 0.

| Reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | `MMUCR` | R/W | Control: M (enable), ASID (current address space) |
| 1 | `FAULT_ADDR` | R | Faulting virtual address (latched on fault) |
| 2 | `FAULT_STATUS` | R | Fault reason and access info (latched on fault) |
| 3 | `TLB_VPN` | R/W | TLB entry upper word: VPN + ASID (staged on write) |
| 4 | `TLB_PTE` | R/W | TLB entry lower word: PPN + flags. **Write commits entry.** |
| 5 | `TLB_INDEX` | R/W | TLB slot selector; **bit 6 selects pinned TLB** |
| 6–15 | — | — | Reserved |

### MMUCR Layout

```
 31              16 15        8 7         1 0
┌──────────────────┬──────────┬───────────┬─┐
│    (reserved)    │ ASID (8) │(reserved) │M│
└──────────────────┴──────────┴───────────┴─┘
```

- **M** (bit 0): 0 = flat/bypass mode (identity map, uncached, no checks). 1 = TLB active.
- **ASID** (bits 15:8): Current address space ID. TLB entries match against this unless `G=1`.

### FAULT_STATUS Layout

```
 31    12 11 10  9  8 7    4 3       0
┌────────┬───┬──┬──┬──┬─────┬─────────┐
│(rsvd)  │USR│ X│ W│ R│(gap)│  TYPE   │
└────────┴───┴──┴──┴──┴─────┴─────────┘
```

- **TYPE** (bits 3:0): `0001` = TLB miss, `0010` = protection violation.
- **R/W/X** (bits 8/9/10): Access type that faulted (one-hot).
- **USR** (bit 11): 1 = fault occurred in user mode.

---

## TLB Entry Format

**TLB_VPN** (reg 3) — `(VPN << 8) | ASID`:

```
 31  28 27                8 7        0
┌──────┬──────────────────┬──────────┐
│ 0000 │    VPN (20)      │ ASID (8) │
└──────┴──────────────────┴──────────┘
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
| VPN  | TLB_VPN  | 27:8  | —    | Virtual page number. Matched against `vaddr[31:12]`. |
| ASID | TLB_VPN  | 7:0   | —    | Address space ID. Matched against `MMUCR.ASID` unless G=1. |
| PPN  | TLB_PTE  | 31:12 | —    | Physical page number. Combined with page offset. |
| SW   | TLB_PTE  | 11:8  | —    | Software-defined (4 bits). Hardware stores but never reads. |
| G    | TLB_PTE  | 7     | 0x80 | Global — skip ASID match. Use for kernel pages shared across ASIDs. |
| U    | TLB_PTE  | 6     | 0x40 | User-accessible. Supervisor always bypasses this check. |
| X    | TLB_PTE  | 5     | 0x20 | Execute permission. |
| W    | TLB_PTE  | 4     | 0x10 | Write permission. |
| R    | TLB_PTE  | 3     | 0x08 | Read permission. |
| C    | TLB_PTE  | 2     | 0x04 | Cacheable. 0 = bypass cache (MMIO, DMA buffers). |
| V    | TLB_PTE  | 0     | 0x01 | Valid. Entry participates in lookup only when `V=1`. |

### Permission Check Rules

On a TLB hit, the hardware checks:

- **Read** → requires `R=1`
- **Write** → requires `W=1`
- **Execute** → requires `X=1`
- **User mode** → additionally requires `U=1`
- **Supervisor mode** → bypasses `U` check (can access `U=0` pages)

If any check fails: protection fault (vector 3, `FAULT_STATUS.TYPE = 0010`).

---

## TLB Operations

### TLB_INDEX (Main TLB)

```
 31                       6   5    4       0
┌─────────────────────────┬─────┬──────────┐
│        (zero)           │ way │   set    │
└─────────────────────────┴─────┴──────────┘
```

**Set constraint:** `set = vaddr[16:12]` (the low 5 bits of the VPN).
Any virtual page can live in exactly two slots:

```
slot_a = VPN & 0x1F         (way 0)
slot_b = (VPN & 0x1F) | 0x20 (way 1)
```

The OS chooses which of the two to use for replacement.

### Pinned TLB (bit 6 of TLB_INDEX)

A 4-entry fully-associative structure checked in parallel with the main
TLB. A pinned hit takes priority. No set constraint — any VPN can go in
any pinned slot.

Use for entries that must **never** cause TLB misses: the miss-handler
code page, the page directory base, and optionally the kernel stack.

| `TLB_INDEX` | Selects |
|-------------|---------|
| `0x00`–`0x3F` | Main TLB slot 0–63 (bit 6 = 0) |
| `0x40`–`0x43` | Pinned slot 0–3 (bit 6 = 1, bits 1:0 select slot) |

### Loading an Entry

Three `WRSYS` instructions, executed **in order**:

```asm
; R1 = TLB_INDEX value (main slot, or pinned with bit 6 set)
; R2 = TLB_VPN value  ({VPN[19:0], ASID[7:0]})
; R3 = TLB_PTE value  ({PPN[19:0], SW[3:0], flags[7:0]})

WRSYS  R1, #0, #5       ; select target slot
WRSYS  R2, #0, #3       ; stage VPN + ASID in holding register
WRSYS  R3, #0, #4       ; write PPN + flags → entry committed
```

**Important:** The `TLB_VPN` write stages data in a holding register.
The entry is committed to the TLB array **only** when `TLB_PTE` is
written. Always write `TLB_INDEX` before `TLB_VPN`/`TLB_PTE`.

### Reading an Entry

For replacement decisions, read both ways of a set:

```asm
; Read way 0 of set S
LLI   R1, S             ; set index (bits 4:0), way=0 (bit 5 clear)
WRSYS R1, #0, #5
RDSYS R2, #0, #3        ; R2 = TLB_VPN
RDSYS R3, #0, #4        ; R3 = TLB_PTE

; Read way 1 of set S
LLI   R1, (S | 0x20)
WRSYS R1, #0, #5
RDSYS R4, #0, #3
RDSYS R5, #0, #4
```

Check `V` (bit 0 of `TLB_PTE`) to determine which slots are occupied.
Use `SW` bits for replacement policy state (LRU, age, etc.).

### Invalidating Entries

**Single entry** (by slot):

```asm
WRSYS  R_idx, #0, #5    ; select slot
WRSYS  R0,    #0, #3    ; clear VPN (optional)
WRSYS  R0,    #0, #4    ; TLB_PTE = 0 → V=0, entry invalidated
```

**Full flush** (all 64 main-TLB slots):

```asm
LLI   R1, #0
LLI   R2, #64
.flush_loop:
WRSYS R1, #0, #5        ; select slot
WRSYS R0, #0, #4        ; V=0
INC   R1, #1
CMP   R1, R2
BNE   .flush_loop
```

**By virtual address** (e.g., on `munmap`): compute `set = vaddr[16:12]`,
read both ways, compare the VPN field, invalidate the matching slot.

---

## Exception Handling

### TLB Miss Handler

Entered via **vector 2** (`VEC_TLB_MISS`, physical 0x08) when no matching
entry is found. Protection faults use **vector 3** (`VEC_TLB_PROT`, 0x0C).
On entry, hardware has already saved `EPC`/`ESR`, entered supervisor mode,
and disabled interrupts. Vector table entries are fetched with MMU
bypassed, so no TLB mapping is required for the vector page itself.

The handler itself, however, runs with the MMU on. To avoid recursive
TLB misses, the handler code and its scratch save area live on a
**pinned vector page** — a page mapped via the pinned TLB so it's
always resolvable. Offsets below (`VECT_SCRATCH_R1`, `VECT_PDBASE`,
etc.) are symbolic locations on that pinned page, not absolute
physical addresses.

```asm
; ── Vector 2 entry point ──────────────────────────
; Hardware state: EPC/ESR saved, S=1, I=0
;
; Rv points to the base of the pinned vector page. The handler
; is linked to know that base (or sets it up from a known pinned VA).

STW   R1, [Rv + #VECT_SCRATCH_R1]
STW   R2, [Rv + #VECT_SCRATCH_R2]
STW   R3, [Rv + #VECT_SCRATCH_R3]
STW   R4, [Rv + #VECT_SCRATCH_R4]

RDSYS R1, #0, #1                   ; R1 = FAULT_ADDR
RDSYS R2, #0, #2                   ; R2 = FAULT_STATUS

; ── Page table walk ───────────────────────────────
; Compute PD index = R1[31:22], PT index = R1[21:12]
; PD base stashed on the pinned page so it's always reachable
LDW   R3, [Rv + #VECT_PDBASE]
; ... walk PD → PT, load PTE into R_pte, build R_vpn ...

; ── Replacement decision ──────────────────────────
; set = VPN[4:0]. Read both ways, pick victim.

; ── Commit entry ──────────────────────────────────
WRSYS R_idx, #0, #5
WRSYS R_vpn, #0, #3
WRSYS R_pte, #0, #4

; ── Restore and return ────────────────────────────
LDW   R1, [Rv + #VECT_SCRATCH_R1]
LDW   R2, [Rv + #VECT_SCRATCH_R2]
LDW   R3, [Rv + #VECT_SCRATCH_R3]
LDW   R4, [Rv + #VECT_SCRATCH_R4]
ERET
```

> **Note.** Physical addresses in the first page of RAM (`0x0000_0000`
> and up) are occupied by the exception vector table and the boot
> data tagged list that the ROM hands to the loader (see
> [boot-protocol.md](./boot-protocol.md)). Don't reuse those physical
> offsets for handler scratch — the pinned vector page gives you a
> clean, guaranteed-resident workspace instead.

### Pinned-Slot Assignment

The miss handler must not cause recursive TLB misses. Critical entries
live in the pinned TLB:

| Pinned slot | Maps                        | Purpose                             |
|:-----------:|-----------------------------|-------------------------------------|
| 0           | Pinned vector page          | Miss-handler code, scratch save area, PD pointer |
| 1           | Page global directory       | First-level walk (updated on context switch) |
| 2           | L2 page-table window        | Scratch window for walking L2 pages |
| 3           | Kernel stack / reserved     | Port-specific                       |

The pinned vector page is mapped at a port-chosen virtual address
(the NetBSD port uses `0xFFFFB000`); it is **not** at virtual 0, and
it is distinct from the physical vector table at `0x00000000` that the
hardware reads during exception dispatch.

Pinned entries do not consume main-TLB slots — the full 64-slot main
TLB remains available for demand-loaded mappings.

---

## Software-Managed Dirty Tracking

There is no hardware dirty bit. The OS tracks dirty pages via the
**write-fault** mechanism:

1. **Initial load:** map the page with `W=0` even if logically writable.
2. **First write:** hardware raises a protection fault (`W=0`, access is write).
3. **Handler:** recognizes the fault (`TYPE=protection`, `W`-access set),
   marks the page dirty in kernel bookkeeping, and reloads the entry
   with `W=1`.
4. **Subsequent writes:** succeed normally.

The one-time fault cost per page is amortized over its lifetime. The
pageout daemon checks kernel bookkeeping — not a hardware bit — to
decide which pages need writing.

---

## Context Switch

### Without ASID (initial bring-up)

1. Save outgoing process state (registers, PC, SR).
2. Flush the main TLB (software loop, ~320 instructions).
3. Stash the new PD base pointer on the pinned vector page (the
   miss handler reads it from there).
4. Update pinned slot 1 (PGD) to point to the new page directory.
5. Restore incoming process state.
6. `ERET` — first fetches TLB-miss and are loaded on demand.

### With ASID (current)

1. Save outgoing process state.
2. Set `MMUCR.ASID` to the incoming process's ASID.
3. Write the new PD base pointer; update pinned slot 1.
4. Restore incoming state; `ERET`.

No TLB flush needed — entries from different ASIDs coexist. Kernel
pages with `G=1` are shared across all ASIDs. When all 256 ASIDs are
in use, flush the TLB and restart allocation (generational ASID
recycling).

---

## Page Table Format (OS Convention)

The hardware has no knowledge of in-memory page tables. The OS may use
any format. A natural 32-bit PTE layout that maps directly to `TLB_PTE`
(avoiding bit-shuffling in the miss handler) is:

```
 31          12 11   8 7 6 5 4 3 2 1 0
┌──────────────┬──────┬─┬─┬─┬─┬─┬─┬─┬─┐
│   PPN (20)   │SW(4) │G│U│X│W│R│C│—│V│
└──────────────┴──────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

With this layout, the miss handler loads the PTE directly into
`TLB_PTE` with no translation — a single `LDW` + `WRSYS` pair. The `SW`
bits carry OS metadata (dirty, referenced, wired, age) that rides
along into the TLB entry.

### Two-Level Page Table (NetBSD port)

```
Virtual address:  [PD index (10)][PT index (10)][Page offset (12)]
                   bits 31:22     bits 21:12     bits 11:0
```

- **Page directory (PD):** 1024 entries × 4 bytes = 4 KB. Each entry
  points to a page table page.
- **Page table (PT):** 1024 entries × 4 bytes = 4 KB. Each entry is a PTE.
- **Coverage:** one PD entry covers 4 MB; the full 4 GB space needs 1024 PDEs.

The NetBSD port uses `PT_L1_*`/`PT_L2_*` naming to avoid confusion with
the L1/L2 caches. The kernel half of the PD is shared across all user
pmaps (copied at `pmap_create`); kernel L2 pages are never freed, so
late-arriving kernel mappings propagate lazily via TLB miss.

---

## Boot Sequence

After reset:

- `SR = { S=1, I=0, flags=0 }` — supervisor mode, interrupts disabled.
- `MMUCR = { M=0, ASID=0 }` — flat/bypass mode.
- `PC = 0xFFFF_0000` (boot ROM).

Recommended flow:

1. Execute boot ROM in flat mode (`M=0`) — all addresses are physical, uncached.
2. Initialize memory (SDRAM on FPGA, SRAM in discrete).
3. Load the kernel from storage into RAM.
4. Set up the interrupt vector table at physical `0x00000000`.
   (The boot ROM also populates the boot-data tagged list at physical
   `0x00000040` — don't overwrite it until the kernel has consumed it;
   see [boot-protocol.md](./boot-protocol.md).)
5. Build initial page tables in RAM.
6. Allocate a pinned vector page containing the TLB miss handler code,
   scratch save area, and PD pointer slot. Populate the PD pointer.
7. Install pinned TLB entries: vector page (slot 0), PGD (slot 1).
8. Set `M=1` via `WRSYS` to enable translation — the miss handler
   resolves the first translated fetch.
9. Jump to the kernel virtual entry point.

### Enabling the MMU Safely

**This is critical.** When `WRSYS` sets `MMUCR.M=1`, the next instruction
fetch goes through the TLB. If that fetch address has no TLB entry, the
CPU takes a TLB miss — but the miss handler itself needs TLB entries to
run, creating an unrecoverable fault.

Two safe patterns:

- **Pattern A — Identity-map before enable.** Ensure the page containing
  the code that enables the MMU has a TLB entry where `VA == PA`. The
  first translated fetch returns the same physical address.

- **Pattern B — Rely on the miss handler.** If the pinned TLB contains
  the miss-handler page and PGD, the first post-enable fetch TLB-misses,
  the handler resolves it from the PD, and execution resumes. This
  avoids needing an identity map but requires the handler and PGD to be
  installed first.

```asm
; Boot code running at physical address (M=0).
; Port-defined constants:
;   VECT_VPN   — VPN of the pinned vector page in kernel virtual space
;   VECT_PPN   — PPN where the handler code actually lives in RAM
;   PGD_VPN / PGD_PPN — same, for the page directory

; Step 1: Pin the vector page in pinned slot 0
LLI   R1, #0x40                ; TLB_INDEX: bit 6 = pinned, slot 0
WRSYS R1, #0, #5
LI    R2, #((VECT_VPN << 8) | 0)  ; TLB_VPN: VPN + ASID=0
WRSYS R2, #0, #3
LI    R3, #((VECT_PPN << 12) | 0xBD)  ; TLB_PTE: PPN + V|C|R|W|X|G
WRSYS R3, #0, #4               ; vector page committed

; Step 2: Pin the PGD in pinned slot 1 (three more WRSYS, same pattern)

; Step 3: Write handler address into the vector table (physical 0x08)
; Step 4: Build the page directory with kernel mappings

; Step 5: Enable the MMU
LLI   R10, #0x0001             ; MMUCR: M=1, ASID=0
WRSYS R10, #0, #0              ; next fetch TLB-misses, handler resolves it

; Step 6: Jump to kernel virtual address
LA    R11, kernel_entry
JMP   R11
```

**Rules:**

1. The TLB miss handler code page **must** be pinned (or identity-mapped
   in the main TLB) before `M=1`.
2. The page directory must be accessible to the handler (pinned, or
   reachable from the handler's pinned page).
3. For Pattern A, the `WRSYS` and the next instruction must be on the
   same identity-mapped page.

### Disabling the MMU

Rare (warm reboot, crash dump) but symmetric: the code that clears
`M=1` must be at a virtual address equal to its physical address,
otherwise the next fetch — bypassing the TLB — will not reach the same
instruction stream.

### WRSYS Is Serializing

`WRSYS` to any sysreg is defined as serializing: all effects of the
write are visible before the next instruction fetch begins. In today's
non-pipelined design this is automatic. In a future pipelined
implementation `WRSYS` must drain the pipeline — any prefetched
instructions after the `WRSYS` must be discarded and refetched under
the new MMU/interrupt/privilege state. `WRSYS` is only used by kernel
code and is never performance-critical, so this conservative choice is
safe.

---

## Cache Considerations

### Uncached Access

MMIO and DMA regions must bypass the cache. Set `C=0` in the TLB entry
for these pages. Penumbra uses PIPT caches, so the `C` bit is available
from the TLB lookup before the cache is consulted — the cache checks
`C` and bypasses itself when `C=0`.

### I-Cache Coherence

After loading new executable code (e.g., `exec()`), the kernel must
invalidate the I-cache to prevent stale instruction fetches. Sequence:
load TLB entries for the new code pages, then write `DCACHE.INVAL` (for
modified data) followed by `ICACHE.INVAL`. See
[sysregs.md](./sysregs.md#devices-23-dcache--icache).

### DMA Coherence

There is no hardware cache snooping. DMA buffers must be mapped with
`C=0` (uncached), or the kernel must invalidate the D-cache around DMA
transfers. The pragmatic rule: use uncached mappings for DMA buffers
(or bounce buffers for user ↔ device transfers) and avoid the problem
entirely.
