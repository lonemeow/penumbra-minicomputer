# Penumbra MMU — Hardware Design

## TLB Hardware

Two parallel lookup structures, combined with pinned-hit-wins priority:

### Main TLB (64-entry, 2-way set-associative)
1. **Storage:** 64 × 64-bit register file, addressed by {way, set}.
2. **Lookup:** Parallel 20-bit VPN + 8-bit ASID comparators on both ways, gated by V and G.
3. **Permission check:** One-hot access type AND with {X,W,R}, plus U check for user mode.
4. **Sysreg access:** Indexed read/write via `TLB_INDEX` addressing.

### Pinned TLB (4-entry, fully associative)
1. **Storage:** 4 × 64-bit register file.
2. **Lookup:** 4-wide parallel VPN + ASID comparators.
3. **Priority:** Pinned hit masks main TLB result.

## Discrete 74xx Feasibility

### Main TLB
- TLB storage: eight 64×8-bit SRAMs (byte-aligned 64-bit entries).
- VPN comparison: three 74HC688 (8-bit comparator) per way × 2 ways = 6 ICs.
- ASID comparison: one 74HC688 per way = 2 ICs.
- Permission check: one 74HC08 (AND) + one 74HC32 (OR).
- Subtotal: ~20 ICs.

### Pinned TLB
- Storage: 4 × 8 bytes = 32 bytes in latches or small SRAM.
- VPN comparison: three 74HC688 per entry × 4 entries = 12 ICs.
- ASID/G logic: ~4 ICs.
- Priority mux: ~2 ICs.
- Subtotal: ~20 ICs.

**Total: ~40 ICs for the complete TLB subsystem.**

## Cache Hardware

Split I/D direct-mapped VIPT caches (1 KiB each, 64 sets × 4-word lines):
- Index and word offset come from the virtual address (`i_vaddr[9:0]`),
  so the cache RAM lookup runs in parallel with TLB translation.
- Tag compare uses the physical address (`i_paddr[31:10]`) once
  translation completes, gated by valid + permission.
- Aliasing-free precondition: cache size ≤ page size (1 KiB ≤ 4 KiB),
  so the index+offset bits live entirely inside the page offset where
  vaddr and paddr are bit-identical. No synonym/homonym handling, no
  page coloring, no ASID flushes — VIPT is purely a timing change here.
- Read miss: burst-fills entire line (4 words) from memory.
- Write hit: updates cache line (byte-granular) + writes through to memory.
- Write miss: pass-through (write-no-allocate).
- Uncacheable (C=0): pass-through to memory.

The cache state machine reacts to its inputs via registered shadow
flops (`i_re_q`, `i_we_q`, `hit_q`, `i_paddr_q`, …) so a real flop
boundary sits between the microcode-driven cache inputs and the cache
state-machine D-cone. The combinational hit path (vaddr → idx ‖ TLB →
tag compare → o_rdata, o_busy) stays untouched, preserving the
zero-cycle hit. CPU-visible cost: read-miss fills, write-throughs, and
invalidates start one cycle later internally; the CPU stalls naturally
through this via the unchanged `o_busy` contract.
