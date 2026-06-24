# Penumbra/2 — Hazard Model

> **Applies to:** Penumbra/2 · pipelined core.

This document specifies the data-hazard handling mechanism for the
Penumbra/2 pipeline: the scoreboard, the stall predicate, the
ISA → physical register mapping, the interaction with flags,
drain-commit, divmul, and exception entry. It is the reference for
implementing `id_stage.sv` and the standalone `scoreboard.sv` (if
factored out).

Higher-level design rationale lives in
[Decision 4](./design-decisions.md#4-hazard-handling-strategy)
(GPR/SPR pure-stall) and
[Decision 12](./design-decisions.md#12-nzcv-flag-forwarding)
(NZCV forwarding). This doc is the implementation contract: it states
*what the scoreboard does and how flags are forwarded*, not *why those
were chosen over the alternatives*.

## Scope

Covered:

- The scoreboard's storage, addressing, and update rules.
- The ID-stage stall predicate, stated formally.
- The decoder's ISA → physical mapping (including the R14
  banking case and SPR-USP cross-bank access).
- Interaction with drain-commit, divmul, and exception entry.
- Flag-hazard (NZCV) semantics ([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)).
- Worked examples of every distinct hazard scenario.

Out of scope:

- Per-stage pipeline-register layout (see
  [pipeline-stages.md](./pipeline-stages.md#inter-stage-pipeline-registers)).
- Branch-resolution flush penalty (see
  [Decision 5](./design-decisions.md#5-branch-resolution-policy)).
- Cache-miss and sysreg-sideband stall semantics
  (see [Decision 11](./design-decisions.md#11-bram-backed-caches-with-single-mem-stall)
  and [sysregs.md](../../system/sysregs.md)).
- The flag-forwarding *datapath* (the bypass-mux wiring in EX) — this
  doc specifies the *model* (which instructions produce/consume NZCV and
  the youngest-wins / committed-SR semantics); the EX-stage realization
  is in [pipeline-stages.md](./pipeline-stages.md). GPR/SPR forwarding
  does not exist in gen2 — gen2.5 adds it (the forward network, the
  write-through, and the relaxed stall predicate are specified in
  [gen2.5 GPR forwarding and regfile write-through](#gen25-gpr-forwarding-and-regfile-write-through)).

## The hazard problem in gen2

Penumbra/2 overlaps up to six instructions across IF1, IF2, ID, EX,
MEM, and WB. A naive pipeline that just keeps fetching would let a
younger instruction in ID read a stale register value because the
older producer has not yet reached WB. This is a classic
read-after-write (RAW) data hazard.

Penumbra/2's chosen response for GPR and SPR operands is **pure
stall**: detect the hazard in ID, stall the ID stage (and
back-pressure IF1/IF2) until the producer commits at WB, then issue.
No operand-forwarding network and no regfile write-through exist for
them. (NZCV is the one exception — it is forwarded, not stalled; see
[Flag (NZCV) hazard model](#flag-nzcv-hazard-model).) The scoreboard is
the mechanism that lets ID know when to stall and when to release.

Three constraints shape the scoreboard's design:

1. **Architectural register aliasing.** Architectural `R14` is
   physically `USP` when `SR.S = 0` and physically `SSP` when
   `SR.S = 1`. `RDSPR/WRSPR USP` reaches `USP` from either mode.
   The same physical storage has multiple ISA-level names.
2. **SPRs need hazard tracking too.** The TLB miss handler in
   `locore.S` uses `RDSPR/WRSPR SCRn` as spill slots in its hot
   path. Draining the pipeline on every `WRSPR` would be
   unacceptable. The scoreboard must therefore cover SPRs as
   ordinary scoreboardable entries, just like GPRs.
3. **Multiple in-flight writers must resolve to the youngest.** A
   chain of instructions can have several in-flight writers to the
   same physical entry at once (e.g. repeated updates to a loop
   accumulator). The reader of such an entry must observe the
   *youngest* writer's result, and writers must not have to serialize
   against each other.

The first constraint drives **physical-addressed** scoreboarding;
the second drives the **SPR coverage**. The third is satisfied *for
free* by in-order completion: because Penumbra/2 retires strictly
in program order, the youngest writer always lands last, so
last-writer-wins holds for every entry with no WAW stall. This is a
consequence of the in-order pipeline, not a mechanism the
scoreboard adds — see [Stall predicate](#stall-predicate). (Penumbra/2 therefore has only RAW
data hazards; WAW and WAR cannot occur.)

## Scoreboard storage

The scoreboard is a flat array of single valid bits, one per
physical scoreboardable entry. `valid = 1` means "no in-flight
instruction is writing this entry"; `valid = 0` means "an issued
instruction has cleared this entry but has not yet completed WB".

| Index | Physical entry | Notes |
|------:|----------------|-------|
| 0     | (unused, R0)   | R0 is never scoreboarded; reads return 0, writes are discarded |
| 1–13  | R1–R13         | General-purpose registers |
| 14    | USP            | `R14` in user mode; `RDSPR/WRSPR USP` from either mode |
| 15    | SSP            | `R14` in supervisor mode |
| 16    | ESR            | `RDSPR/WRSPR ESR`. Exception entry's save-state pulse writes ESR directly and bypasses the scoreboard. |
| 17    | EPC            | `RDSPR/WRSPR EPC`. Same exception-entry bypass as ESR. |
| 18    | SCR0           | `RDSPR/WRSPR SCR0` only |
| 19    | SCR1           | `RDSPR/WRSPR SCR1` only |
| 20    | SCR2           | `RDSPR/WRSPR SCR2` only |
| 21    | SCR3           | `RDSPR/WRSPR SCR3` only |

No part of SR appears in this table. The condition flags (NZCV) are
resolved by **forwarding**, not a stall, so they need no valid bit
([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)). The S and I bits
are serialized by drain-commit
([Control-state serialization: the S and I bits](#control-state-serialization-the-s-and-i-bits)).

R0 (entry 0) is reserved-unused in the array; the decoder never
produces index 0 as a source or destination, so its valid bit is
read-don't-care. The implementation may either omit it (entries
1..21, 21 bits of state) or include it as a tied-1 bit for indexing
simplicity (22 bits). The implementation choice is a synthesis
detail; the spec treats the array as having 21 live entries.

R15 (PC) is not in the scoreboard. ISA-level reads of R15 are
resolved by the decoder to the current PC value (or PC-relative
constants for `MOV R15, …` immediate forms; see
[instruction-set.md](../../system/instruction-set.md)), never from
the regfile. ISA-level writes to R15 are encoded as branches and
are not regfile writes. The scoreboard need not represent it.

**SR is handled entirely outside the scoreboard.** Its three logical
parts each have their own mechanism, and none is a scoreboard entry:

- **NZCV** (the condition flags) is **forwarded** MEM→EX / WB→EX, so a
  flag reader never stalls and needs no valid bit
  ([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)).
- **S and I** are **serialized by drain-commit** on every instruction
  that writes them. This is a correctness factoring, not an
  optimization: S and I are consumed by structures *outside* the
  pipeline's dataflow (the MMU and the IF1 IRQ logic), which a
  value-hazard mechanism — scoreboard *or* forward — cannot protect.
  [Control-state serialization: the S and I bits](#control-state-serialization-the-s-and-i-bits)
  develops this in full.

The two mechanisms are not interchangeable: forwarding answers "what is
the most recent NZCV value?" for an in-pipeline reader, while drain
serialization answers "is any younger instruction in flight under the
old mode?" for out-of-pipeline consumers. NZCV needs the former; S and I
need the latter — which is why NZCV can be forwarded and S/I cannot.

## Valid bit lifecycle

`valid[P]` is **derived combinationally** each cycle as *"no
instruction ahead of the ID instruction (in EX, MEM, or WB) has P
as its destination"* — not latched in a set/clear flip-flop. This
re-derive form (mandated in [Interaction with exception entry](#interaction-with-exception-entry) for flush-safety) is what
makes the lifecycle below behave correctly when several writers to
P are in flight at once: `valid[P]` only returns to 1 once the
*youngest* such writer has left WB. The set/clear description below
is the *logical* behavior the derivation produces, not a literal
flop.

For each physical entry P, the valid bit's logical lifecycle is:

1. **Reset.** With no instructions in flight, every `valid[P]`
   reads 1.
2. **Clear at ID issue.** When ID issues an instruction whose
   destination is P (i.e., when the instruction transitions from
   ID-held to ID/EX-register-written), P now has an in-flight
   writer, so `valid[P]` reads 0. Issue and the clear are the same
   event.
3. **Set when the last writer drains.** `valid[P]` reads 1 again
   only on the cycle after the *youngest* in-flight writer to P
   leaves WB — at which point the regfile holds that youngest
   value. If a single writer is in flight this is just "set at WB
   completion"; if several are in flight (e.g., a chain of writers
   to a loop accumulator) the earlier writers' WBs do not set it, because they
   are still ahead-of-reader writers until they drain. The bit is
   observable on the next cycle's ID stall predicate evaluation.
4. **Divmul second write.** Divmul writes two physical entries —
   `Rd` (low/quotient) and `Rdh` (high/remainder, from `IR[15:12]`)
   — through the **single** write port over two consecutive WB
   cycles (the register file has no second write port; see
   [Write port and divmul sequencing](./regfile.md#write-port-and-divmul-sequencing)).
   The divmul instruction occupies WB for both cycles and is an
   in-flight writer of both entries throughout, so under the
   re-derive model ([Interaction with exception entry](#interaction-with-exception-entry)) `valid[Rd]` and `valid[Rdh]` are
   both 0 until it leaves WB, then both return to 1 together. The
   low-then-high write order is for the shared write port, not for
   early wakeup — the held extra cycle freezes any consumer anyway.

**Exception entry bypass.** When the hardware exception save-state
pulse fires ([Interaction with drain-commit](#interaction-with-drain-commit)), the direct flop writes to ESR and EPC
bypass the scoreboard. The scoreboard is not touched on save-state.
This is safe because save-state happens during a pipeline drain in
which no in-flight instruction has ESR or EPC as a destination —
see [Interaction with drain-commit](#interaction-with-drain-commit) for the argument.

**No write-through.** A producer's destination is not visible to
the regfile read port until the WB cycle has retired. Specifically,
a younger instruction that reads the producer's destination *on
the same cycle the producer is in WB* still observes the old
regfile value. Issuing such a younger instruction requires that
its source's valid bit be 1, which it is not on the WB cycle (the
set fires at end-of-cycle). The younger instruction therefore
stalls one more cycle. This is the explicit "no write-through"
trade in [Decision 4](./design-decisions.md#4-hazard-handling-strategy);
gen2.5 adds a one-mux write-through that closes this cycle, alongside the
operand-forwarding network — see
[gen2.5 GPR forwarding and regfile write-through](#gen25-gpr-forwarding-and-regfile-write-through).

## Stall predicate

Let `S` be the set of physical source-register indices for the
instruction currently held in the ID register, and let `D` be its
destination index (or empty if the instruction has no GPR/SPR
destination). The ID stage stalls iff:

```
stall_id = (∃ p ∈ S : valid[p] = 0)        // RAW — the ONLY data hazard in this pipeline
```

where `valid[p] = 0` means "some instruction ahead of the ID
instruction (in EX, MEM, or WB) has `p` as its destination" — i.e.,
the value the reader wants is not yet in the regfile. `stall_id` is
registered into the ID/EX boundary's clock-enable chain via the
standard back-pressure mechanism
([Decision 10](./design-decisions.md#10-stall-propagation-policy-back-pressure)):
ID holds its input register; IF1/IF2 back-pressure; ID/EX gets a
bubble next cycle.

**Why RAW is the only data hazard — no WAW, no WAR.** WAW and WAR
hazards arise only when instructions can *complete out of order*.
Penumbra/2 completes strictly in program order: loads stall the
whole pipeline in MEM (back-pressure), divmul stalls EX, and no
instruction ever retires past an older one. Writes therefore reach
the regfile in program order, so the youngest writer to any entry
always lands last — **last-writer-wins for every register, for
free.** A second writer to an entry that already has an in-flight
writer does *not* stall: it simply issues behind the first and, by
in-order WB, commits after it. The scoreboard needs no
single-writer / WAW clause at all.

This correctness depends on `valid[p]` being **derived** from the
set of in-flight destinations each cycle (the re-derive model of
[Interaction with exception entry](#interaction-with-exception-entry)), not latched as
a single set-on-WB flip-flop. The derived form represents
arbitrarily many in-flight writers correctly: `valid[p]` returns to
1 only when the *last* (youngest) writer ahead of the reader has
left WB, at which point the regfile holds that youngest value. A
naive single flop "set on any WB" would instead be set prematurely
by an *older* writer's WB while a younger writer to the same entry
is still in flight — exposing a reader to the stale value. That is
an implementation defect, not a real hazard, and the re-derive
model avoids it; see [Interaction with exception entry](#interaction-with-exception-entry).

**Structural stalls** (divmul busy in EX, drain-commit drain,
cache miss) are not part of `stall_id` — they are EX/MEM stalls
that back-pressure ID through the cascade of
[Decision 10](./design-decisions.md#10-stall-propagation-policy-back-pressure).
A divmul in EX, for example, holds its successor in ID directly via
back-pressure; no scoreboard term is needed to keep a younger
writer to `Rd`/`Rdh` from issuing, because it physically cannot
advance into EX while the divmul occupies it. A *reader* of
`Rd`/`Rdh` is held by the ordinary RAW clause (their `valid` bits
stay 0 for the whole divmul iteration).

**Source-set composition.** The source set `S` contains only
scoreboarded (GPR/SPR) operands. Flag reads are **not** in `S` — they
are served by the bypass
([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)), so they never
contribute a scoreboard stall. Multi-source cases the decoder must
handle:

| Instruction class | Scoreboard sources `S` | Flag read? |
|-------------------|------------------------|:----------:|
| ALU `OP Rd, Ra, Rb` | `{Ra, Rb}` | — |
| `ADC`/`SBC Rd, Ra, Rb` | `{Ra, Rb}` | carry (forwarded) |
| ALU immediate `OP Rd, Ra, #imm` | `{Ra}` | — |
| `LD Rd, [Ra, #off]` | `{Ra}` | — |
| `ST Rs, [Ra, #off]` | `{Rs, Ra}` | — |
| `Bcc target` | `{}` | NZCV (forwarded) |
| `B[L] Rb` (register branch) | `{Rb}` | — |
| `RDSPR Rd, SPR` | `{SPR_phys}` (USP, ESR, EPC, SCRn). For `RDSPR Rd, SR`: `{}` | NZCV (for SR) |
| `WRSPR SPR, Rs` | `{Rs}` (and `D = SPR_phys`; `SPR` ∈ USP, ESR, EPC, SCRn — `WRSPR SR` is reserved) | — |
| `RDSYS Rd, sysreg_id` | `{}` (sysreg ID is encoded; not a regfile read) | — |
| `WRSYS sysreg_id, Rs` | `{Rs}` | — |
| MUL/DIV | `{Ra, Rb}` (writes a 2-entry destination, see below) | — |

For MUL/DIV the instruction writes **two** physical
entries, `Rd` (low/quotient) and `Rdh` (high/remainder). While the
divmul is in flight it is an in-flight writer of both, so both
`valid[Rd]` and `valid[Rdh]` read 0 and any *reader* of either
entry stalls (RAW) until the divmul completes. There is no
writer-side stall: a later instruction writing `Rd` or `Rdh` cannot
issue anyway, because the divmul stalls EX and back-pressures it in
ID ([Stall predicate](#stall-predicate), structural stalls).

## ISA → physical register mapping

The decoder produces three physical indices per cycle:
`phys_src_a`, `phys_src_b`, `phys_dst`. The mapping consumes the
opcode, register fields, and the current architectural `SR.S` bit.

Most mappings are trivial — architectural `R0..R13` map directly to
physical entries 0..13. The non-trivial cases are:

| ISA-level reference | Physical entry |
|---------------------|----------------|
| `R0`                | 0 (never used; reads zero, writes discarded) |
| `R1..R13`           | 1..13 |
| `R14` when `SR.S = 0` | **USP** (14) |
| `R14` when `SR.S = 1` | **SSP** (15) |
| `R15`               | (not scoreboarded; resolves to PC) |
| `RDSPR/WRSPR USP, …`  | **USP** (14), regardless of mode |
| `RDSPR/WRSPR ESR, …`  | 16 |
| `RDSPR/WRSPR EPC, …`  | 17 |
| `RDSPR SR`  | (not scoreboarded) — NZCV is forwarded ([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)); the S/I bits are read from committed SR. (`WRSPR SR` is reserved; the live S/I writers — `ERET`, `EI`/`DI` — serialize by drain-commit, see [Control-state serialization: the S and I bits](#control-state-serialization-the-s-and-i-bits).) |
| `RDSPR/WRSPR SCRn, …` | 18..21 |

The mapping is **combinational** in the ID stage; it is not
registered. This is acceptable for fmax because the SR.S bit is
quiescent ([Control-state serialization: the S and I bits](#control-state-serialization-the-s-and-i-bits)) — no in-flight instruction can change it
between this cycle's decode and next cycle's issue.

### The cross-bank SPR-USP case

In supervisor mode, `RDSPR/WRSPR USP` is the only way for the
kernel to access user-mode R14 (e.g., to save user state on
syscall entry). The decoder maps this access to physical entry
**USP** (14), not to R14's architectural mapping (which would be
SSP in supervisor mode).

This is what makes the scoreboard physical-addressed: a kernel
sequence

```
WRSPR USP, R1     ; write user stack pointer
…                 ; some instructions
ERET              ; drop to user mode
                  ; user code now does ADD R2, R14, …
```

must see the user-mode `R14` read in the post-ERET instruction
correctly RAW-stall on the WRSPR if the WRSPR has not yet reached
WB by the time the user instruction reaches ID. Both touch
physical entry USP. An architectural-name-indexed scoreboard would
see them as touching different entries (`USP` vs `R14`), miss the
hazard, and the user code could read a stale value.

In practice the ERET's drain-commit semantics
([Decision 9](./design-decisions.md#9-drain-commit-primitive))
ensure the WRSPR has reached WB before the ERET's PC redirect,
so this hazard never actually fires. But the scoreboard cannot
*rely* on drain-commit to fix correctness here — drain-commit is
about ordering ERET against younger insns, not about WRSPR against
younger insns. The physical-addressed scoreboard is the
mechanism that makes the general case (any WRSPR followed by any
USP-aliasing read, with or without intervening ERET) correct.

### SCRn coverage rationale

`SCR0..SCR3` are general-purpose SPR-scratch registers used heavily
by the TLB miss handler in
[`netbsd/sys/arch/penumbra/penumbra/locore.S`](../../../netbsd/sys/arch/penumbra/penumbra/locore.S)
`_real_miss_handler` as spill storage during the lookup. The handler
reads and writes them several times per miss. If WRSPR-SCRn were a
drain-commit instruction, every TLB miss would drain the pipeline
four to eight times — a catastrophic CPI hit on a path that is
already on the kernel hot-path radar.

Treating SCRn as ordinary scoreboardable entries keeps WRSPR-SCRn
non-drain-commit. The cost is the four extra scoreboard bits and
the decoder cycles to compute the index. The benefit is that the
miss handler's scratch use pipelines normally.

## The WRSPR-USP / R14 aliasing case (worked example)

The cleanest illustration of why physical addressing matters.
Suppose the kernel is preparing to ERET to a user context:

```
; supervisor mode, SR.S = 1
WRSPR  USP, R3      ; (A) load saved-user-SP into USP physical entry
ADD    R5, R6, R7   ; (B) unrelated work — fills the pipe
ERET                ; (C) returns to user mode; SR.S → 0
; user mode, SR.S = 0
ADD    R8, R14, R9  ; (D) read user stack pointer — physically reads USP
```

| Cycle | IF1 | IF2 | ID | EX | MEM | WB | Notes |
|-------|-----|-----|----|------|------|------|-------|
| 1 | A | — | — | — | — | — | |
| 2 | B | A | — | — | — | — | |
| 3 | C | B | A | — | — | — | A issues; `valid[USP] ← 0` |
| 4 | D | C | B | A | — | — | B issues |
| 5 | — | D | C | B | A | — | C enters ID; ERET is drain-commit, stalls in EX next cycle |
| 6 | — | — | D | C(drain) | B | A | A in WB; `valid[USP] ← 1` at end of cycle |
| 7 | — | — | D | C(drain) | — | B | C drains until MEM/WB empty |
| 8 | — | — | D | C(commit) | — | — | C commits in EX; SR.S → 0 |
| 9 | — | — | D | — | — | — | D's decode now sees SR.S=0 → maps R14 to USP. valid[USP] is 1. Issues. |

The point: D's decode in cycle 9 uses SR.S=0 (post-ERET) and maps
its `R14` source to physical USP. The scoreboard's valid bit for
USP has been set since cycle 6 (after A's WB). D issues without
stalling, reading the correct USP value that A wrote.

If the scoreboard had been indexed by ISA name, A would have
cleared `valid[USP]` and B/C would not have touched `valid[R14]`.
D's source `R14` would have been checked against the architectural
`R14` slot, which is clean — and D would have issued *before* A's
write had reached the regfile (had this been a tighter loop
without the ERET drain). Stale USP read.

Drain-commit on ERET happens to hide the bug in this specific
sequence. The next worked example removes the ERET to make the
hazard fire on a non-trivial sequence the architectural scoreboard
would miss:

```
; supervisor mode, SR.S = 1, with crash-recovery code that wants
; to inspect the user SP without leaving supervisor mode
WRSPR USP, R3         ; physical USP ← R3
RDSPR R5,  USP        ; read it back to verify
```

Both reference physical USP. Both run in supervisor mode, so
architectural `R14` is SSP, not USP. An ISA-name-indexed
scoreboard sees `WRSPR USP` and `RDSPR USP` as both touching
"USP" (because SPR encodings use the SPR name, not R14), so even
the broken design happens to catch this one — but the test that
*really* breaks the broken design is an interleaving where the
producer uses one ISA name and the consumer uses the other:

```
; supervisor mode
WRSPR USP, R3         ; physical USP ← R3  (cleared valid[USP] / valid[R14_arch])
; ...
; mode switch handled by trap, no drain-commit between
RDSPR R5,  USP        ; user-mode-style access in same window
```

This is the canonical reason for physical addressing. Even when
drain-commit hides the hazard in practice, the scoreboard's
*correctness invariant* must not depend on it.

## Control-state serialization: the S and I bits

The supervisor bit `S` and interrupt-enable bit `I` live in SR but
are **not** scoreboard entries. Neither is NZCV — but NZCV is kept out
of the scoreboard because it is *forwarded*
([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)), whereas S and I
are kept out because no value-hazard mechanism — scoreboard *or* forward
— can order them at all. This section explains why, and what orders them
instead.

### Why a scoreboard cannot protect S or I

A scoreboard valid bit answers exactly one question: *"has this
value reached the regfile read port yet?"* That is the only hazard
it expresses — a producer computes a value, a consumer reads it
through the regfile, stall the consumer until the value lands.

`S` and `I` are not consumed that way. They are consumed by
structures *outside* the pipeline's register dataflow:

- **S** is read by the MMU (to select user/supervisor permission
  checks and translation) and by the privilege checker, at IF1
  fetch and at MEM access — *while younger instructions are
  mid-execution*. A change to S doesn't just produce a new value
  for some future reader; it changes how every younger in-flight
  instruction is translated and privilege-checked. Those younger
  instructions were already fetched and decoded under the old S.
  The only correct response is to ensure **no younger instruction
  is in flight** when S changes — i.e., a pipeline flush, not a
  stall.
- **I** is read by the IF1 IRQ-acceptance logic. It does not change
  how any instruction *executes* (unlike S), only whether an
  interrupt is taken at an instruction boundary. But that consumer
  is still outside the regfile dataflow, so a value scoreboard
  can't express the ordering either.

In both cases the requirement is *serialization*, supplied by the
**drain-commit** primitive
([Decision 9](./design-decisions.md#9-drain-commit-primitive)),
not by a scoreboard valid bit.

### Every writer of S and I is drain-commit

| Writer | Writes | Ordering |
|--------|--------|----------|
| ERET | S, I, NZCV | drain-commit |
| `EI` / `DI` | I only | **drain-commit** (see 7.4) |
| Exception-entry save-state pulse | S=1, I=0 | post-drain (pipeline already flushed) |

There is no non-serializing writer of S or I. Consequently the
scoreboard can never reach a state where `valid[S]` or `valid[I]`
would be "pending" with a younger reader in flight — because the
act of writing S/I either drains the pipeline (drain-commit) or
fires only after it is already empty (exception entry). A
scoreboard bit for S or I would therefore be **dead logic**:
always restored by the drain before anyone could observe it
pending. Tracking it would not be incorrect, just inert — and inert
logic that looks load-bearing obscures the real mechanism, so gen2
omits it.

A *direct* status-register write (`WRSPR SR`) would be a fourth writer
in that table and would need the same drain-commit ordering — a direct
`SR.S` write feeds the MMU and the IF1 IRQ logic out of pipeline exactly
as ERET's does. gen2 **reserves** the `WRSPR SR` encoding rather than
implementing it: software changes S/I only via exception entry, ERET, and
`EI`/`DI`, and NZCV via flag-writing ALU ops, so a direct SR write is
never needed. Reserving the encoding removes the case instead of building
the serialization for it.

### SR.S quiescence for the decoder's R14 mapping

This is the one place the absence of S-scoreboarding could look
worrying, so it is worth stating explicitly. The decoder reads
`SR.S` *combinationally* in ID to map architectural `R14` to
physical USP or SSP ([ISA → physical register mapping](#isa--physical-register-mapping)). That read is **not** protected by
the scoreboard — it reads the architectural SR.S flop directly.
Its correctness instead rests on `SR.S` being quiescent: no
in-flight instruction may be *in the process* of changing `SR.S`
between cycle T (decode) and cycle T+1 (issue).

Drain-commit on every S-writer provides exactly that. Because ERET
and exception entry all drain (or post-drain), the
moment any instruction sits in ID the SR.S it reads is stable for
that instruction's entire pipeline residence. Without drain-commit
on the mode-changers, the decoder would need a re-mapping mechanism
(or a stall on every R14-touching instruction that follows a
mode-changer). The drain-commit decision buys this quiescence for
free — and note this is a *second*, independent reason S-changes
must drain, reinforcing 7.1: it is not only the MMU mid-execution,
it is also the decoder's own combinational read.

### Why EI/DI are drain-commit too

`I` does not change instruction execution, so EI/DI could in
principle be cheap single-cycle ops with the I-bit tracked some
other way. gen2 makes them **drain-commit** anyway, for three
reasons:

1. **`DI` correctness.** After `DI`, no younger instruction may be
   interrupted. If `DI` weren't serialized, an IRQ arriving while
   `DI` is in EX could be accepted at a younger instruction's
   boundary — i.e., taken while interrupts should already be off.
   That is a real correctness bug. Drain-commit defines a precise
   boundary: `DI` commits, the pipeline is drained, and only then
   do younger instructions fetch under I=0.

2. **`EI`'s one-instruction shadow must stay *architectural*.** The
   ISA mandates that `EI` enables interrupts with a one-instruction
   delay (the `ei_shadow` mechanism), so that `EI; ERET` returns
   atomically and so that `EI; NOP; DI` cracks exactly one
   interrupt window at a safe point. "One instruction" has to mean
   one *architectural* instruction, independent of pipeline depth —
   otherwise a programmer would have to pad with pipeline-depth-many
   NOPs and the ISA contract would leak the microarchitecture.
   Drain-commit guarantees it: because the pipeline is drained when
   `EI` commits, the shadowed instruction is unambiguously the
   single next fetch, so one `NOP` suffices on gen2 exactly as on
   the microcoded gen1. (Note `EI; DI` with *no* instruction
   between opens no window at all — the shadow covers the `DI`,
   which re-masks; this is correct delayed-EI behavior, not a bug.
   Full interrupt-recognition timing is in
   [exception-flow.md](./exception-flow.md).)

3. **Uniformity and cost.** Making EI/DI drain-commit means *every*
   instruction touching S or I serializes, so the scoreboard owns
   NZCV and nothing else of SR — one rule, no special case for I.
   The cost (~2 cycles per EI/DI) is irrelevant: EI/DI bracket
   critical sections and never sit on a truly hot inner loop.

Reclaiming the EI/DI cost (and the broader cheap-`spl` engineering)
is a gen2.5 concern, alongside forwarding.

## Flag (NZCV) hazard model

NZCV is **not** a scoreboard entry. The condition flags are resolved by
**forwarding** rather than by stalling a reader until the producer
commits ([Decision 12](./design-decisions.md#12-nzcv-flag-forwarding)).
This is the one place gen2 departs from pure stall; GPR and SPR operands
remain pure-stall.

**Why flags, and only flags, are forwarded.** Nearly every ALU op writes
NZCV, but a *writer* never stalls ([Stall predicate](#stall-predicate)),
so a stream of flag-writing ALU ops issues at one per cycle regardless.
The cost is entirely on the *readers* of NZCV, of which the ISA has
exactly two kinds:

- **`Bcc`** (conditional branches, cond `0001`–`1110`) — read NZCV to
  decide taken / not-taken.
- **`ADC` / `SBC`** — read the carry flag as an input
  (`Rd + Rs + C` / `Rd − Rs − ~C`).

A stall would make each of these wait ~3 cycles for the youngest
in-flight flag writer to commit; because branches are pervasive, that is
the single largest CPI source pure stall would impose. Forwarding
removes it, and because NZCV is 4 bits with one EX consumer, the path is
cheap.

**Producers and consumers:**

- **Producers** (decoder asserts `writes_flags`): flag-writing ALU ops
  (`ADD/SUB/CMP/AND/…`), the immediate ALU ops, `MUL/MULU/DIV/DIVU`
  (set N,Z; force C=V=0), and `ERET`. A producer's
  EX-computed NZCV rides the EX/MEM and MEM/WB pipeline registers in the
  `flag_value` field.
- **Consumers** (decoder asserts `reads_flags`): every `Bcc`, every
  `ADC`/`SBC`, and `RDSPR SR` (which returns NZCV among its bits).

### The flag bypass

The EX flag consumer selects its NZCV from a youngest-first mux:

```
flags = MEM.flag_value    if the MEM-stage insn writes_flags
      → WB.flag_value     else if the WB-stage insn writes_flags
      → architectural SR   otherwise
```

There is **no EX→EX leg.** A consumer in EX and its producer can never
occupy EX in the same cycle (one in-order pipe, distinct instructions),
so the tightest a producer can be is one stage ahead — in MEM — when the
consumer is in EX. MEM and WB are therefore the only forwarding sources,
and a producer's flags are always already computed (it has left EX) by
the time the consumer needs them, so the bypassed value is always valid.

### Architectural SR is the committed source of truth

The bypass's lowest-priority input is the architectural SR flag bits,
written at WB by the youngest committed flag writer. This is what makes
forwarding correct across flushes, with no scoreboard involvement:

- **After a taken-branch flush**, the surviving older instructions in
  MEM/WB commit their flags to SR normally; the flushed instructions
  are *younger* and wrote nothing. Refetched instructions forward from
  the draining MEM/WB survivors, or — once drained — from committed SR.
- **After a fault**, the save-state pulse snapshots `ESR ← SR` (the
  committed flags as of the instruction before the fault) and the
  handler runs with an empty pipe, reading committed SR.
- **After `ERET`**, `SR ← ESR` restores the flags; the drained pipe
  then forwards from committed SR.

**Flush-safety is structural.** Forwarding flows older→younger; a
flush only removes *younger* instructions. So if a flag producer is
flushed, every reader of it is younger and flushed too — a stranded
forward (a surviving reader whose producer vanished) cannot arise. The
scoreboard's re-derive machinery
([Interaction with exception entry](#interaction-with-exception-entry))
is not involved, because NZCV is not in the scoreboard.

### Last-writer-wins, without a tag

Multiple flag writers can be in flight at once; they retire in program
order, so the architectural SR ends up holding the youngest writer's
result, and the youngest-first bypass selects the youngest *in-flight*
writer ahead of any reader. The hardware never identifies which writer
is youngest — priority order (MEM before WB) and in-order commit supply
it. `writes_flags` and `reads_flags` are the only decoder bits needed.

**A flag reader does not stall on its producer.** `CMP; Bcc` (and
`ADD; ADC`) costs **zero** flag-hazard cycles:

```
CMP R1, R2       ; (1) writes NZCV
BEQ target       ; (2) reads NZCV
```

| Cycle | IF1 | IF2 | ID | EX | MEM | WB | Notes |
|-------|-----|-----|----|------|------|------|-------|
| 3 | — | BEQ | CMP | — | — | — | CMP in ID |
| 4 | — | — | BEQ | CMP | — | — | BEQ issues with **no stall** |
| 5 | — | — | — | BEQ | CMP | — | BEQ in EX, CMP in MEM → MEM→EX bypass feeds CMP's flags to BEQ |

BEQ resolves in EX against the forwarded flags. (If taken, the 3-bubble
flush still applies — that is the branch *control* hazard
([Decision 5](./design-decisions.md#5-branch-resolution-policy)), not
the flag *data* hazard, which forwarding has eliminated.)

## Interaction with drain-commit

Drain-commit instructions (ERET, WRSYS) stall in EX rather than
issue-stalling in ID. They do not interact with the scoreboard
beyond the normal source-read clearing rules:

- **ERET** reads no GPR sources (PC and SR come from EPC/ESR,
  which are direct EX-stage reads). It writes no GPR destination.
  Scoreboard is untouched.
- **WRSYS** reads one GPR source (the value to write to the
  sysreg). The decoder treats it as a normal RAW source; the
  scoreboard clears nothing because WRSYS has no scoreboarded
  destination (the sysreg sideband is not in the scoreboard).
- **EI / DI** (also drain-commit) write only the I bit, which is
  not scoreboarded. They clear nothing and set nothing in the
  scoreboard; their effect is ordered entirely by the drain.

Drain-commit's main contribution to the scoreboard is indirect:
it provides SR.S quiescence ([Control-state serialization: the S and I bits](#control-state-serialization-the-s-and-i-bits)) by ensuring no
mode-changing instruction is in flight while a younger instruction
decodes — and, dually, it is what lets S and I stay out of the
scoreboard entirely.

## Interaction with divmul

The MUL/DIV instructions execute as a single µop with a
multi-cycle EX iteration (~33 cycles). The divmul unit owns the
ALU during the iteration and produces two register results — `Rd`
(low half / quotient) and `Rdh` (high half / remainder) — which are
written through the regfile's **single** write port at WB over two
consecutive cycles (see
[Write port and divmul sequencing](./regfile.md#write-port-and-divmul-sequencing)).
MUL/DIV also set NZCV (N,Z; C=V=0); like every flag producer those flags
are forwarded, not scoreboarded
([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)), so only the two
GPR destinations below concern the scoreboard.

Scoreboard interaction:

1. **At ID issue** the decoder clears `valid[Rd]` *and*
   `valid[Rdh]` together. Both must be valid pre-issue ([Stall predicate](#stall-predicate)
   stall predicate, with the pair-destination form).
2. **During divmul busy** the EX stage holds the instruction; the
   pipeline back-pressures upstream as for any EX stall.
   `valid[Rd]` and `valid[Rdh]` remain 0 throughout the divmul
   iteration.
3. **At writeback** the instruction occupies WB for two consecutive
   cycles — writing `Rd` (low) then `Rdh` (high) through the single
   port — holding the pipeline one extra cycle for the second write.
   It is an in-flight writer of both entries the whole time, so
   `valid[Rd]` and `valid[Rdh]` both clear at issue and both return
   to 1 together when it leaves WB ([Interaction with exception entry](#interaction-with-exception-entry)'s re-derive model).

Divide-by-zero (`DIV`/`DIVU` with `Rs = 0`) raises `VEC_ARITH`
(vector slot 10); that fault propagates through the normal
precise-exception mechanism in
[exception-flow.md](./exception-flow.md), flushing
the divmul's writes by virtue of the WB-stage fault-commit flush
rule (rather than by special handling in the scoreboard).

## Interaction with exception entry

When the precise-exception machinery commits a fault at WB
(see [exception-flow.md](./exception-flow.md)), it triggers:

1. A **fault-commit flush** of every younger in-flight insn
   (IF1, IF2, ID, EX, MEM).
2. A **one-cycle save-state pulse**: direct flop writes of
   `EPC ← faulting_PC`, `ESR ← SR`, with `SR.S ← 1` and
   `SR.I ← 0`. R14 banks to SSP.
3. A **vector-fetch FSM** in IF that drives the indirect load
   from `vec_num << 2` to PC, completing in a few cycles.

The scoreboard's role across this sequence:

- **Flushed instructions clear nothing.** The fault-commit
  flush signal forces ID/EX, EX/MEM, MEM/WB to bubble at the
  next clock edge. Any valid-bit clears those flushed
  instructions had performed at their issue cycle are **not
  rolled back** — that is, the scoreboard treats issued
  instructions as having owned their destination valid bit, even
  after flush.
- **This is safe** because a flushed instruction's destination
  has not been written and never will be; the valid bit will be
  restored when the *next* instruction to write that destination
  reaches WB. In the meantime no instruction reads the destination
  (because the trap handler's code runs after the flush and uses
  whatever scoreboard state it inherits).
- **Wait — is that actually safe?** Consider: an instruction
  issues, clears `valid[R5]`, then a younger instruction faults
  before the issuing instruction reaches WB. The flush discards
  the issuing instruction along with the faulting one. The
  scoreboard now has `valid[R5] = 0` permanently — no one will
  ever set it.

  **Resolution:** the scoreboard must restore the valid bits of
  flushed instructions that have not yet reached WB. The cleanest
  mechanism is to **re-derive the scoreboard state** from the
  set of in-flight instructions at every cycle, rather than
  treating it as flip-flops latched on clear/set events. The
  scoreboard is then a function of "which physical entries are
  destinations of EX/MEM/WB instructions plus the
  about-to-issue ID instruction." A fault-commit flush
  empties those stages, so all valid bits return to 1
  automatically.

  Implementation note: gen2's scoreboard is small enough (22 bits)
  that the "re-derive every cycle" form is cheap. The
  `id_stage.sv` design should use that form rather than separate
  set/clear flip-flops, so that flush handling is implicit.

- **Save-state's direct ESR/EPC writes bypass the scoreboard.**
  No in-flight instruction at the moment of save-state can have
  ESR or EPC pending, because save-state fires only after the
  fault-commit flush has emptied the pipeline of younger insns
  and the faulting insn has already reached WB. The bypass is
  therefore not a hazard — it is just a state update outside the
  scoreboard's purview.

## Worked stall examples

These complement the examples in
[pipeline-stages.md §Cycle-accurate timing examples](./pipeline-stages.md#cycle-accurate-timing-examples)
by focusing on scoreboard-driven stalls specifically.

### Example A: Pure RAW (already in pipeline-stages.md, summarized)

```
ADD R1, R2, R3       ; (1) producer
ADD R4, R1, R5       ; (2) consumer (RAW on R1)
```

(2) stalls in ID for 3 cycles. See
[pipeline-stages.md Example 1](./pipeline-stages.md#example-1-scoreboard-raw-stall-on-independent-alu-pair).

### Example B: two writers + a reader (no WAW stall; reader waits via re-derive)

This is the case that *looks* like it needs a WAW stall but does
not, because completion is in-order.

```
LD  R1, [R2, #0]    ; (1) writer to R1 — misses in MEM, long
ADD R1, R3, R4      ; (2) second writer to R1 (no RAW on R1)
SUB R5, R1, R6      ; (3) reader of R1 — must get ADD's value
```

| Cycle | IF1 | IF2 | ID | EX | MEM | WB | Notes |
|-------|-----|-----|----|------|------|------|-------|
| 3 | — | SUB | ADD | LD | — | — | LD in EX; ADD issues **without stalling** (it is a writer, not a reader of R1) |
| 4 | — | — | SUB (stall) | ADD | LD (miss) | — | SUB reads R1, two writers ahead → `valid[R1]=0` → stall. ADD held in EX behind the stalled LD (back-pressure) |
| K | — | — | SUB (stall) | ADD | bubble | LD | LD finally drains to WB. `valid[R1]` still 0 — **ADD is still an in-flight writer** |
| K+1 | — | — | SUB (stall) | — | ADD | — | ADD advances; still ahead of SUB |
| K+2 | — | — | SUB (stall) | — | — | ADD | ADD in WB; R1 ← ADD's value at end of cycle |
| K+3 | — | — | SUB (issue) | — | — | — | `valid[R1]=1` (no writer ahead) → SUB issues, reads ADD's value |

The key rows are K and K+1: LD's WB does **not** release SUB,
because the re-derive scoreboard sees ADD still ahead. SUB waits
for the *youngest* writer (ADD), exactly as last-writer-wins
requires — and ADD never had to stall. A naive single-flop "set on
any WB" would have wrongly released SUB at cycle K (LD's WB),
handing it the stale load value; the derived `valid[R1]` is what
prevents that. No WAW stall is involved anywhere.

### Example C: Cross-bank SPR-USP hazard

```
; supervisor mode
WRSPR USP, R5        ; (1) writes physical USP
RDSPR R6,  USP       ; (2) reads physical USP — direct RAW
```

(2) RAW-stalls on `valid[USP]` for 3 cycles. The architectural
scoreboard would not have caught this; physical addressing does.

### Example D: SCRn pipelining (no extra stall)

```
WRSPR SCR0, R1       ; (1) save
WRSPR SCR1, R2       ; (2) save
RDSPR R3,   SCR0     ; (3) restore — RAW on SCR0
```

(2) does not stall on (1) (different physical entries). (3)
RAW-stalls on `valid[SCR0]` for 3 cycles. This is the pattern in
`_real_miss_handler`; the per-entry tracking keeps it as fast as
GPR-equivalent code.

### Example E: Divmul pair-destination stall

```
MUL R1, R2, R4       ; (1) writes Rd=R1 (low half), Rdh=R4 (high half); ~33 cycles
ADD R5, R4, R6       ; (2) RAW on R4 (the high half)
```

(2) stalls in ID until the divmul completes ~33 cycles later, when
`valid[R4]` is set. (1) clears both `valid[R1]` and `valid[R4]` at
issue, so any read of either stalls until the divmul EX-completes.

## gen2.5 GPR forwarding and regfile write-through

> **Applies to:** Penumbra/2.5 only. gen2 keeps the pure-stall model
> above unchanged; this section is the gen2.5 delta.

gen2 resolves every GPR/SPR RAW hazard by stalling the reader in ID until
its producer commits at WB ([Stall predicate](#stall-predicate)). gen2.5
keeps the scoreboard but adds an operand-forwarding network and a regfile
write-through, so a reader issues as soon as its producer's value is
*reachable*, not when it is *committed*. The `valid` vector is unchanged;
what changes is that `valid[p] = 0` no longer forces a stall when the value
can be forwarded.

NZCV is already forwarded in gen2
([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)); this is the same
mechanism widened to 32-bit GPR operands. The asymmetry that makes it more
than a copy of the flag bypass is the **read stage**: flags are read in EX,
GPR operands in ID — a stage earlier — so a GPR dependency spans one extra
hop and needs three coverage points instead of two.

### Coverage by producer distance

A consumer reads its operands in ID and uses them in EX one cycle later.
For a producer `d` instructions ahead (in-order, no intervening stall):

| `d` | Producer stage when consumer is in EX | Mechanism | Source register |
|----:|---------------------------------------|-----------|-----------------|
| 1 | MEM | EX→EX forward (ALU/link result) | EX/MEM |
| 2 | WB | MEM→EX forward (loaded / older result) | MEM/WB |
| 3 | retiring as the consumer reads in ID | WB→ID write-through | regfile write port |
| ≥4 | retired | committed regfile read | regfile |

The two forwards feed the EX operand inputs from a youngest-first mux
(EX/MEM beats MEM/WB), exactly like the flag bypass. The write-through is a
mux on the regfile read port: a read of the entry being written this cycle
returns the write data, gated off entry 0 (R0 reads zero).

### What still stalls — load-use and RDSYS

A load and an RDSYS produce their value in **MEM**, not EX, so the value is
absent from the EX/MEM register and present only once the producer reaches
MEM/WB. A consumer one instruction behind such a producer (the producer in
EX, the consumer about to enter EX) therefore cannot be served by the `d=1`
forward. It stalls one cycle in ID — the **load-use interlock** — after
which the producer is in MEM/WB and the `d=2` forward delivers the value.
This is the single residual data stall: **1-cycle load-use distance**, loads
and RDSYS alike. (The MEM back-pressure already holds the consumer for the
producer's own multi-cycle MEM access; the interlock adds only the one
alignment cycle.)

### Relaxed stall predicate

A read source `p` (enabled) stalls iff it has an in-flight writer the
forwarding network cannot reach:

```
stall(p) = reads(p) & ~valid[p] & ~forwardable(p)

forwardable(p) = ~(p is the EX-stage writer, and that writer is a load/RDSYS)  // load-use
               & ~(p is a divmul aux (Rdh) destination)                        // deferred
               & ~(p is an SPR-file source: EPC / ESR / SCRn)                   // deferred
               & ~(the instruction is WRSYS and p is its value operand)         // bypasses the network
```

Everything `valid` would otherwise stall on — an EX-stage ALU producer
(`d=1`), any MEM-stage producer (`d=2`), any WB-stage producer (`d=3`) — is
now reachable, so `forwardable(p)` is true and the reader issues. The
relaxed predicate is a strict subset of the gen2 predicate: forwarding only
*removes* stalls, never adds one — a worthwhile assertion, with one care: the
destination-match legs (load-use, divmul-aux) must skip **entry 0 (R0)**.
R0 is never a real producer (writes discarded, reads zero), and the scoreboard
ties `valid[R0]=1`; a divmul that discards its high half targets `Rdh = R0`, so
without the guard the aux leg would match every R0 reader and stall it where the
scoreboard does not — breaking the subset and over-stalling.

**WRSYS is the fourth deferred case, and for a different reason than the SPR-file
reads.** A WRSYS composes its sysreg-write datum in the integration from the
*registered* ID/EX operand (`o_sys_wdata = idex_op_b`), which never passes
through the EX forward muxes. So its value operand cannot be forwarded at all;
it keeps the conservative stall so the registered operand holds the committed
value by the time WRSYS drain-commits. (Without this, a WRSYS of a
freshly-computed value — e.g. the TLB miss handler installing a just-built PTE
in hot code — writes stale data and the handler re-faults forever.)

### Forward-source gating

- **EX/MEM source** (the slot in MEM): contributes only when it is a valid,
  non-faulting GPR writer **whose result is in the register** — i.e. not a
  load or RDSYS (those carry an address/EA there, not the value). This is the
  same condition the load-use interlock checks one stage earlier.
- **MEM/WB source** (the slot in WB): contributes when it is a valid,
  non-faulting GPR writer; loads and RDSYS qualify here, because MEM has
  produced the value into the MEM/WB register.

USP (entry 14) is regfile-backed, so it forwards and write-throughs like any
GPR. The SPR-file/scratch-file sources (EPC/ESR/SCRn) read through a separate
path and keep the conservative stall; widening forwarding to them is deferred
(it needs a write-through on each of the three SPR backends). The divmul aux
(Rdh) keeps its scoreboard stall — negligible after a ~33-cycle op.

### Operand capture across an EX hold

The forward is combinational and re-evaluated every cycle, and it is live only
while the producer sits in MEM/WB. The textbook lockstep — consumer in EX
exactly when the producer is one or two stages ahead — guarantees that on the
consumer's *first* EX cycle. But a consumer can be **held in EX longer than that
window** by a *downstream* MEM stall it did not cause: a function prologue's
store burst (`sub sp; stw …; stw …`), or a slow load sitting ahead of it. While
the consumer waits, the older producer keeps advancing and **retires from WB**.
A naively re-evaluated forward then misses (the producer is gone) and reverts to
the stale registered ID operand, which is latched when the consumer finally
advances — a wrong result, on common code, that the single-memory-op test
(`memcpy`) never triggers.

EX therefore **captures** each forwarded operand instead of re-deriving it: the
forward's fallback is the freshly-read ID operand on the slot's first EX cycle
(flagged by the registered ID-issue signal) and the previously-resolved operand
on every later, held cycle. Because the retained register tracks the forward, a
value captured while the producer was in MEM/WB persists after it drains. A
store's `op_b` is its immediate offset, which never drains, so only the port-B
*register* (the stored datum) is captured alongside `op_a`.

### Store data and the operand-B register

The regfile port-B read feeds `op_b` for register/register ALU ops and
`store_data` for stores (a store's `op_b` is its immediate offset).
Forwarding follows the *value*, not the port: the forwarded port-B register
lands on `op_b` for non-stores and on `store_data` for stores, so a store of
a just-produced value (`ADD R1,…; STW R1,[R2]`) forwards correctly while the
store's immediate offset is left intact.

### Worked examples

**Back-to-back ALU — zero stall (`d=1`, EX→EX).**

```
ADD R1, R2, R3       ; (1) producer
ADD R4, R1, R5       ; (2) consumer (RAW on R1)
```

| Cycle | ID | EX | MEM | WB | Notes |
|-------|----|----|-----|----|-------|
| 3 | (2) | (1) | — | — | (1) in EX; (2) reads R1 — `valid[R1]=0` |
| 4 | — | (2) | (1) | — | (1) in MEM; EX/MEM→EX forward feeds (1)'s result to (2). No stall. |

In gen2 this pair costs 3 stall cycles ([Example A](#example-a-pure-raw-already-in-pipeline-stagesmd-summarized)); in gen2.5 it costs zero.

**Load-use — one stall (`d=1` load, then `d=2` MEM→EX).**

```
LDW R1, [R2, #0]     ; (1) load
ADD R3, R1, R4       ; (2) consumer (RAW on R1)
```

| Cycle | ID | EX | MEM | WB | Notes |
|-------|----|----|-----|----|-------|
| 3 | (2) | (1) | — | — | (1) is a load in EX → load-use interlock stalls (2) |
| 4 | (2) | — | (1) | — | (1) launches its access (MEM back-pressures; (2) held) |
| 5 | — | (2) | — | (1) | (1) reaches WB; MEM/WB→EX forward feeds the loaded value to (2) |

The net cost over an independent successor is the one interlock cycle.

## Verification considerations

The scoreboard's correctness invariants — physical addressing,
last-writer-wins under in-order completion, flush-aware
re-derivation, SR.S quiescence — are small in count but easy to get
wrong individually. Suggested testbench coverage for
`tb_scoreboard.cpp` (or however the unit test ends up packaged):

1. **Basic RAW.** Sequential ADD producer/consumer for every
   physical entry (1..21). Confirm 3-cycle stall.
2. **Last-writer-wins, no writer stall.** Two writers to the same
   entry with no intervening reader (the second a flag/ALU op);
   confirm the second issues **without stalling** on the first, and
   that a *reader* placed after both stalls until the *second*
   writer's WB and reads the second's value (Example B). The
   negative assertion — that the reader is NOT released by the
   first writer's WB — is the one that catches a premature-set
   implementation.
3. **Cross-bank USP.** WRSPR-USP followed by RDSPR-USP in
   supervisor mode, and (separately) WRSPR-USP in supervisor mode
   followed by R14 read after a synthetic mode flip. Confirm
   hazard caught.
4. **Pair destination.** MUL followed by ADD reading either Rd or
   Rdh; confirm both reader forms stall for the divmul duration.
5. **Flush recovery.** Issue a producer, follow with a younger
   instruction that takes a synthetic fault, confirm the
   producer's destination valid bit is correctly set after the
   exception entry's pipeline drain.
6. **SR.S quiescence.** ERET that changes SR.S with an R14-using
   instruction immediately after; confirm the decoder reads the
   post-ERET SR.S.
7. **Flags are out of scope for the scoreboard.** NZCV is forwarded,
   not scoreboarded, so it does not appear in `tb_scoreboard`. Flag
   forwarding — that `CMP; Bcc` and `ADD; ADC` incur no stall, that the
   consumer observes the youngest in-flight writer's flags, and that the
   architectural SR is the post-flush fallback — is verified against the
   flag bypass and the EX stage
   ([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)).

A randomized stream test (random GPR/SPR mix with backpressure
events injected) is also recommended once the directed tests pass.

## Cross-references

- [Decision 4](./design-decisions.md#4-hazard-handling-strategy)
  — rationale for pure stall (GPR/SPR) and the unified physical
  scoreboard.
- [Decision 12](./design-decisions.md#12-nzcv-flag-forwarding)
  — rationale for forwarding NZCV instead of scoreboarding it.
- [Decision 9](./design-decisions.md#9-drain-commit-primitive)
  — drain-commit, which provides SR.S quiescence for the
  scoreboard.
- [Decision 10](./design-decisions.md#10-stall-propagation-policy-back-pressure)
  — back-pressure stall propagation; what `stall_id` does to
  upstream stages.
- [pipeline-stages.md §Stall sources](./pipeline-stages.md#stall-sources)
  and [§Cycle-accurate timing examples](./pipeline-stages.md#cycle-accurate-timing-examples).
- [exception-flow.md](./exception-flow.md) — fault commit and
  save-state's bypass of the scoreboard.
- [control-decode.md](./control-decode.md) — the decoder's
  control-vector layout, including the `phys_src_*`/`phys_dst`
  fields and the `writes_flags`/`reads_flags` bits that drive the flag
  bypass ([Flag (NZCV) hazard model](#flag-nzcv-hazard-model)).
- [regfile.md](./regfile.md) — the 2R/1W regfile with R14
  banking. *(To be written.)*
- [instruction-set.md](../../system/instruction-set.md) — the
  ISA-level reference for SPR encodings (USP, ESR, EPC, SR,
  SCRn).
