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

TLB hardware structure (a set-associative main TLB beside a fully
associative pinned one) and 74xx feasibility live in
[internals/mmu-internals.md](../internals/mmu-internals.md).

### Quick Reference

| Property | Value |
|----------|-------|
| Page size | 4 KB (12-bit offset) |
| Main TLB | Set-associative; each VPN maps to one candidate slot in each way. Baseline: 32 sets × 2 ways, 64 slots |
| Pinned TLB | Fully associative; pinned hit wins over main TLB hit. Baseline: 8 slots |
| TLB entry width | Two 32-bit sysreg words: `TLB_VPN` and `TLB_PTE` |
| Exception vectors | `VEC_TLB_MISS` = 2 (0x08), `VEC_TLB_PROT` = 3 (0x0C) |
| Sysreg device ID | 0 |
| Interface version | 1 or 2 ([Interface Versions](#interface-versions)) |
| Flat/bypass mode | `M=0` (reset default): identity map, uncached, no checks |

The interface version, the page size and the two TLB geometries are
what [`MMU_CONFIG`](#mmu_config-layout) reports; software reads them
from there rather than assuming them. The baseline is what an MMU
that reports nothing describes.

---

## MMU Control Registers

Accessed via `WRSYS`/`RDSYS` with device ID 0.

| Reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | `MMUCR` | R/W | Control: M (enable), ASID (current address space) |
| 1 | `FAULT_ADDR` | R | Faulting virtual address (latched on fault); physical if deferred |
| 2 | `FAULT_STATUS` | R | Fault reason and access info (latched on fault) |
| 3 | `TLB_VPN` | R/W | TLB entry upper word: VPN + ASID (staged on write) |
| 4 | `TLB_PTE` | R/W | TLB entry lower word: PPN + flags. **Write commits entry.** |
| 5 | `TLB_INDEX` | R/W | TLB slot selector; **bit 6 (version 1) or bit 31 (version 2) selects the pinned TLB** |
| 6 | `TLB_INVAL_VA` | W | Version 2: invalidate the main-TLB entries an address hits |
| 7 | `TLB_INVAL_ASID` | W | Version 2: invalidate an ASID's non-global main-TLB entries |
| 8 | `TLB_INVAL_ALL` | W | Version 2: invalidate the whole main TLB |
| 9–14 | — | — | Reserved |
| 15 | `MMU_CONFIG` | R | Interface version and translation geometry |

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

- **TYPE** (bits 3:0): `0001` = TLB miss, `0010` = protection
  violation, `0011` = alignment, `0100` = bus fault, `0101` = deferred
  bus fault ([Deferred Bus Faults](./architecture.md#deferred-bus-faults)).
- **R/W/X** (bits 8/9/10): Access type that faulted (one-hot).
- **USR** (bit 11): 1 = the faulting access ran in user mode. A
  deferred bus fault does not report its write's mode and reads 0.

### MMU_CONFIG Layout

Reports the interface version and the translation geometry software
would otherwise have to assume: how many pinned slots it may spend,
how many main-TLB slots it must walk to enumerate resident entries,
the page size its page tables describe, and the width of the ASID it
allocates.

```
 31 30 29    25 24      20 19    15 14    10 9       4 3    0
┌─────┬────────┬──────────┬────────┬────────┬─────────┬──────┐
│rsvd │  ASID  │PAGE_LOG2 │  WAYS  │SETS_L2 │ PINNED  │ VER  │
└─────┴────────┴──────────┴────────┴────────┴─────────┴──────┘
```

- **VER** (bits 3:0): Interface version, 1 or 2
  ([Interface Versions](#interface-versions)). Numbering starts at 1,
  so an implemented register never reads as zero.
- **PINNED** (bits 9:4): Number of pinned TLB slots. A count, not an
  exponent — the pinned TLB is fully associative and need not be a
  power of two.
- **SETS_L2** (bits 14:10): Base-2 logarithm of the main TLB's set
  count. The set index is taken from the VPN, so the count is always a
  power of two.
- **WAYS** (bits 19:15): Main TLB associativity, as a count; not
  necessarily a power of two.
- **PAGE_LOG2** (bits 24:20): Base-2 logarithm of the page size in
  bytes.
- **ASID** (bits 29:25): Width in bits of the ASID field in `MMUCR`
  and `TLB_VPN`.

#### Interface Versions

`VER` scopes the whole translation interface, not just the fields
beside it: it says how `TLB_INDEX` selects a slot and which registers
the MMU implements, and so which geometries the MMU can report.
`MMUCR`, `TLB_VPN` and `TLB_PTE` are the same in both versions, and
so are the 4 KB page and an ASID of at most 8 bits.

| | Version 1 | Version 2 |
|---|---|---|
| `TLB_INDEX` pinned select | bit 6 | bit 31 |
| `TLB_INDEX` slot number | bits 5:0 | bits 30:0 |
| Registers 6–8 | reserved | the [invalidation registers](#invalidation-registers-version-2) |
| `PINNED` | at most 8 | at most 63 |
| `SETS_L2` | at most 5 | at most 20 |
| `WAYS` | at most 2 | at most 31 |
| `PAGE_LOG2` | 12 | 12 |
| `ASID` | at most 8 | at most 8 |

A version reaches every geometry within its bounds. Version 1's are
what its `TLB_INDEX` can address: 32 sets of 2 ways and 8 pinned
slots. In version 2 the pinned count and the ways are bounded only by
their `MMU_CONFIG` fields and the set count by the 20-bit VPN its
index is taken from, and the largest main slot number,
`SETS × WAYS − 1`, stays within `TLB_INDEX`'s 31 bits. A geometry past
version 1's bounds is therefore only ever reported under version 2,
and a version-2 MMU may report any geometry within its bounds,
including one version 1 could also describe.

Software that does not know the version it reads cannot form a
`TLB_INDEX` value or tell which registers exist, and must not drive
the MMU.

**A read of zero reports version 1 with the configuration below.**
Reserved MMU registers read as zero, so an implementation that omits
this register describes itself correctly by doing nothing, and
software needs no separate presence test — it substitutes these
values whenever the register reads zero.

| Field | Value | Meaning |
|-------|------:|---------|
| `VER` | 1 | — |
| `PINNED` | 8 | 8 pinned slots |
| `SETS_L2` | 5 | 32 sets |
| `WAYS` | 2 | 2-way set associative |
| `PAGE_LOG2` | 12 | 4 KB pages |
| `ASID` | 8 | 8-bit ASID |

An implementation whose geometry matches that table may report either
zero or the equivalent packed value `0x10C11481`; software treats them
as the same. A version-2 MMU reports its value in full: 256 sets of 2
ways with the table's other fields, for example, read `0x10C12082`.

Five pinned slots are the floor for a two-level walk that keeps the
handler, both page-table roots and a scratch window resident — see
[Pinned-Slot Assignment](#pinned-slot-assignment). An implementation
reporting fewer cannot run that translation scheme.

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
| C    | TLB_PTE  | 2     | 0x04 | Cacheable. 0 = bypass cache (MMIO). |
| —    | TLB_PTE  | 1     | 0x02 | Reserved; software writes 0. Tentatively earmarked for a future *speculatable* attribute — see [Reserved bits](#reserved-bits). |
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

### TLB_INDEX

Selects the slot `TLB_VPN` and `TLB_PTE` read and write. A main-TLB
slot is numbered `way × SETS + set`, where `set` is the VPN's low
`SETS_L2` bits and `way` runs from 0 to `WAYS − 1`, so the slot
numbers run from 0 to `SETS × WAYS − 1`. A virtual page can live in
exactly `WAYS` slots, one in each way:

```
slot(way) = way × SETS + (VPN mod SETS)
```

The OS chooses which of them to use for replacement.

The pinned TLB's slots are numbered from 0 to `PINNED − 1`. One bit of
`TLB_INDEX`, P, selects the pinned TLB, at the position the
[interface version](#interface-versions) gives it:

Version 1:

```
 31                         7   6   5            0
┌──────────────────────────┬───┬───────────────┐
│          (zero)          │ P │     slot      │
└──────────────────────────┴───┴───────────────┘
```

Version 2:

```
 31 30                                          0
┌───┬─────────────────────────────────────────────┐
│ P │                    slot                     │
└───┴─────────────────────────────────────────────┘
```

| Selects | Version 1 | Version 2 |
|---------|-----------|-----------|
| Main slot `n` | `n` (`0x00`–`0x3F`) | `n` |
| Pinned slot `n` | `0x40 + n` (`0x40`–`0x47`) | `0x8000_0000 + n` |

With the baseline's 32 sets, a main slot number carries the set in
bits 4:0 and the way in bit 5, so version 1's two ways of set `S` are
slots `S` and `S | 0x20`.

In version 2, a slot number at or past its TLB's slot count selects no
entry: a `TLB_PTE` write to it changes nothing, and `TLB_VPN` and
`TLB_PTE` read zero there.

### Pinned TLB

A fully associative structure of `PINNED` slots, checked in parallel
with the main TLB. A pinned hit takes priority. No set constraint —
any VPN can go in any pinned slot.

Use for entries that must **never** cause TLB misses: the miss-handler
code page, the page directory base, and optionally the kernel stack.

### Loading an Entry

Three `WRSYS` instructions, executed **in order**:

```asm
; R1 = TLB_INDEX value (a main slot, or a pinned one with P set)
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

For replacement decisions, read each way of the set:

```asm
; Read set S, way by way: way w is slot w * SETS + S
LLI   R1, S             ; way 0
WRSYS R1, #0, #5
RDSYS R2, #0, #3        ; R2 = TLB_VPN
RDSYS R3, #0, #4        ; R3 = TLB_PTE

ADD   R1, R6            ; way 1 (R6 = SETS)
WRSYS R1, #0, #5
RDSYS R4, #0, #3
RDSYS R5, #0, #4
```

Check `V` (bit 0 of `TLB_PTE`) to determine which slots are occupied.
Use `SW` bits for replacement policy state (LRU, age, etc.).

### Invalidating Entries

Every version invalidates a slot by writing `TLB_PTE = 0` to it:

```asm
WRSYS  R_idx, #0, #5    ; select slot
WRSYS  R0,    #0, #3    ; clear VPN (optional)
WRSYS  R0,    #0, #4    ; TLB_PTE = 0 → V=0, entry invalidated
```

The write commits whatever `TLB_VPN` was last staged, so a slot
invalidated this way can keep any VPN word, that of a live entry in
another way of its set included. A search by VPN therefore checks `V`
before it trusts a match.

**Version 1** offers only that. A full flush writes `TLB_PTE = 0` to
every main slot:

```asm
LLI   R1, #0
LLI   R2, #(SETS * WAYS)
.flush_loop:
WRSYS R1, #0, #5        ; select slot
WRSYS R0, #0, #4        ; V=0
INC   R1, #1
CMP   R1, R2
BNE   .flush_loop
```

Invalidating by virtual address (e.g., on `munmap`) reads the VPN's
slots, one in each way, and invalidates the valid one whose `TLB_VPN`
matches; invalidating an address space reads every slot.

**Version 2** does the search in the MMU, through its
[invalidation registers](#invalidation-registers-version-2).

#### Invalidation Registers (version 2)

Three write-only registers invalidate main-TLB entries by what they
map:

| Reg | Name | Value written | Invalidates |
|-----|------|---------------|-------------|
| 6 | `TLB_INVAL_VA` | a `TLB_VPN` word, `(VPN << 8) \| ASID` | every valid entry a translation of the VPN would hit with that ASID in `MMUCR`: the VPN equal, and the ASID equal or `G=1` |
| 7 | `TLB_INVAL_ASID` | an ASID, in bits 7:0 | every valid entry with `G=0` and that ASID |
| 8 | `TLB_INVAL_ALL` | ignored | every entry |

- **The main TLB only.** The pinned TLB is managed by slot and never
  matched.
- **Valid entries only,** so a stale VPN word left in an invalidated
  slot never matches.
- **No other state changes:** `TLB_INDEX` and the staged `TLB_VPN`
  keep their values.
- **The write blocks.** It completes when the invalidation has, so the
  instruction after it, and the fetch of that instruction, see the
  TLB without the matched entries. There is no status to poll. An
  interrupt waits for the write as it would for any instruction.
- **The cost depends on the reach.** `TLB_INVAL_VA` examines one set
  and is cheap. `TLB_INVAL_ASID` and `TLB_INVAL_ALL` examine every
  set, so their cost grows with `SETS`, and on a large TLB they are
  expensive. A kernel retiring an address space can skip the sweep
  instead: if no new address space gets that ASID before the flush
  taken when ASIDs run out, its entries are never hit again.

Reads return zero. In version 1, registers 6–8 are reserved.

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
; set = VPN mod SETS. Read the set's ways, pick a victim.

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

> **Note.** Keep handler scratch out of low physical RAM:
> `0x0000_0000`–`0x0000_003F` is the exception vector table, and
> which of the rest is free only the boot data says (see
> [boot-protocol.md: ROM to Loaded
> Program](./boot-protocol.md#rom-to-loaded-program)). The pinned
> vector page gives you a clean, guaranteed-resident workspace
> instead.

### Pinned-Slot Assignment

The miss handler must not cause recursive TLB misses. Critical entries
live in the pinned TLB:

| Pinned slot | Maps                        | Purpose                             |
|:-----------:|-----------------------------|-------------------------------------|
| 0           | Pinned vector page          | Miss-handler code, scratch save area, PD pointer |
| 1           | Kernel page directory       | First-level walk for kernel VAs (pinned once at boot, never changes) |
| 2           | L2 page-table window        | Scratch window for walking L2 pages |
| 3           | Kernel stack / reserved     | Port-specific                       |
| 4           | Current user L1 table       | First-level walk for user VAs (re-pinned on context switch; NetBSD's `PTLB_USER_L1`) |
| 5–7         | Free                        | Port-specific                       |

The pinned vector page is mapped at a port-chosen virtual address
(the NetBSD port uses `0xFFFFB000`); it is **not** at virtual 0, and
it is distinct from the physical vector table at `0x00000000` that the
hardware reads during exception dispatch.

Pinned entries do not consume main-TLB slots — the whole main TLB
remains available for demand-loaded mappings.

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
2. Flush the main TLB (`TLB_INVAL_ALL` in version 2; in version 1, a
   software loop over every slot).
3. Stash the new PD base pointer on the pinned vector page (the
   miss handler reads it from there).
4. Re-pin the incoming process's user page-table root (slot 4 in
   the NetBSD layout). The kernel PD pin (slot 1) never changes.
5. Restore incoming process state.
6. `ERET` — first fetches TLB-miss and are loaded on demand.

### With ASID (current)

1. Save outgoing process state.
2. Set `MMUCR.ASID` to the incoming process's ASID.
3. Write the new PD base pointer; re-pin the user page-table root
   (slot 4 in the NetBSD layout — the kernel PD pin is permanent).
4. Restore incoming state; `ERET`.

No TLB flush needed — entries from different ASIDs coexist. Kernel
pages with `G=1` are shared across all ASIDs. When every ASID is in
use (256 of them with an 8-bit ASID), flush the TLB and restart
allocation (generational ASID recycling).

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
the L1/L2 caches. Kernel and user PDs are completely disjoint: the
TLB miss walker branches on the faulting VA's MSB and uses either
the kernel PD (pinned, permanent) or the current user PD (pinned,
re-programmed per `pmap_activate`). User PDs hold *only* user-VA
entries — no kernel-half mirroring, no propagation needed.

---

## Boot Sequence

After reset:

- `SR = { S=1, I=0, flags=0 }` — supervisor mode, interrupts disabled.
- `MMUCR = { M=0, ASID=0 }` — flat/bypass mode.
- `PC = 0xFFFF_0000` (boot ROM).
- The TLB's contents, main and pinned, are undefined. With `M=0`
  nothing consults them, so software initializes the TLB before it
  sets `M=1`.

Recommended flow:

1. Execute boot ROM in flat mode (`M=0`) — all addresses are physical, uncached.
2. Initialize memory (SDRAM on FPGA, SRAM in discrete).
3. Load the kernel from storage into RAM.
4. Set up the interrupt vector table at physical `0x00000000`.
   (The boot data lies wherever R1 points, possibly right beside the
   vector table — don't overwrite it until it has been consumed; see
   [boot-protocol.md: ROM to Loaded
   Program](./boot-protocol.md#rom-to-loaded-program).)
5. Build initial page tables in RAM.
6. Allocate a pinned vector page containing the TLB miss handler code,
   scratch save area, and PD pointer slot. Populate the PD pointer.
7. Initialize the TLB: invalidate every main slot (`TLB_INVAL_ALL` in
   version 2, a [slot loop](#invalidating-entries) in version 1) and
   write `TLB_PTE = 0` to every pinned slot. A pinned hit wins over
   everything, so an entry left in an unused pinned slot — from reset,
   or from whatever ran before — would capture its addresses.
8. Install pinned TLB entries: vector page (slot 0), PGD (slot 1).
9. Set `M=1` via `WRSYS` to enable translation — the miss handler
   resolves the first translated fetch.
10. Jump to the kernel virtual entry point.

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
; Boot code running at physical address (M=0), the TLB initialized
; (Boot Sequence, step 7).
; Port-defined constants:
;   PIN_SEL    — TLB_INDEX's pinned select for the MMU's version
;                (0x40 in version 1, 0x8000_0000 in version 2)
;   VECT_VPN   — VPN of the pinned vector page in kernel virtual space
;   VECT_PPN   — PPN where the handler code actually lives in RAM
;   PGD_VPN / PGD_PPN — same, for the page directory

; Step 1: Pin the vector page in pinned slot 0
LI    R1, #(PIN_SEL | 0)       ; TLB_INDEX: pinned slot 0
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
4. The TLB must be initialized first: every main slot invalidated and
   every pinned slot written, `V=0` in the ones not in use. Its
   contents are undefined after reset.

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

MMIO regions must bypass the cache. Set `C=0` in the TLB entry for
these pages. Penumbra's L1 caches are VIPT with each way at most one
page, so the cache index runs in parallel with TLB translation and the
physical tag compare follows it.

`C=0` sends a missing access to memory as a single access and never
installs a line, but a cache need not check `C` on a hit: an access
through a `C=0` mapping may hit a line left from a time the same
physical page was mapped with `C=1`. Before changing a physical page's
mapping from cacheable to uncacheable, software therefore invalidates
every cache that may hold its lines (`INVAL_ALL`, after `FLUSH_ALL` on
a write-back cache). Device pages are never mapped cacheable, so they
never meet this.

### Reserved bits

`TLB_PTE` bit 1 (`0x02`) is reserved; software writes 0. It is
tentatively earmarked — not yet finalized — for a *speculatable* (or
idempotent) attribute: a future control by which an uncacheable page
(`C=0`) could advertise that its accesses are side-effect-free, so a
core that issues memory accesses speculatively may treat them as it
would cacheable memory. The motivating case is the boot ROM, which is
uncacheable yet idempotent. The encoding is held here so it is not spent
elsewhere; its exact semantics are left open until the speculation model
is settled.

### I-Cache Coherence

After loading new executable code (e.g., `exec()`), the kernel must
invalidate the I-cache to prevent stale instruction fetches. Sequence:
load TLB entries for the new code pages, then write `DCACHE.INVAL_ALL`
(for modified data) followed by `ICACHE.INVAL_ALL`. See
[sysregs.md](./sysregs.md#cache-devices-2--dcache-3--icache-9--l2).

### Coherence With Other Hardware

There is no hardware cache snooping. Memory that hardware other than
the CPU reads or modifies must be mapped with `C=0` (uncached), or the
kernel must invalidate the D-cache around that hardware's accesses.
The pragmatic rule: use uncached mappings for such memory and avoid
the problem entirely.
