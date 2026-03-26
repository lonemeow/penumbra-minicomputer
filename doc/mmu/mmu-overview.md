# Penumbra MMU - Overview

## Goals

- Page-based virtual-to-physical address translation
- Per-page protection (read, write, execute) with user/supervisor distinction
- TLB for fast translation in the common case
- Support for precise exceptions on page faults to enable demand paging
- Per-page cacheability control for memory-mapped I/O

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

Memory-mapped I/O regions must bypass the cache. Cacheability is controlled by a **C (cacheable) bit in each page table entry**:

- **C=1:** Normal cached access through the L1 cache
- **C=0:** Cache is bypassed; access goes directly to the system bus

Since Penumbra is PIPT, the TLB lookup (which produces the physical address) also produces the C bit before the cache is consulted. The cache checks C and bypasses itself when C=0. This comes essentially for free in the PIPT design.

## MMU Control Registers

The MMU has its own set of privileged control registers, separate from the CPU's status register. This maintains a clean interface: the CPU sends virtual addresses to the MMU and receives physical addresses and status; MMU configuration is not a CPU concern.

MMU registers are accessed via the system register bus using `MTSYS`/`MFSYS` instructions with device ID 0. See the ISA architecture overview for the system register access mechanism.

| sys_reg | Name         | Description |
|---------|--------------|-------------|
| 0       | MMUCR        | MMU control register — contains the M (enable) bit |
| 1       | FAULT_ADDR   | Virtual address that caused the most recent fault |
| 2       | FAULT_STATUS | Fault reason: not present, protection violation, read/write/execute, user/supervisor |
| 3       | TLB_VPN      | Write: set VPN for next TLB load |
| 4       | TLB_PTE      | Write: set PTE (PPN + flags + WR bit) for next TLB load; writing triggers the entry to be placed in the TLB |
| 5       | TLB_INDEX    | TLB entry index for targeted read/invalidation |
| 6-15    | (reserved)   | Future expansion |

On a page fault (vector 4) or TLB miss (vector — see TLB section), the MMU latches the faulting virtual address into FAULT_ADDR and the reason into FAULT_STATUS before the CPU enters the exception handler. The handler reads these via `MFSYS`.

### MMU Flat Mode (M=0)

When the M bit in MMUCR is 0 (the reset default), the TLB is bypassed entirely:
- Virtual address = physical address (identity mapping)
- All accesses are uncached (C is forced to 0)
- No permission checks are performed

This allows the boot ROM and early bootloader to execute without any TLB setup. The OS sets M=1 after initializing page tables and loading the TLB.

Hardware cost: one OR gate on the TLB hit signal (force hit when M=0), one AND gate forcing C=0, and the identity mapping is simply passing the virtual address through to the physical address bus (mux selects virtual address when M=0, TLB output when M=1).

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

The TLB is **software-managed**: the hardware performs lookups, but on a TLB miss, a software exception handler walks the page table and loads the new entry. The hardware has no knowledge of the page table format, giving the OS complete flexibility over page table structure.

- **Organization:** 2-way set-associative
- **Size:** 64 entries (32 sets × 2 ways)
- **Lookup:** Virtual page number (VPN, bits 31:12) indexes into a set; both ways are compared in parallel
- **Miss:** Raises a TLB miss exception; software handler loads the entry via MTSYS to TLB_VPN and TLB_PTE

### TLB Entry Format

Each TLB entry contains:

| Field | Bits | Description |
|-------|------|-------------|
| VPN   | 20   | Virtual page number (addr bits 31:12) |
| PPN   | 20   | Physical page number |
| V     | 1    | Valid (entry is in use) |
| C     | 1    | Cacheable |
| R     | 1    | Read permission |
| W     | 1    | Write permission |
| X     | 1    | Execute permission |
| U     | 1    | User-accessible (0 = supervisor only) |
| D     | 1    | Dirty (page has been written to) |
| WR    | 1    | Wired — entry is exempt from replacement |

Total: **48 bits per entry**. For 64 entries: 3072 bits total (fits in one ECP5 EBR; in discrete, a few byte-wide SRAMs).

### Wired Entries

Any TLB entry can be marked as wired by setting the **WR bit** when loading the entry via TLB_PTE. Wired entries are never evicted by the replacement policy.

This is managed entirely by the OS:
- The TLB miss handler wires its own page table pages so it can walk the page table without causing recursive TLB misses
- The kernel can wire frequently-accessed pages (kernel stack, interrupt handler code) for performance
- Wiring strategies can be changed at runtime without hardware modification — useful for performance experimentation

Hardware cost: one extra bit per TLB entry, one AND gate in the replacement logic (`eligible_for_eviction = V AND NOT WR`).

If both ways in a set are wired and a new entry maps to that set, one wired entry is overwritten (the OS should avoid this, but the hardware must not deadlock).

### Replacement Policy

On a TLB miss, after the software handler writes TLB_VPN and TLB_PTE:
1. The VPN indexes the target set (VPN bits select the set)
2. If either way in the set is invalid (V=0), use that slot
3. If one way is wired (WR=1) and the other is not, replace the unwired entry
4. If neither is wired, replace using a simple policy (e.g., pseudo-LRU or round-robin per set — 1 bit per set)
5. If both are wired, replace way 0 (degenerate case the OS should avoid)

### TLB Miss Handling

On a TLB miss, the MMU:
1. Latches the faulting virtual address into FAULT_ADDR
2. Sets FAULT_STATUS to indicate "TLB miss" (distinct from permission fault)
3. Raises the TLB miss exception (vector 4, shared with page fault — handler checks FAULT_STATUS to distinguish)

The exception handler:
1. Reads FAULT_ADDR via `MFSYS R0, #0, #1`
2. Reads FAULT_STATUS via `MFSYS R1, #0, #2` to determine miss vs. permission fault
3. Walks the page table in software to find the PTE
4. If the page is present: loads the TLB entry via `MTSYS` to TLB_VPN then TLB_PTE, returns via RTI
5. If the page is not present: invokes the page fault handler (demand paging)

### TLB Invalidation

- **Invalidate by index:** Write to TLB_INDEX, then clear the V bit — for targeted invalidation (e.g., on `munmap`)
- **Invalidate all:** Set a control bit in MMUCR to clear all V bits simultaneously — for context switch or `exec()`

### TLB and I-Cache Coherence

After loading new executable code (e.g., `exec()` in Minix 2), the kernel must:
1. Load the new TLB entries for the code pages
2. Issue a full I-cache invalidate instruction

This ensures the I-cache doesn't serve stale instructions from a previous mapping.

## Page Table Entry Format (Software)

Since the TLB is software-managed, the in-memory page table format is defined by the OS, not the hardware. However, a natural format that matches the TLB entry layout is:

| Bits | Field | Description |
|------|-------|-------------|
| 31:12 | PPN | Physical page number |
| 7 | WR | Wired hint (OS can use this to decide whether to set WR when loading TLB) |
| 6 | D | Dirty |
| 5 | U | User-accessible |
| 4 | X | Execute |
| 3 | W | Write |
| 2 | R | Read |
| 1 | C | Cacheable |
| 0 | V | Valid / present |

This is a suggested format only — the hardware does not interpret in-memory page tables. The OS is free to use any format, as long as the TLB miss handler translates it into the TLB_VPN/TLB_PTE register format.

## Boot Sequence

After reset, the CPU starts in the following state:
- SR = `{ S=1, I=0, flags=0 }` — supervisor mode, interrupts disabled
- MMUCR = `{ M=0 }` — MMU in flat mode (identity mapped, uncached)
- PC = `0xFFFF_E000` (base of boot ROM)

Typical boot sequence:
1. Boot ROM executes in flat uncached mode (M=0) starting at `0xFFFF_E000`
2. Initialize SDRAM controller
3. Load bootloader/kernel from SPI flash or SD card into SDRAM (starting at `0x0000_0000`)
4. Set up interrupt vector table at `0x0000_0000`
5. Set up initial page tables in SDRAM
6. Load TLB with initial entries
7. Set M=1 via `MTSYS` (enable MMU translation)
8. Jump to kernel entry point

## Address Space Layout

_To be defined._ Typical split:
- User space in lower portion of address space
- Kernel space in upper portion (mapped in all address spaces)
- I/O region mapped at fixed physical addresses with C=0
