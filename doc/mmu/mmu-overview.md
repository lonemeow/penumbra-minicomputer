# Penumbra MMU - Overview

## Goals

- Page-based virtual-to-physical address translation
- Per-page protection (read, write, execute) with user/supervisor distinction
- Software-managed TLB for fast translation — hardware does lookups only, software handles all management (loading, replacement, invalidation)
- Support for precise exceptions on page faults to enable demand paging
- Per-page cacheability control for memory-mapped I/O
- Design must be feasible in discrete 74xx logic

## Cache Architecture

Penumbra uses a **split I/D cache** design with both caches being **direct-mapped** and **PIPT** (physically indexed, physically tagged).

### I-Cache

- Read-only from the cache's perspective
- No write policy complexity (no dirty bits, no writeback)
- Supports a full invalidation instruction (privileged) to maintain coherence after loading executable code via the D-cache path (e.g., `exec()` in Minix 2)
- Targeted per-line invalidation may be added later if ISA encoding space permits

### D-Cache

- **Initial implementation:** Write-through (every store writes to both cache and main memory)
- **Upgrade path:** Write-back with dirty bits per cache line (add 1 bit per tag entry)
- **Future enhancements:** Single-entry write buffer, then write-combining/writeback buffer

Design guidelines for a clean upgrade path:
1. Cache-to-bus interface is a well-defined boundary (`addr`, `wdata`, `rdata`, `we`, `re`, `burst_len`, `valid`, `ready`)
2. Tag storage is a separate submodule (today: `{valid, tag}`; write-back adds: `{valid, dirty, tag}`)
3. Cache controller FSM is separated from the datapath — changing write policy is an FSM change, not a datapath change
4. Stores always write the cache data array (not bypassed to bus), so removing the simultaneous bus write for write-back is a clean FSM transition removal

### Cache Parameters (Initial)

- Line size: TBD (likely 16 bytes / 4 words for spatial locality without large fill penalties)
- Cache size: TBD (likely 2-4 KB each, affordable on ECP5 with 208 EBR blocks)

### Cache Miss Stall

Cache misses use the same `busy`/`done` stall mechanism as long-latency ALU operations. The microcode holds on a "wait for memory ready" micro-op until the cache fill completes.

## Uncached Access

Memory-mapped I/O regions must bypass the cache. Cacheability is controlled by a **C (cacheable) bit in each TLB entry**:

- **C=1:** Normal cached access through the L1 cache
- **C=0:** Cache is bypassed; access goes directly to the system bus

Since Penumbra is PIPT, the TLB lookup (which produces the physical address) also produces the C bit before the cache is consulted. The cache checks C and bypasses itself when C=0. This comes essentially for free in the PIPT design.

## MMU Control Registers

The MMU has its own set of privileged control registers, separate from the CPU's status register. This maintains a clean interface: the CPU sends virtual addresses to the MMU and receives physical addresses and status; MMU configuration is not a CPU concern.

MMU registers are accessed via the system register bus using `MTSYS`/`MFSYS` instructions with device ID 0. See the ISA architecture overview for the system register access mechanism.

| sys_reg | Name         | Description |
|---------|--------------|-------------|
| 0       | MMUCR        | MMU control register (see below) |
| 1       | FAULT_ADDR   | Faulting virtual address (hardware-latched, read-only to software) |
| 2       | FAULT_STATUS | Fault reason (hardware-latched, read-only to software; see encoding below) |
| 3       | TLB_VPN      | TLB entry upper word: VPN + ASID (read/write) |
| 4       | TLB_PTE      | TLB entry lower word: PPN + flags (read/write; write commits entry) |
| 5       | TLB_INDEX    | Target TLB slot: `{ 26'b0, way[0], set[4:0] }` |
| 6-15    | (reserved)   | Future expansion |

### MMUCR Register Layout

| Bit | Name | Description |
|-----|------|-------------|
| 0   | M    | Enable translation (0=bypass/flat, 1=TLB active) |
| 7:1 | —    | Reserved |
| 15:8| ASID | Current address space ID (8-bit, for TLB matching) |
| 31:16| —   | Reserved |

### FAULT_STATUS Register Layout

| Bits | Field | Description |
|------|-------|-------------|
| 3:0  | TYPE  | Fault type: `0001`=TLB miss, `0010`=protection violation |
| 7:4  | —     | Reserved (gap for future fault types) |
| 8    | R     | Faulting access was a read |
| 9    | W     | Faulting access was a write |
| 10   | X     | Faulting access was an execute (fetch) |
| 11   | USR   | Faulting access was in user mode |
| 31:12| —     | Reserved |

On a fault, the MMU latches the faulting virtual address into FAULT_ADDR and the reason into FAULT_STATUS before the CPU enters the exception handler. The handler reads these via `MFSYS`. Both registers are read-only to software — the MMU hardware writes them.

### MMU Flat Mode (M=0)

When the M bit in MMUCR is 0 (the reset default), the TLB is bypassed entirely:
- Virtual address = physical address (identity mapping)
- All accesses are uncached (C is forced to 0)
- No permission checks are performed

This allows the boot ROM and early bootloader to execute without any TLB setup. The OS sets M=1 after initializing page tables and loading the TLB.

Hardware cost: one mux on the physical address bus (bypass vs. TLB output), one AND gate forcing C=0.

Use cases for uncached pages:
- Memory-mapped I/O registers (e.g., UART, SPI, Wiznet Ethernet)
- Kernel-owned DMA bounce buffers

## DMA Coherence Strategy

Penumbra does **not** implement hardware cache snooping. DMA coherence is managed in software:

- All DMA buffers are allocated in kernel memory mapped with C=0 (uncached)
- No direct userspace DMA; the kernel uses bounce buffers to copy between user buffers and DMA-safe uncached memory
- This eliminates the need for D-cache invalidation around DMA transfers
- Future optimization: D-cache invalidate instruction or fast block copy instructions could reduce the bounce buffer overhead

This approach is sufficient for the target workload (Minix 2 with Wiznet Ethernet at 10/100 Mbit).

## Page Size

**4 KB** (12-bit page offset, 20-bit virtual page number).

This gives a symmetric two-level page table split: 10 bits (page directory index) + 10 bits (page table index) + 12 bits (page offset) = 32 bits.

- 64-entry TLB covers 256 KB of active memory — sufficient for Minix 2 working sets
- Average fragmentation waste of ~2 KB per mapping — acceptable with 32 MB SDRAM
- Each page table page holds exactly 1024 PTEs at 4 bytes each — fills one 4 KB page perfectly

## Page Table Structure

Since the TLB is software-managed, the page table structure is an OS convention, not a hardware requirement. However, the natural structure for 4 KB pages with 32-bit addresses is a **two-level page table**:

```
Virtual address:  [PD index (10 bits)][PT index (10 bits)][Page offset (12 bits)]
                   bits 31:22          bits 21:12          bits 11:0
```

- **Page directory (PD):** 1024 entries × 4 bytes = 4 KB (one page). Each entry points to a page table.
- **Page table (PT):** 1024 entries × 4 bytes = 4 KB (one page). Each entry is a PTE mapping one virtual page to a physical page.
- **Total pages mapped per PT:** 1024 × 4 KB = 4 MB
- **Full address space:** 1024 PTs × 4 MB = 4 GB

For Minix 2 with small processes, most page directory entries will be empty (not present), so only a few page table pages are allocated per process. A typical process with 64 KB of text+data+stack needs only 1 page directory + 1 page table page = 8 KB of overhead.

## TLB

### Overview

The TLB is **fully software-managed**: the hardware performs parallel lookups and permission checks, but all management — loading entries, choosing replacement victims, invalidation — is done by software via sysreg instructions. The hardware has no replacement logic, no LRU state, and no knowledge of the page table format.

- **Organization:** 2-way set-associative
- **Size:** 64 entries (32 sets × 2 ways)
- **Lookup:** VPN (bits 31:12) indexes the set; both ways are compared in parallel
- **Miss:** Raises a TLB miss exception; software handler decides which slot to replace, loads the entry via sysreg writes
- **Entry width:** 64 bits (wider than the 32-bit native word; accessed as two 32-bit sysreg halves)

### TLB Entry Format (64 bits)

```
 63        44 43        24 23    16 15     8 7 6 5 4 3 2 1 0
┌────────────┬────────────┬────────┬────────┬─┬─┬─┬─┬─┬─┬─┬─┐
│  VPN (20)  │  PPN (20)  │ASID(8) │ SW (8) │G│U│X│W│R│C│—│V│
└────────────┴────────────┴────────┴────────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

| Field | Bits | Width | Description |
|-------|------|-------|-------------|
| VPN   | 63:44 | 20 | Virtual page number (vaddr bits 31:12) |
| PPN   | 43:24 | 20 | Physical page number |
| ASID  | 23:16 | 8  | Address space ID (reserved until ASID support enabled) |
| SW    | 15:8  | 8  | Software-defined — hardware stores but never interprets. OS may use for pinning, LRU/age tracking, dirty status, or any other purpose |
| G     | 7     | 1  | Global — hardware skips ASID comparison when set (for kernel pages shared across all address spaces) |
| U     | 6     | 1  | User-accessible (0 = supervisor only) |
| X     | 5     | 1  | Execute permission |
| W     | 4     | 1  | Write permission |
| R     | 3     | 1  | Read permission |
| C     | 2     | 1  | Cacheable (0 = bypass cache, go direct to bus) |
| _rsvd_| 1     | 1  | Reserved for future hardware flag |
| V     | 0     | 1  | Valid (entry is in use) |

**Design notes:**
- **64 bits per entry = 8 bytes.** Clean byte alignment for discrete 74xx SRAM (e.g., two 64×32-bit SRAMs or eight 64×8-bit SRAMs).
- **Total TLB storage:** 64 entries × 64 bits = 4096 bits = 512 bytes. Fits in one ECP5 EBR (or one 74HC189 bank in discrete).
- **Hardware flags (bits 7:0):** These are the only bits the hardware reads during lookup and permission checking. The rest (SW, ASID, VPN, PPN) are used for matching or are software-managed.
- **No hardware-managed dirty bit.** Dirty tracking is fully software-managed: the OS loads writable pages with W=0, traps the first write (protection fault), sets W=1 and updates its own dirty bookkeeping. This keeps the TLB as pure lookup hardware with no read-modify-write path.
- **No hardware replacement.** The "pinned/wired" concept is a software convention using the SW bits. The OS decides replacement policy entirely.
- **ASID:** Reserved in the entry format but not matched in the initial implementation (all entries effectively global). When ASID matching is enabled, the hardware adds one 8-bit comparator per way.

### Sysreg Access Protocol

TLB entries are accessed as two 32-bit halves via the sysreg interface, with TLB_INDEX selecting the target slot:

**TLB_INDEX** (sysreg 5): `{ 26'b0, way[0], set[4:0] }`

**TLB_VPN** (sysreg 3) — upper 32 bits of entry:
```
 31      28 27            8 7          0
┌──────────┬───────────────┬────────────┐
│  0000    │   VPN (20)    │  ASID (8)  │
└──────────┴───────────────┴────────────┘
```

**TLB_PTE** (sysreg 4) — lower 32 bits of entry:
```
 31          12 11       4 3 2 1 0
┌──────────────┬─────────┬─┬─┬─┬─┬─┬─┬─┬─┐
│   PPN (20)   │ SW (8)  │G│U│X│W│R│C│—│V│
└──────────────┴─────────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

**Write (load an entry):**
```asm
MTSYS  r_idx, #0, #5    ; select slot {way, set}
MTSYS  r_vpn, #0, #3    ; write VPN + ASID
MTSYS  r_pte, #0, #4    ; write PPN + SW + flags → commits entry to TLB
```

**Read (inspect an entry — for replacement decisions):**
```asm
MTSYS  r_idx, #0, #5    ; select slot {way, set}
MFSYS  r_vpn, #0, #3    ; read VPN + ASID
MFSYS  r_pte, #0, #4    ; read PPN + SW + flags
```

Note: the 64-bit entry width exceeds the 32-bit native word size. Loading or reading an entry requires two sysreg operations. A future optimization could provide a "read set" operation that returns both ways, reducing replacement-decision overhead from 4 to 2 sysreg reads.

### TLB Lookup (Hardware)

On every memory access (when M=1):
1. Compute set index from VPN: `set = vaddr[16:12]` (lower 5 bits of VPN)
2. Compare VPN field of both ways against `vaddr[31:12]`
3. Compare ASID field against MMUCR.ASID (skip if entry's G=1)
4. Check V=1
5. If match: extract PPN, C, permission flags → permission check
6. If no match in either way: TLB miss exception

**Permission check** (on hit):
- Execute access: require X=1 (else protection fault)
- Read access: require R=1 (else protection fault)
- Write access: require W=1 (else protection fault)
- User mode access: require U=1 (else protection fault)
- Supervisor can access U=0 pages (supervisor bypasses U check)

On protection fault: latch FAULT_ADDR and FAULT_STATUS, raise exception.

### TLB Miss Handling

On a TLB miss, the MMU:
1. Latches the faulting virtual address into FAULT_ADDR
2. Sets FAULT_STATUS with TYPE=0001 (TLB miss) and the access type/mode bits
3. Raises the TLB miss exception (vector 4, shared with page fault)

The exception handler:
1. Reads FAULT_ADDR and FAULT_STATUS via `MFSYS`
2. Checks FAULT_STATUS.TYPE to distinguish TLB miss from protection fault
3. Walks the page table in software to find the PTE
4. If the page is present:
   a. Reads both ways of the target set via TLB_INDEX + MFSYS to decide which slot to replace
   b. Loads the new entry via TLB_INDEX + TLB_VPN + TLB_PTE
   c. Returns via RTI
5. If the page is not present: invokes the page fault handler (demand paging)

**Handler safety:** The TLB miss handler itself must not cause recursive TLB misses. The OS ensures this by using SW bits to "pin" the handler's code page(s) and the kernel page table page(s). Since replacement is software-controlled, pinned entries are never overwritten.

### Software-Managed Dirty Tracking

There is no hardware dirty bit. The OS tracks dirty pages using a write-fault mechanism:

1. On initial page mapping or TLB load: set W=0 in the TLB entry (read-only)
2. First write to the page → protection fault (W=0 violation)
3. Fault handler: mark the page dirty in kernel data structures, set W=1 in TLB entry, return
4. Subsequent writes succeed (W=1)

This keeps the TLB hardware purely as a lookup table with no write-back path. The one-time fault cost per page is amortized over the page's lifetime.

### TLB Invalidation

All invalidation is done in software via sysreg writes — there is no hardware flush command, consistent with the fully software-managed TLB philosophy:

- **Invalidate one entry:** Write TLB_INDEX to select the slot, then write TLB_PTE with V=0.
- **Invalidate all:** Software loop over all 64 entries, writing V=0 to each via TLB_INDEX + TLB_PTE (~320 instructions). Used on context switch (when ASID is not implemented) or `exec()`. Once ASID is enabled, full flushes become rare.
- **Invalidate by VPN search:** Compute the target set from VPN[16:12], read both ways, clear matching entries. Two reads + one write — fast enough.

### TLB and I-Cache Coherence

After loading new executable code (e.g., `exec()` in Minix 2), the kernel must:
1. Load the new TLB entries for the code pages
2. Issue a full I-cache invalidate instruction

This ensures the I-cache doesn't serve stale instructions from a previous mapping.

## Page Table Entry Format (Software)

Since the TLB is software-managed, the in-memory page table format is defined by the OS, not the hardware. However, a natural 32-bit format that maps directly to TLB_PTE is:

| Bits | Field | Description |
|------|-------|-------------|
| 31:12 | PPN | Physical page number |
| 11:4  | SW  | Software-defined (pinned, dirty, age, etc.) |
| 3     | R   | Read |
| 2     | W   | Write |
| 1     | X   | Execute |
| 0     | V   | Valid / present |

This is a suggested format only — the hardware does not interpret in-memory page tables. The OS is free to use any format, as long as the TLB miss handler translates it into the TLB_VPN/TLB_PTE register format. The in-memory PTE does not need to match the TLB entry layout.

## Boot Sequence

After reset, the CPU starts in the following state:
- SR = `{ S=1, I=0, flags=0 }` — supervisor mode, interrupts disabled
- MMUCR = `{ M=0, ASID=0 }` — MMU in flat mode (identity mapped, uncached)
- PC = `0xFFFF_E000` (base of boot ROM)

Typical boot sequence:
1. Boot ROM executes in flat uncached mode (M=0) starting at `0xFFFF_E000`
2. Initialize SDRAM controller
3. Load bootloader/kernel from SPI flash or SD card into SDRAM (starting at `0x0000_0000`)
4. Set up interrupt vector table at `0x0000_0000`
5. Set up initial page tables in SDRAM
6. Load TLB with initial entries (kernel pages with G=1, handler code pinned via SW bits)
7. Set M=1 via `MTSYS` (enable MMU translation)
8. Jump to kernel entry point

## Address Space Layout

_To be defined._ Typical split:
- User space in lower portion of address space
- Kernel space in upper portion (mapped in all address spaces, G=1)
- I/O region mapped at fixed physical addresses with C=0
