# Penumbra/2 — Hazard Model

This document specifies the data-hazard handling mechanism for the
Penumbra/2 pipeline: the scoreboard, the stall predicate, the
ISA → physical register mapping, the interaction with flags,
drain-commit, divmul, and exception entry. It is the reference for
implementing `id_stage.sv` and the standalone `scoreboard.sv` (if
factored out).

Higher-level design rationale lives in
[design-decisions.md §4](./design-decisions.md#4-hazard-handling-strategy).
This doc is the implementation contract: it states *what the
scoreboard does*, not *why we picked it over forwarding*.

## Scope

Covered:

- The scoreboard's storage, addressing, and update rules.
- The ID-stage stall predicate, stated formally.
- The decoder's ISA → physical mapping (including the R14
  banking case and SPR-USP cross-bank access).
- Interaction with drain-commit, divmul, and exception entry.
- Flag-hazard (NZCV) semantics (Section 8).
- Worked examples of every distinct hazard scenario.

Out of scope:

- Per-stage pipeline-register layout (see
  [pipeline-stages.md](./pipeline-stages.md#inter-stage-pipeline-registers)).
- Branch-resolution flush penalty (see
  [Decision 5](./design-decisions.md#5-branch-resolution-policy)).
- Cache-miss and sysreg-sideband stall semantics
  (see [Decision 11](./design-decisions.md#11-bram-backed-caches-with-single-mem-stall)
  and [sysregs.md](../../system/sysregs.md)).
- Forwarding paths — gen2 has none by design; gen2.5 will add
  WB→ID write-through and EX→EX flag forwarding.

## 1. The hazard problem in gen2

Penumbra/2 overlaps up to six instructions across IF1, IF2, ID, EX,
MEM, and WB. A naive pipeline that just keeps fetching would let a
younger instruction in ID read a stale register value because the
older producer has not yet reached WB. This is a classic
read-after-write (RAW) data hazard.

Penumbra/2's chosen response is **pure stall**: detect the hazard
in ID, stall the ID stage (and back-pressure IF1/IF2) until the
producer commits at WB, then issue. No forwarding network exists;
no regfile write-through exists. The scoreboard is the mechanism
that lets ID know when to stall and when to release.

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
   same physical entry at once — most sharply NZCV, which nearly
   every ALU op writes. The reader of such an entry must observe
   the *youngest* writer's result, and writers must not have to
   serialize against each other (serializing NZCV writers would
   collapse the pipeline, since almost every instruction is one).

The first constraint drives **physical-addressed** scoreboarding;
the second drives the **SPR coverage**. The third is satisfied *for
free* by in-order completion: because Penumbra/2 retires strictly
in program order, the youngest writer always lands last, so
last-writer-wins holds for every entry with no WAW stall. This is a
consequence of the in-order pipeline, not a mechanism the
scoreboard adds — see Section 4. (Penumbra/2 therefore has only RAW
data hazards; WAW and WAR cannot occur.)

## 2. Scoreboard storage

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
| 18    | NZCV           | The condition flags carried in SR. Written by flag-writing ALU ops, `WRSPR SR`, and `ERET`; read by `Bcc` and `RDSPR SR`. **Only the NZCV flags are scoreboard-tracked; the S and I bits of SR are not** — see Section 7. |
| 19    | SCR0           | `RDSPR/WRSPR SCR0` only |
| 20    | SCR1           | `RDSPR/WRSPR SCR1` only |
| 21    | SCR2           | `RDSPR/WRSPR SCR2` only |
| 22    | SCR3           | `RDSPR/WRSPR SCR3` only |

R0 (entry 0) is reserved-unused in the array; the decoder never
produces index 0 as a source or destination, so its valid bit is
read-don't-care. The implementation may either omit it (entries
1..22, 22 bits of state) or include it as a tied-1 bit for indexing
simplicity (23 bits). The implementation choice is a synthesis
detail; the spec treats the array as having 22 live entries.

R15 (PC) is not in the scoreboard. ISA-level reads of R15 are
resolved by the decoder to the current PC value (or PC-relative
constants for `MOV R15, …` immediate forms; see
[instruction-set.md](../../system/instruction-set.md)), never from
the regfile. ISA-level writes to R15 are encoded as branches and
are not regfile writes. The scoreboard need not represent it.

**SR is deliberately split.** Of the three logical parts of SR —
the NZCV flags, the supervisor bit S, and the interrupt-enable bit
I — only NZCV is a scoreboard entry (index 18). The S and I bits
are **not** value-hazard-tracked; their ordering is provided by
drain-commit serialization on every instruction that writes them.
This is not an optimization — it is a correctness factoring,
because S and I are consumed by structures *outside* the pipeline's
dataflow (the MMU and the IF1 IRQ logic) that a regfile-read
scoreboard cannot protect. Section 7 develops this in full; the
short version is that a scoreboard valid bit answers only "has this
value reached the regfile yet?", which is the wrong question for
control state that the MMU samples mid-execution.

## 3. Valid bit lifecycle

`valid[P]` is **derived combinationally** each cycle as *"no
instruction ahead of the ID instruction (in EX, MEM, or WB) has P
as its destination"* — not latched in a set/clear flip-flop. This
re-derive form (mandated in Section 11 for squash-safety) is what
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
   completion"; if several are in flight (e.g., a chain of NZCV
   writers) the earlier writers' WBs do not set it, because they
   are still ahead-of-reader writers until they drain. The bit is
   observable on the next cycle's ID stall predicate evaluation.
4. **Divmul second write.** When the divmul unit drives its
   `o_result_hi` write through write-port 2, the corresponding
   physical entry (`Rdh` in the encoded instruction) also has its
   valid bit set. This is a separate clear/set pair from `Rd`'s
   normal lifecycle: divmul clears both `valid[Rd]` and
   `valid[Rdh]` at issue, and sets each one independently as its
   write port completes. (In practice both writes happen on the
   same cycle in gen2's divmul implementation, but the spec
   permits them to be staggered if a future divmul redesign needs
   it.)

**Exception entry bypass.** When the hardware exception save-state
pulse fires (Section 9), the direct flop writes to ESR and EPC
bypass the scoreboard. The scoreboard is not touched on save-state.
This is safe because save-state happens during a pipeline drain in
which no in-flight instruction has ESR or EPC as a destination —
see Section 9 for the argument.

**No write-through.** A producer's destination is not visible to
the regfile read port until the WB cycle has retired. Specifically,
a younger instruction that reads the producer's destination *on
the same cycle the producer is in WB* still observes the old
regfile value. Issuing such a younger instruction requires that
its source's valid bit be 1, which it is not on the WB cycle (the
set fires at end-of-cycle). The younger instruction therefore
stalls one more cycle. This is the explicit "no write-through"
trade in [Decision 4](./design-decisions.md#4-hazard-handling-strategy);
gen2.5 will add a one-mux write-through that closes this cycle.

## 4. Stall predicate

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
[Section 11](#11-interaction-with-exception-entry)), not latched as
a single set-on-WB flip-flop. The derived form represents
arbitrarily many in-flight writers correctly: `valid[p]` returns to
1 only when the *last* (youngest) writer ahead of the reader has
left WB, at which point the regfile holds that youngest value. A
naive single flop "set on any WB" would instead be set prematurely
by an *older* writer's WB while a younger writer to the same entry
is still in flight — exposing a reader to the stale value. That is
an implementation defect, not a real hazard, and the re-derive
model avoids it; see Section 11.

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

**Source-set composition.** Most instructions have at most two
source registers. Multi-source cases the decoder must handle:

| Instruction class | Sources contributed to S |
|-------------------|--------------------------|
| ALU `OP Rd, Ra, Rb` | `{Ra, Rb}` |
| ALU immediate `OP Rd, Ra, #imm` | `{Ra}` |
| `LD Rd, [Ra, #off]` | `{Ra}` |
| `ST Rs, [Ra, #off]` | `{Rs, Ra}` |
| `Bcc target` | `{NZCV}` (the flag entry, index 18) — see Section 8 |
| `B[L] Rb` (register branch) | `{Rb}` |
| `RDSPR Rd, SPR` | `{SPR_phys}` (e.g., USP, ESR, SCR0…). For `RDSPR Rd, SR` the source is `{NZCV}` — the S/I bits it also returns are serialized by drain-commit, not scoreboarded (Section 7). |
| `WRSPR SPR, Rs` | `{Rs}` (and `D = SPR_phys`). `WRSPR SR` writes NZCV (`D = NZCV`) and is itself drain-commit. |
| `RDSYS Rd, sysreg_id` | `{}` (sysreg ID is encoded; not a regfile read) |
| `WRSYS sysreg_id, Rs` | `{Rs}` |
| MUL/DIV | `{Ra, Rb}` (writes a 2-entry destination, see below) |

For MUL/DIV the instruction writes **two** physical
entries, `Rd` (low/quotient) and `Rdh` (high/remainder). While the
divmul is in flight it is an in-flight writer of both, so both
`valid[Rd]` and `valid[Rdh]` read 0 and any *reader* of either
entry stalls (RAW) until the divmul completes. There is no
writer-side stall: a later instruction writing `Rd` or `Rdh` cannot
issue anyway, because the divmul stalls EX and back-pressures it in
ID (Section 4, structural stalls).

## 5. ISA → physical register mapping

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
| `RDSPR/WRSPR SR,  …`  | 18 (NZCV). The flag bits are the scoreboarded part; the S/I bits `RDSPR SR` reads and `WRSPR SR` writes are serialized by drain-commit (Section 7). `WRSPR SR` is drain-commit; `EI`/`DI` are drain-commit and touch only the I bit (no scoreboard entry). |
| `RDSPR/WRSPR SCRn, …` | 19..22 |

The mapping is **combinational** in the ID stage; it is not
registered. This is acceptable for fmax because the SR.S bit is
quiescent (Section 7) — no in-flight instruction can change it
between this cycle's decode and next cycle's issue.

### 5.1 The cross-bank SPR-USP case

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

### 5.2 SCRn coverage rationale

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

## 6. The WRSPR-USP / R14 aliasing case (worked example)

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

## 7. Control-state serialization: the S and I bits

The supervisor bit `S` and interrupt-enable bit `I` live in SR but
are **not** scoreboard entries. This section explains why a value
scoreboard is the wrong mechanism for them, and what orders them
instead.

### 7.1 Why a scoreboard cannot protect S or I

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

### 7.2 Every writer of S and I is drain-commit

| Writer | Writes | Ordering |
|--------|--------|----------|
| ERET | S, I, NZCV | drain-commit |
| WRSPR SR | S, I, NZCV | drain-commit |
| `EI` / `DI` | I only | **drain-commit** (see 7.4) |
| Exception-entry save-state pulse | S=1, I=0 | post-drain (pipeline already squashed) |

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

### 7.3 SR.S quiescence for the decoder's R14 mapping

This is the one place the absence of S-scoreboarding could look
worrying, so it is worth stating explicitly. The decoder reads
`SR.S` *combinationally* in ID to map architectural `R14` to
physical USP or SSP (Section 5). That read is **not** protected by
the scoreboard — it reads the architectural SR.S flop directly.
Its correctness instead rests on `SR.S` being quiescent: no
in-flight instruction may be *in the process* of changing `SR.S`
between cycle T (decode) and cycle T+1 (issue).

Drain-commit on every S-writer provides exactly that. Because ERET,
WRSPR SR, and exception entry all drain (or post-drain), the
moment any instruction sits in ID the SR.S it reads is stable for
that instruction's entire pipeline residence. Without drain-commit
on the mode-changers, the decoder would need a re-mapping mechanism
(or a stall on every R14-touching instruction that follows a
mode-changer). The drain-commit decision buys this quiescence for
free — and note this is a *second*, independent reason S-changes
must drain, reinforcing 7.1: it is not only the MMU mid-execution,
it is also the decoder's own combinational read.

### 7.4 Why EI/DI are drain-commit too

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

## 8. Flag (NZCV) hazard model

NZCV is its own scoreboard entry (index 18). It is the *only* part
of SR the scoreboard tracks — S and I are serialized by
drain-commit (Section 7), so they never appear here.

**Producers and consumers of NZCV:**

- **Writers (become an in-flight writer of NZCV at ID issue):**
  flag-writing ALU ops (most of `ADD/SUB/CMP/AND/…`), `WRSPR SR`,
  `ERET`. The decoder asserts a `writes_flags` control bit for
  these.
- **Readers (place NZCV in their source set `S`):** every `Bcc`,
  and `RDSPR SR` (which returns NZCV among other bits). The decoder
  asserts a `reads_flags` control bit for these.

NZCV obeys the *same* rule as every other scoreboard entry: a
**reader** stalls in ID until `valid[NZCV] = 1` (no flag writer
ahead of it remains in EX/MEM/WB), and a **writer never stalls on
another writer**. This is not a special case for flags — it is the
universal RAW-only behavior of the in-order scoreboard (Section 4).
It simply *matters most* here, because nearly every instruction is
a flag writer.

### 8.1 Why last-writer-wins is mandatory for NZCV

If flag writers had to serialize against each other (a WAW stall on
NZCV), then because almost every ALU op writes NZCV, *every*
independent ALU op would stall on its predecessor and the pipeline
would degenerate to one-instruction-at-a-time — un-pipelined
execution. Last-writer-wins is therefore not an optimization for
NZCV; it is required for the pipeline to pipeline at all.

The in-order completion property (Section 4) supplies it directly:
multiple flag writers can be in flight at once, they retire in
program order, and the architectural NZCV ends up holding the
youngest writer's result. A reader stalls only until the youngest
flag writer *ahead of it* has drained through WB — exactly the RAW
stall, no more.

**Decoder bits.** `writes_flags` and `reads_flags` (introduced
above) are sufficient. No "youngest-writer tag" is needed: the
re-derive scoreboard (Section 11) computes `valid[NZCV]` as
"no EX/MEM/WB stage currently holds a `writes_flags` instruction,"
which is true exactly when the youngest in-flight flag writer has
left WB. The youngest-wins outcome falls out of in-order WB; the
hardware never has to identify *which* writer is youngest.

**Independent ALU ops pipeline (the case Option-A-style WAW would
have killed):**

```
ADD R1, R2, R3   ; (1) writes R1 and NZCV
ADD R4, R5, R6   ; (2) writes R4 and NZCV — independent of (1)
```

| Cycle | IF1 | IF2 | ID | EX | MEM | WB | Notes |
|-------|-----|-----|----|------|------|------|-------|
| 1 | ADD1 | — | — | — | — | — | |
| 2 | ADD2 | ADD1 | — | — | — | — | |
| 3 | — | ADD2 | ADD1 | — | — | — | ADD1 issues; an NZCV writer is now in flight |
| 4 | — | — | ADD2 | ADD1 | — | — | ADD2 issues with **no stall** — it does not wait on ADD1's NZCV |
| 5 | — | — | — | ADD2 | ADD1 | — | both flowing, 1 insn/cycle |

Both NZCV writers are in flight in cycle 4; neither stalls. (Under
a WAW-on-NZCV rule, ADD2 would have stalled ~3 cycles here, and so
would every ALU op after it — the un-pipelining the user's rule
prevents.)

**The dominant real cost: `CMP → Bcc`.**

```
CMP R1, R2       ; (1) writes NZCV
BEQ target       ; (2) reads NZCV (RAW)
```

| Cycle | IF1 | IF2 | ID | EX | MEM | WB | Notes |
|-------|-----|-----|----|------|------|------|-------|
| 3 | — | BEQ | CMP | — | — | — | CMP issues; NZCV writer in flight |
| 4 | — | — | BEQ (stall) | CMP | — | — | BEQ reads NZCV, `valid=0` → stall |
| 5 | — | — | BEQ (stall) | — | CMP | — | still stalled |
| 6 | — | — | BEQ (stall) | — | — | CMP | CMP in WB; NZCV becomes architectural at end of cycle |
| 7 | — | — | BEQ (issue) | — | — | — | `valid[NZCV]=1` → BEQ issues |

`CMP → Bcc` costs **3 stall cycles** — identical to a GPR RAW, and
the single largest CPI source on branch-heavy code in gen2. A tight
loop body of `CMP; Bcc` therefore runs ~3 cycles of bubble plus the
branch flush per iteration. This is the headline target for
gen2.5's narrow EX→EX flag-forwarding path, which removes the
3-cycle stall entirely (the flag value is forwarded from CMP's EX
to Bcc's EX). gen2 takes the hit to keep the no-forwarding baseline
clean.

## 9. Interaction with drain-commit

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
- **WRSPR SR** (also drain-commit) writes the NZCV flags (plus the
  drain-serialized S/I bits). Its `valid[NZCV]` entry is cleared on
  issue and set on commit (in EX, since the instruction never
  reaches WB). Scoreboard-wise it looks like a normal flag writer
  that also drains the pipeline. The S/I writes it performs are not
  scoreboarded (Section 7).
- **EI / DI** (also drain-commit) write only the I bit, which is
  not scoreboarded. They clear nothing and set nothing in the
  scoreboard; their effect is ordered entirely by the drain.

Drain-commit's main contribution to the scoreboard is indirect:
it provides SR.S quiescence (Section 7) by ensuring no
mode-changing instruction is in flight while a younger instruction
decodes — and, dually, it is what lets S and I stay out of the
scoreboard entirely.

## 10. Interaction with divmul

The MUL/DIV instructions execute as a single µop with a
multi-cycle EX iteration (~34 cycles). The divmul unit owns the
ALU and writes two register results (`Rd` for the low half /
quotient, `Rdh` for the high half / remainder) via the regfile's
two write ports. Both results commit in the same cycle in gen2's
implementation.

Scoreboard interaction:

1. **At ID issue** the decoder clears `valid[Rd]` *and*
   `valid[Rdh]` together. Both must be valid pre-issue (Section 4
   stall predicate, with the pair-destination form).
2. **During divmul busy** the EX stage holds the instruction; the
   pipeline back-pressures upstream as for any EX stall.
   `valid[Rd]` and `valid[Rdh]` remain 0 throughout the divmul
   iteration.
3. **At divmul completion** both valid bits are set in the same
   cycle as the regfile writes commit.
4. **WB does not run for divmul instructions**, because the divmul
   commits in EX (similar to drain-commit, but without the drain
   semantics — divmul has no ordering effect on younger insns
   beyond the scoreboard). The MEM and WB stages see bubbles
   propagated from EX during the divmul iteration.

Divide-by-zero (`DIV`/`DIVU` with `Rs = 0`) raises `VEC_ARITH`
(vector slot 10); that fault propagates through the normal
precise-exception mechanism in
[exception-flow.md](./exception-flow.md), squashing
the divmul's writes by virtue of the WB-stage fault-commit squash
rule (rather than by special handling in the scoreboard).

## 11. Interaction with exception entry

When the precise-exception machinery commits a fault at WB
(see [exception-flow.md](./exception-flow.md)), it triggers:

1. A **fault-commit squash** of every younger in-flight insn
   (IF1, IF2, ID, EX, MEM).
2. A **one-cycle save-state pulse**: direct flop writes of
   `EPC ← faulting_PC`, `ESR ← SR`, with `SR.S ← 1` and
   `SR.I ← 0`. R14 banks to SSP.
3. A **vector-fetch FSM** in IF that drives the indirect load
   from `vec_num << 2` to PC, completing in a few cycles.

The scoreboard's role across this sequence:

- **Squashed instructions clear nothing.** The fault-commit
  squash signal forces ID/EX, EX/MEM, MEM/WB to bubble at the
  next clock edge. Any valid-bit clears those squashed
  instructions had performed at their issue cycle are **not
  rolled back** — that is, the scoreboard treats issued
  instructions as having owned their destination valid bit, even
  after squash.
- **This is safe** because a squashed instruction's destination
  has not been written and never will be; the valid bit will be
  restored when the *next* instruction to write that destination
  reaches WB. In the meantime no instruction reads the destination
  (because the trap handler's code runs after the squash and uses
  whatever scoreboard state it inherits).
- **Wait — is that actually safe?** Consider: an instruction
  issues, clears `valid[R5]`, then a younger instruction faults
  before the issuing instruction reaches WB. The squash flushes
  the issuing instruction along with the faulting one. The
  scoreboard now has `valid[R5] = 0` permanently — no one will
  ever set it.

  **Resolution:** the scoreboard must restore the valid bits of
  squashed instructions that have not yet reached WB. The cleanest
  mechanism is to **re-derive the scoreboard state** from the
  set of in-flight instructions at every cycle, rather than
  treating it as flip-flops latched on clear/set events. The
  scoreboard is then a function of "which physical entries are
  destinations of EX/MEM/WB instructions plus the
  about-to-issue ID instruction." A fault-commit squash
  empties those stages, so all valid bits return to 1
  automatically.

  Implementation note: gen2's scoreboard is small enough (22 bits)
  that the "re-derive every cycle" form is cheap. The
  `id_stage.sv` design should use that form rather than separate
  set/clear flip-flops, so that squash handling is implicit.

- **Save-state's direct ESR/EPC writes bypass the scoreboard.**
  No in-flight instruction at the moment of save-state can have
  ESR or EPC pending, because save-state fires only after the
  fault-commit squash has emptied the pipeline of younger insns
  and the faulting insn has already reached WB. The bypass is
  therefore not a hazard — it is just a state update outside the
  scoreboard's purview.

## 12. Worked stall examples

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
MUL R1, R2, R4       ; (1) writes Rd=R1 (low half), Rdh=R4 (high half); ~34 cycles
ADD R5, R4, R6       ; (2) RAW on R4 (the high half)
```

(2) stalls in ID until the divmul completes ~34 cycles later, when
`valid[R4]` is set. (1) clears both `valid[R1]` and `valid[R4]` at
issue, so any read of either stalls until the divmul EX-completes.

## 13. Verification considerations

The scoreboard's correctness invariants — physical addressing,
last-writer-wins under in-order completion, squash-aware
re-derivation, SR.S quiescence — are small in count but easy to get
wrong individually. Suggested testbench coverage for
`tb_scoreboard.cpp` (or however the unit test ends up packaged):

1. **Basic RAW.** Sequential ADD producer/consumer for every
   physical entry (1..22). Confirm 3-cycle stall.
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
5. **Squash recovery.** Issue a producer, follow with a younger
   instruction that takes a synthetic fault, confirm the
   producer's destination valid bit is correctly set after the
   exception entry's pipeline drain.
6. **SR.S quiescence.** ERET that changes SR.S with an R14-using
   instruction immediately after; confirm the decoder reads the
   post-ERET SR.S.
7. **NZCV pipelining.** A run of independent flag-writing ALU ops;
   confirm they issue back-to-back with no inter-writer stall.
   Then `CMP; Bcc`; confirm the 3-cycle reader stall, and that the
   Bcc observes the most recent flag writer's result.

A randomized stream test (random GPR/SPR mix with backpressure
events injected) is also recommended once the directed tests pass.

## Cross-references

- [design-decisions.md §4](./design-decisions.md#4-hazard-handling-strategy)
  — rationale for pure stall and the unified physical scoreboard.
- [design-decisions.md §9](./design-decisions.md#9-drain-commit-primitive)
  — drain-commit, which provides SR.S quiescence for the
  scoreboard.
- [design-decisions.md §10](./design-decisions.md#10-stall-propagation-policy-back-pressure)
  — back-pressure stall propagation; what `stall_id` does to
  upstream stages.
- [pipeline-stages.md §Stall sources](./pipeline-stages.md#stall-sources)
  and [§Cycle-accurate timing examples](./pipeline-stages.md#cycle-accurate-timing-examples).
- [exception-flow.md](./exception-flow.md) — fault commit and
  save-state's bypass of the scoreboard.
- [control-decode.md](./control-decode.md) — the decoder's
  control-vector layout, including the `phys_src_*`/`phys_dst`
  fields and the `writes_flags`/`reads_flags` bits Section 8
  introduces.
- [regfile.md](./regfile.md) — the 2R/2W regfile with R14
  banking. *(To be written.)*
- [instruction-set.md](../../system/instruction-set.md) — the
  ISA-level reference for SPR encodings (USP, ESR, EPC, SR,
  SCRn).
