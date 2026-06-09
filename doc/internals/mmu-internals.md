# Penumbra MMU — Hardware Design

> **Applies to:** all generations · shared hardware.

## TLB Hardware

Two parallel lookup structures, combined with pinned-hit-wins priority:

### Main TLB (64-entry, 2-way set-associative)
1. **Storage:** 64 × 64-bit register file, addressed by {way, set}.
2. **Lookup:** Parallel 20-bit VPN + 8-bit ASID comparators on both ways, gated by V and G.
3. **Permission check:** One-hot access type AND with {X,W,R}, plus U check for user mode.
4. **Sysreg access:** Indexed read/write via `TLB_INDEX` addressing.

### Pinned TLB (8-entry, fully associative)
1. **Storage:** 8 × 64-bit register file.
2. **Lookup:** 8-wide parallel VPN + ASID comparators.
3. **Priority:** Pinned hit masks main TLB result.

### Indexed readback (TLB enumeration)

The sysreg interface reads entries back by index (`TLB_INDEX` selects a
{set, way} or pinned slot; `TLB_VPN`/`TLB_PTE` return that slot's
contents). This read direction is **load-bearing, not diagnostic**: it
is how software enumerates which slots are resident during selective
invalidation (TLB shootdown by address or by ASID).

The reason readback cannot be elided: the TLB is the only record of
*slot occupancy*. The page tables are authoritative for translations
(VA→PA), but they are keyed by VA, not by hardware slot — and the miss
handler installs a refilled entry into a hardware-chosen slot without
the kernel tracking where. So to invalidate "the entry for this VA" or
"every entry for this ASID," software reads slots back to find the
matching one(s). Eliminating readback would require a software shadow
of every slot's contents, updated on the (hot) miss path — a worse
trade than the (cold) readback probe. Treat readback as a permanent
part of the sysreg contract.

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

## VIPT L1 Cache — Architectural Constraint

The split I/D L1 caches are **virtually-indexed, physically-tagged
(VIPT)** so the cache RAM lookup overlaps TLB translation: the index
and word offset come from the virtual address, while the tag compare
uses the physical address once translation completes.

The load-bearing invariant — true for **any** Penumbra L1, regardless
of generation — is **cache size ≤ page size**. With a 4 KiB page, an
L1 way ≤ 4 KiB keeps the index + offset bits entirely inside the page
offset, where the virtual and physical addresses are bit-identical.
That makes the cache alias-free for free: no synonym/homonym handling,
no page colouring, no ASID flushes. VIPT is then purely a timing
choice, not a correctness one.

This constraint binds the architecture; the *realization* — storage
type, hit latency, line/set geometry, fill and write policy — is
per-generation:

- **Penumbra/1** — distributed-RAM, zero-cycle combinational hit. See
  [`penumbra1/l1-cache.md`](penumbra1/l1-cache.md).
- **Penumbra/2** — BRAM-backed, registered (one-cycle) hit. Documented
  under `penumbra2/` once that RTL exists.
