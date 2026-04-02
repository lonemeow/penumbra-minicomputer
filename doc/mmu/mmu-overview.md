# Penumbra MMU

## Overview

The Penumbra MMU provides page-based virtual-to-physical address translation with per-page protection and cacheability control. It is designed around a **fully software-managed TLB**: the hardware performs fast parallel lookups and permission checks, but all management — loading entries, choosing replacement victims, invalidation, dirty tracking — is done by the OS via privileged `WRSYS`/`RDSYS` instructions.

This means:
- The hardware has **no page table walker**, no replacement policy, no dirty-bit logic
- The OS has **complete control** over what's in the TLB and when entries change
- The in-memory page table format is an OS convention, not a hardware requirement

### Quick Reference

| Property | Value |
|----------|-------|
| Page size | 4 KB (12-bit offset) |
| TLB slots | 64 (0–63); each virtual page maps to exactly 2 candidate slots |
| TLB entry width | Two 32-bit sysreg words: TLB_VPN and TLB_PTE |
| Exception vectors | VEC_TLB_MISS=2 (0x08), VEC_TLB_PROT=3 (0x0C) — separate vectors, also distinguished by FAULT_STATUS |
| Sysreg device ID | 0 |
| Flat/bypass mode | M=0 (reset default): identity map, uncached, no checks |

---

## MMU Control Registers

Accessed via `WRSYS`/`RDSYS` with device ID 0.

| Reg | Name | R/W | Description |
|-----|------|-----|-------------|
| 0 | MMUCR | R/W | Control: M (enable), ASID (current address space) |
| 1 | FAULT_ADDR | R | Faulting virtual address (latched by hardware on fault) |
| 2 | FAULT_STATUS | R | Fault reason and access info (latched by hardware on fault) |
| 3 | TLB_VPN | R/W | TLB entry upper word: VPN + ASID |
| 4 | TLB_PTE | R/W | TLB entry lower word: PPN + flags. **Write commits entry to TLB.** |
| 5 | TLB_INDEX | R/W | Target TLB slot for reads/writes |
| 6–15 | — | — | Reserved |

### MMUCR Layout

```
 31              16 15        8 7         1 0
┌──────────────────┬──────────┬───────────┬─┐
│    (reserved)    │ ASID (8) │(reserved) │M│
└──────────────────┴──────────┴───────────┴─┘
```

- **M** (bit 0): 0 = flat/bypass mode (identity map, uncached, no checks). 1 = TLB active.
- **ASID** (bits 15:8): Current address space ID. TLB entries match against this unless G=1.

### FAULT_STATUS Layout

```
 31    12 11 10  9  8 7    4 3       0
┌────────┬───┬──┬──┬──┬─────┬─────────┐
│(rsvd)  │USR│ X│ W│ R│(gap)│  TYPE   │
└────────┴───┴──┴──┴──┴─────┴─────────┘
```

- **TYPE** (bits 3:0): `0001` = TLB miss (no matching entry), `0010` = protection violation (entry found, access denied). Gap at bits 7:4 reserved for future fault types.
- **R/W/X** (bits 8/9/10): Which access type faulted (one-hot, same encoding as `ACC_READ`/`ACC_WRITE`/`ACC_EXEC`).
- **USR** (bit 11): 1 = fault occurred in user mode.

---

## TLB Entry Format

Each TLB entry is stored as two 32-bit words, read and written through the
TLB_VPN (reg 3) and TLB_PTE (reg 4) system registers. These are the values
the programmer constructs and the layouts that matter for OS code.

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
| VPN | TLB_VPN | 27:8 | — | Virtual page number. Matched against `vaddr[31:12]`. |
| ASID | TLB_VPN | 7:0 | — | Address space ID. Matched against MMUCR.ASID (unless G=1). |
| PPN | TLB_PTE | 31:12 | — | Physical page number. Combined with page offset for physical address. |
| SW | TLB_PTE | 11:8 | — | Software-defined (4 bits). Hardware stores but never reads. Use for dirty tracking, LRU, pinning. |
| G | TLB_PTE | 7 | 0x80 | Global — skip ASID match. Use for kernel pages shared across all address spaces. |
| U | TLB_PTE | 6 | 0x40 | User-accessible. 0 = supervisor only. Supervisor always bypasses this check. |
| X | TLB_PTE | 5 | 0x20 | Execute permission. Checked on instruction fetches. |
| W | TLB_PTE | 4 | 0x10 | Write permission. Checked on data stores. Set W=0 initially for dirty tracking (see below). |
| R | TLB_PTE | 3 | 0x08 | Read permission. Checked on data loads. |
| C | TLB_PTE | 2 | 0x04 | Cacheable. 0 = bypass cache (use for MMIO, DMA buffers). |
| V | TLB_PTE | 0 | 0x01 | Valid. Entry participates in lookup only when V=1. Clear to invalidate. |

### Permission Check Rules

On a TLB hit, the hardware checks (one-hot AND between access type and {X,W,R}):
- **Read** → requires R=1
- **Write** → requires W=1
- **Execute** → requires X=1
- **User mode** → additionally requires U=1
- **Supervisor mode** → bypasses U check (can access U=0 pages)

If any check fails: protection fault (vector 4, FAULT_STATUS.TYPE = `0010`).

---

## TLB Operations

### TLB_INDEX

The TLB has 64 slots, numbered 0–63. TLB_INDEX selects which slot to
read or write:

```
 31                       6   5    4       0
┌─────────────────────────┬─────┬──────────┐
│        (zero)           │ way │   set    │
└─────────────────────────┴─────┴──────────┘
```

Slots 0–31 are way 0, slots 32–63 are way 1 (same sets, second copy).

**Constraint:** The hardware lookup is wired so that a virtual address can
only match entries in one specific set: **`set = vaddr[16:12]`** (the low
5 bits of the VPN). Each set has two slots (way 0 and way 1), so any given
virtual page can live in exactly two possible slots:

```
slot_a = VPN & 0x1F            (way 0)
slot_b = (VPN & 0x1F) | 0x20  (way 1)
```

The OS chooses which of the two to use for replacement. The set/way
structure is invisible for sequential operations like a full flush
(just iterate 0–63).

### Loading an Entry

Three `WRSYS` instructions, executed in order:

```asm
; Load TLB entry: map vpage → ppage with given flags
; r1 = TLB_INDEX value ({way, set})
; r2 = TLB_VPN value  ({4'b0, VPN[19:0], ASID[7:0]})
; r3 = TLB_PTE value  ({PPN[19:0], SW[3:0], flags[7:0]})

WRSYS  R1, #0, #5       ; select target slot
WRSYS  R2, #0, #3       ; stage VPN + ASID
WRSYS  R3, #0, #4       ; write PPN + flags → entry committed
```

**Important:** The TLB_VPN write stages data in a holding register. The entry is committed to the TLB array only when TLB_PTE is written. Always write TLB_INDEX before TLB_VPN/TLB_PTE.

### Reading an Entry

For replacement decisions, read both ways of a set:

```asm
; Read way 0 of set S
LLI   R1, S             ; set index (bits 4:0), way=0 (bit 5 clear)
WRSYS R1, #0, #5        ; select slot
RDSYS R2, #0, #3        ; R2 = TLB_VPN (VPN + ASID)
RDSYS R3, #0, #4        ; R3 = TLB_PTE (PPN + SW + flags)

; Read way 1 of set S
LLI   R1, (S | 0x20)    ; same set, way=1 (bit 5 set)
WRSYS R1, #0, #5
RDSYS R4, #0, #3        ; R4 = way1 VPN
RDSYS R5, #0, #4        ; R5 = way1 PTE
```

Check V bit (bit 0 of TLB_PTE) to determine which slots are occupied. Use SW bits for replacement policy decisions.

### Invalidating Entries

**Single entry:**
```asm
WRSYS  R_idx, #0, #5    ; select slot
WRSYS  R0,    #0, #3    ; clear VPN (optional but clean)
WRSYS  R0,    #0, #4    ; write PTE with V=0 → entry invalidated
```

**All entries (full flush):**
```asm
; Iterate over all 64 slots (set 0-31, way 0-1)
LLI   R1, #0            ; index = 0
LLI   R2, #64           ; count
.flush_loop:
WRSYS R1, #0, #5        ; select slot
WRSYS R0, #0, #4        ; V=0 (R0 is always 0)
INC   R1, #1
CMP   R1, R2
BNE   .flush_loop
```

**By virtual address** (e.g., on `munmap`):
```asm
; Compute set from faulting VPN, check both ways, invalidate matches
; set = VPN[4:0] = vaddr[16:12]
; Check way 0 and way 1, compare VPN field, invalidate if match
```

---

## Exception Handling

### TLB Miss Handler

The TLB miss handler is entered via **exception vector 2** (VEC_TLB_MISS, physical address 0x08) when the hardware finds no matching entry. Protection faults use **vector 3** (VEC_TLB_PROT, 0x0C). The CPU has already saved EPC/ESR and entered supervisor mode with interrupts disabled. Vector table entries are fetched from physical addresses (MMU bypassed) so no TLB mapping is needed for the vector page.

**Recommended stackless handler** using a fixed save area in the pinned vector page (0x00000000):

```asm
; ── Vector 4 entry point ──────────────────────────
; Hardware state: EPC/ESR saved, S=1, I=0
; Save scratch registers to fixed area at 0x40-0x5F

STW   R1, [R0 + 0x40]          ; save scratch (R0=0, Format M offset)
STW   R2, [R0 + 0x44]
STW   R3, [R0 + 0x48]
STW   R4, [R0 + 0x4C]

RDSYS R1, #0, #1               ; R1 = FAULT_ADDR
RDSYS R2, #0, #2               ; R2 = FAULT_STATUS

; Check fault type (bits 3:0)
AND   R3, R2, #0x0F            ; isolate TYPE field
CMPI  R3, #1                   ; TLB miss?
BNE   .protection_fault        ; if not, it's a protection fault

; ── Page table walk ───────────────────────────────
; R1 = faulting virtual address
; Compute PD index = R1[31:22], PT index = R1[21:12]
; PD base stored at fixed location 0x50

LDW   R3, [R0 + 0x50]          ; R3 = page directory base (physical)
; ... shift R1 to get PD index, load PD entry ...
; ... from PD entry, get PT base, load PT entry ...
; ... build TLB_VPN and TLB_PTE from the PTE ...

; ── Replacement decision ──────────────────────────
; Compute target set from faulting address
; Read both ways, pick victim (check V bits, SW bits)
; ... (see "Reading an Entry" above) ...

; ── Load TLB entry ────────────────────────────────
WRSYS R_idx, #0, #5            ; select victim slot
WRSYS R_vpn, #0, #3            ; stage VPN + ASID
WRSYS R_pte, #0, #4            ; commit PPN + flags

; ── Restore and return ────────────────────────────
LDW   R1, [R0 + 0x40]
LDW   R2, [R0 + 0x44]
LDW   R3, [R0 + 0x48]
LDW   R4, [R0 + 0x4C]
ERET

.protection_fault:
; ... handle permission violation or page-not-present ...
; ... may involve demand paging, signal delivery, etc. ...
```

### Handler Safety — Pinning

The TLB miss handler must not cause recursive TLB misses. Pin these pages using SW bits and never replace them:

| Pinned page | Purpose | Entries |
|-------------|---------|---------|
| Vector page (0x00000000) | Handler code, scratch save area, PD base pointer | 1 |
| Page directory | First-level page table walk | 1 |
| Kernel page table pages | Second-level walk for kernel space | 2–3 |
| **Total** | | **4–5 of 64** |

Since replacement is fully software-controlled, "pinning" is just a convention: the handler checks SW bits and never evicts entries where the pin bit is set.

---

## Software-Managed Dirty Tracking

There is no hardware dirty bit. The OS tracks dirty pages using the **write-fault mechanism**:

1. **Initial load:** Map the page with W=0 (read-only) even if the page is logically writable
2. **First write:** Hardware raises protection fault (W=0, write access)
3. **Fault handler:** Recognize this as a dirty-tracking fault (FAULT_STATUS shows TYPE=protection, W=1). Mark the page dirty in kernel bookkeeping. Reload the TLB entry with W=1.
4. **Subsequent writes:** Succeed normally (W=1)

The one-time fault cost per page is amortized over the page's lifetime. When the pageout daemon needs to write a page to disk, it checks its dirty bookkeeping to decide which pages need writing.

---

## Context Switch

When switching between processes:

### Without ASID (initial implementation)
1. Save outgoing process state (registers, PC, SR)
2. Flush all TLB entries (software loop, ~320 instructions)
3. Load new process's page directory base pointer to the fixed location (0x50)
4. Re-pin handler/kernel entries for the new address space
5. Restore incoming process state
6. `ERET` — first few instructions will TLB miss and be loaded on demand

### With ASID (future)
1. Save outgoing process state
2. Set MMUCR.ASID to the incoming process's ASID
3. Load new PD base pointer
4. Restore incoming process state, `ERET`

No TLB flush needed — entries from different ASIDs coexist. Kernel pages with G=1 are shared. ASID recycling: when all 256 ASIDs are used, flush the TLB and restart allocation.

---

## Page Table Format (OS Convention)

The hardware has no knowledge of in-memory page tables. The OS is free to use any format. A natural 32-bit PTE format that maps directly to TLB_PTE (avoiding bit-shuffling in the miss handler) is:

```
 31          12 11   8 7 6 5 4 3 2 1 0
┌──────────────┬──────┬─┬─┬─┬─┬─┬─┬─┬─┐
│   PPN (20)   │SW(4) │G│U│X│W│R│C│—│V│
└──────────────┴──────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

If the in-memory PTE uses this layout, the miss handler can load it directly into TLB_PTE with no translation — a single `LDW` + `WRSYS` pair. The SW bits can carry OS metadata (pinned, dirty, age) that gets copied into the TLB entry.

### Two-Level Page Table (Recommended)

```
Virtual address:  [PD index (10 bits)][PT index (10 bits)][Page offset (12 bits)]
                   bits 31:22          bits 21:12          bits 11:0
```

- **Page directory (PD):** 1024 entries × 4 bytes = 4 KB. Each entry points to a page table page.
- **Page table (PT):** 1024 entries × 4 bytes = 4 KB. Each entry is a PTE.
- **Coverage:** One PD entry covers 4 MB. Full address space = 1024 PD entries = 4 GB.

For Minix 2 with small processes: ~1 PD + 1–2 PT pages per process = 8–12 KB overhead.

---

## Boot Sequence

After reset:
- SR = `{ S=1, I=0, flags=0 }` — supervisor mode, interrupts disabled
- MMUCR = `{ M=0, ASID=0 }` — flat/bypass mode
- PC = `0xFFFF_0000` (boot ROM)

Recommended boot procedure:
1. Execute boot ROM in flat mode (M=0) — all addresses are physical, uncached
2. Initialize SDRAM controller
3. Load kernel from storage into SDRAM at `0x0000_0000`
4. Set up interrupt vector table at `0x0000_0000` (including TLB miss handler)
5. Set up scratch save area at `0x0000_0040` and PD base at `0x0000_0050`
6. Build initial page tables in SDRAM
7. Load initial TLB entries: pin vector page, PD, kernel PT pages (G=1, pin bit in SW)
8. **Identity-map the boot code page** (see below)
9. Set M=1 via `WRSYS` to enable TLB translation
10. Jump to kernel entry point — now running with virtual addressing

### Enabling the MMU safely

**This is critical. Getting it wrong causes the CPU to take a TLB miss on the very first instruction fetch after M=1, before any fault handler can run.**

When `WRSYS` sets MMUCR.M=1, the next instruction fetch goes through the TLB. If that fetch address has no TLB entry, the CPU takes a TLB miss exception — but the exception handler itself needs TLB entries to run, creating an unrecoverable fault.

The safe pattern is **identity-map before enable**: ensure the page containing the code that enables the MMU has a TLB entry where virtual address = physical address. When M transitions from 0 to 1, the first translated fetch returns the same physical address that bypass mode would have, so execution continues seamlessly.

```asm
; Boot code running at physical address 0x0000_0xxx (M=0)

; Step 1: Identity-map the page containing this code
;         VPN = 0x00000 (page 0), PPN = 0x00000, V=1, R=1, X=1, G=1
LLI   R1, #0
WRSYS R1, #0, #5           ; TLB_INDEX = {way=0, set=0}
LLI   R2, #0               ; TLB_VPN = {VPN=0, ASID=0}
WRSYS R2, #0, #3
LLI   R3, #0xA9            ; TLB_PTE = {PPN=0, flags: V=1, X=1, R=1, G=1}
WRSYS R3, #0, #4           ; entry committed

; Step 2: Also identity-map the vector/handler page (if different),
;         kernel page directory, kernel PT pages — all pinned

; Step 3: Enable MMU
LLI   R10, #0x0501         ; MMUCR: M=1, ASID=5
WRSYS R10, #0, #0          ; ← M goes high on the next clock edge

; Next instruction fetch is now TLB-translated, but identity map
; ensures the physical address is the same. Execution continues.

; Step 4: Jump to kernel virtual address
LLI   R11, kernel_entry
JMP   R11
```

**Rules for MMU enable:**
1. The page containing the `WRSYS` that sets M=1 **must** be identity-mapped in the TLB before the write
2. The vector page (0x00000000) and TLB miss handler code **must** have valid, pinned TLB entries before M=1
3. The instruction immediately after the `WRSYS` must be on the same identity-mapped page (don't let the enable instruction be the last word of a page)

### WRSYS as a serializing instruction

`WRSYS` to any sysreg is defined as **serializing**: it guarantees that all effects of the write are visible before the next instruction fetch begins. In the current non-pipelined design this is automatic (execute completes → fetch starts). For future pipelined implementations:

- Any `WRSYS` must drain the pipeline: discard any prefetched instructions, ensure the write has taken effect, then resume fetching
- This is the conservative but safe choice — `WRSYS` is only used by kernel code and is never performance-critical
- This avoids the class of bugs where a prefetched instruction executes under the old MMU/interrupt/privilege state (these bugs are notoriously difficult to reproduce and diagnose)

### Disabling the MMU

The reverse transition (M=1 → M=0) has the same concern: the code that disables the MMU must be at a virtual address that equals its physical address (identity-mapped), so that when the next fetch bypasses the TLB and uses the virtual address as a physical address, it still reaches the same instruction stream.

In practice, MMU disable is rare (warm reboot, crash dump). The same identity-map rule applies.

---

## Address Space Layout

_To be defined._ Typical split:
- User space in lower portion of address space
- Kernel space in upper portion (mapped in all address spaces, G=1)
- I/O region mapped at fixed physical addresses with C=0

---

## Cache Considerations

### Uncached Access

MMIO and DMA regions must bypass the cache. Set C=0 in the TLB entry for these pages. Since Penumbra uses PIPT caches, the C bit is available from the TLB lookup before the cache is consulted — the cache checks C and bypasses itself when C=0.

### I-Cache Coherence

After loading new executable code (e.g., `exec()` in Minix 2), the kernel must invalidate the I-cache to prevent stale instruction fetches. Sequence: load new TLB entries for code pages, then issue I-cache invalidate.

### DMA Coherence

No hardware cache snooping. All DMA buffers must be mapped with C=0 (uncached). Use kernel bounce buffers for user↔DMA transfers. Sufficient for the target workload (Minix 2 with 10/100 Mbit Ethernet).

---

## Hardware Implementation Notes

_This section is for hardware designers, not OS implementers._

### Cache Architecture

Implemented in `rtl/soc/cache.sv` — a single parameterized module reused for both
I-cache and D-cache instances. Split I/D, both direct-mapped PIPT:

- **Module:** `cache.sv` with parameters: `NUM_SETS` (default 64), `LINE_WORDS` (default 4), `NUM_WAYS` (default 1), `CACHE_TYPE` (default WT/WnA)
- **I-Cache:** Read-only, invalidate-all via sysreg (device 3). Required after code load.
- **D-Cache:** Write-through, write-no-allocate. Invalidate via sysreg (device 2). Required for DMA coherence.
- **Line size:** 4 words (16 bytes), parameterizable
- **Cache size:** 4 KB per cache at default settings (64 sets × 4 words × 4 bytes), parameterizable
- **Geometry discovery:** Software reads INFO sysreg to learn line size, sets, ways, and type
- **Disabled at reset:** Cache starts disabled (CTRL.ENABLE=0), passes through like cache_stub. Kernel enables after TLB setup.
- Cache misses stall via the same `busy` mechanism as long-latency ALU ops
- Read miss: burst-fills entire line from memory (LINE_WORDS sequential reads), then returns to IDLE for re-hit
- Write hit: updates cache line (byte-granular) + writes through to memory
- Write miss: passes write to memory without filling (write-no-allocate)
- Uncacheable (C=0): passes through to memory regardless of enable state

### TLB Hardware

The TLB is a pure lookup table — no state machines, no replacement FSM:
1. **Storage:** 64 × 64-bit register file, addressed by {way, set}
2. **Lookup:** Parallel 20-bit VPN + 8-bit ASID comparators on both ways, gated by V and G
3. **Permission check:** One-hot access type AND with {X,W,R}, plus U check for user mode — single gate level
4. **Sysreg access:** Indexed read/write via TLB_INDEX addressing

### Discrete 74xx Feasibility

- TLB storage: eight 64×8-bit SRAMs (byte-aligned 64-bit entries)
- VPN comparison: three 74HC688 (8-bit comparator) per way × 2 ways = 6 ICs
- ASID comparison: one 74HC688 per way = 2 ICs
- Permission check: one 74HC08 (AND) + one 74HC32 (OR)
- Total: ~20 ICs for the complete TLB
