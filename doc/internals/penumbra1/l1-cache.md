# Penumbra/1 — L1 Cache

> **Applies to:** Penumbra/1 · microcoded core.

The gen1 L1 is the distributed-RAM realization of the VIPT cache whose
generation-neutral constraint is described in
[`../mmu-internals.md`](../mmu-internals.md#vipt-l1-cache--architectural-constraint).
Module: `hw/rtl/soc/cache_vipt.sv`, instantiated twice in `cpu_core`
(once for I, once for D).

## Geometry

Split I/D direct-mapped VIPT caches, **1 KiB each, 64 sets × 4-word
lines**:

- Index and word offset come from the virtual address (`i_vaddr[9:0]`),
  so the cache RAM lookup runs in parallel with TLB translation.
- Tag compare uses the physical address (`i_paddr[31:10]`) once
  translation completes, gated by valid + permission.
- The 1 KiB ≤ 4 KiB page-size precondition makes it alias-free (see the
  architectural-constraint section linked above); asserted at sim time.

## Policy

- Read miss: burst-fills the entire line (4 words) from memory.
- Write hit: updates the cache line (byte-granular) + writes through to
  memory.
- Write miss: pass-through (write-no-allocate).
- Uncacheable (`C=0`): pass-through to memory.

## Zero-cycle hit and the flop boundary

The cache state machine reacts to its inputs via registered shadow
flops (`i_re_q`, `i_we_q`, `hit_q`, `i_paddr_q`, …) so a real flop
boundary sits between the microcode-driven cache inputs and the cache
state-machine D-cone. The combinational hit path (vaddr → idx ‖ TLB →
tag compare → `o_rdata`, `o_busy`) stays untouched, preserving the
zero-cycle hit. CPU-visible cost: read-miss fills, write-throughs, and
invalidates start one cycle later internally; the CPU stalls naturally
through this via the unchanged `o_busy` contract.

The zero-cycle combinational hit is the defining gen1 trait, and the
reason `cache_vipt.sv` is **not** reused by Penumbra/2 — whose
pipelined front end wants a registered (BRAM) hit instead. See
Decision 11 in
[`../penumbra2/design-decisions.md`](../penumbra2/design-decisions.md).

## Line fill and the split-L1 arbiter

The two caches' back sides share the
[CPU memory port](../cpu-bus.md#cpu-memory-port) through
`cpu_bus_arbiter` (`hw/rtl/penumbra1/cpu_bus_arbiter.sv`), which
registers the winning request and drives it onto the port.  A cache
cannot tell from `busy` alone when the arbiter has latched its
request, so the arbiter tells it: a combinational `o_*_req_accepted`
pulse fires for exactly one cycle, the cycle the arbiter latches that
cache's request.  The cache advances its fill pointer on the next
cycle without waiting for the completion, and the arbiter re-latches
the next request on the completion cycle itself (`BUSY → BUSY`), so
the port stays driven word after word with no dead cycle between
completions.

A 4-word fill from a port that completes every cycle:

| Cycle | `re` | `addr` (cache drives) | port `busy` | `rdata` | `req_accepted` |
|------:|:----:|:---------------------:|:-----------:|:-------:|:--------------:|
| 0     | 1    | A+0                   | 0           | —       | pulse (A+0 latched) |
| 1     | 1    | A+4                   | 0           | **D0**  | pulse (A+4 latched) |
| 2     | 1    | A+8                   | 0           | **D4**  | pulse (A+8 latched) |
| 3     | 1    | A+C                   | 0           | **D8**  | pulse (A+C latched) |
| 4     | 0    | —                     | 0           | **DC**  | —              |

A port that needs wait cycles raises `busy` between completions, but
`busy` must be low on each word's completion cycle: the arbiter
latches the next request only then, so a port holding `busy` across a
whole fill would never deliver a word.  `cache_vipt.sv` uses
`i_req_accepted` to advance its `req_idx`/`resp_idx` counters during
line fills.
