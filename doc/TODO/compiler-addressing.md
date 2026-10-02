# Compiler: addressing, PIC and constants

## Compiler: a zero that enters a PHI is copied rather than read from R0

Constant zeros are read from R0 since `c6c5049b8f13`, but a zero
that enters a PHI cannot be: the PHI's register has several
definitions, so it cannot become R0.  Each zero is copied from R0
where it is used rather than shared from one register, and a
returned zero can land
outside R1 and take a second copy, as in `ptr-minus-int.c`'s
`walk_back`:

```
mov  r3, zero
mov  r1, r3
jmp  lr
```

The copy and its reader share a block only once block placement has
duplicated the return into its predecessors, after machine copy
propagation's last run, so the pair survives `AllowRegisterRenaming`
too (see it in "codegen pass/gate audit").

## Compiler: the PIC test suite has no dynamic PIE leg

`make test-compiler-pic` links its `-fPIC` set static at a fixed
address, so lld resolves the GOT at link time and the flat ISS image
needs no runtime relocator.  That exercises GOT-indirect codegen (it
catches a broken anchor pairing, `d2d682ee1250`) but cannot reach
TLS-GD, which needs `__tls_get_addr` and runtime relocation.  Nor does
it run a GOT that a relocator fills at run time: a GOT read is
selected as one no store changes, so it may move ahead of any store
in its function, and only a relocator in the same image would show a
read that moved ahead of the write to its slot.  A true dynamic PIE
leg would cover both.

## Compiler: support `[R0 + offset]` absolute addressing for low memory

`PenumbraTargetLowering::isLegalAddressingMode` currently rejects
`AddrMode` queries with `HasBaseReg = false` — that is, it tells
LSR / CodeGenPrepare that "just an offset" is not a legal address.
Strictly speaking it *is* legal: R0 is hardwired to zero, so
`LDW Rd, [R0 + offset]` reaches the low ±32 KB of memory in a
single instruction.  The kernel uses this for the trap vectors
(`0xFFFF_0000` is reachable as a negative offset from R0 thanks to
sign extension), and bare-metal MMIO probes commonly do
`*(volatile int *)0x100 = ...`.

The codegen pipeline does not currently materialize this form.  A
`G_LOAD` whose pointer is a small constant address goes through
the constant-materialization path (LLI / LLIS) and emits a real
`MOV` of the constant into a base register first.  Selection-side
work to recognise `(load (constant fits-in-simm16))` → `LDW Rd,
[R0 + offset]` would close this gap; once it does, drop the
`HasBaseReg` reject in `isLegalAddressingMode` so LSR knows the
mode is free.

Low-priority — the only places affected today are kernel
exception-vector reads and explicit MMIO accesses with very small
absolute addresses, both of which are written in inline asm or
hand-tuned C and don't go through optimization-sensitive paths.
But the discrepancy between "what the hardware can encode" and
"what the cost model claims" is worth closing for correctness of
optimization decisions in code we haven't yet seen.

## Compiler: constants are not rematerializable

A zero is selected as a generic `COPY $r0`, which the allocator
cannot rematerialize, and a full 32-bit constant is an `LLI; LUI`
chain it cannot copy either.  Such a constant live across a call or a
loop gets a callee-saved register or a stack slot, never a recompute.

- **Loop constants stayed in loops — RESOLVED.**  The Localizer sinks
  a constant to its use block, and nothing hoisted it back, because
  every instruction was marked side-effecting (see "Instruction
  properties" in the codegen pass/gate audit).  With that fixed,
  MachineLICM hoists them; `LLI`/`LLIS` are now rematerializable and
  as cheap as a move, so under register pressure the allocator
  recomputes a hoisted constant instead of spilling it.  Over twelve
  of Doom's renderer, collision and fixed-point files: 14592 → 14308
  instructions and 1349 → 1335 SP-relative loads and stores, where
  the side-effect fix alone gave 14436 and 1464.  `pmci_burst`'s idle
  fill (2 of its 14 cycles per byte on penumbra1, see "SD read
  throughput is bottlenecked by pmci_burst byte-PIO") is not yet
  rechecked.
- **Constants in stack slots.**  `sqlite3VdbeExec` keeps `rc = 0` in a
  slot reloaded 218 times and `1` in one reloaded 61 times.
  `libsqlite3` has at least 1,250 constant reloads (939 of them zero)
  and 609 spill stores.  The Localizer adds to it: a constant feeding
  more than two phi edges is not localized (`PhiThreshold = 2`,
  upstream "FIXME: Tune this more"), so the VDBE's zero becomes one
  function-wide value.
- **Constants and cheap derived values in callee-saved registers.**
  In Doom's `R_RenderBSPNode`, `side ^ 1` and `bsp + 48` each hold a
  callee-saved register; `solidsegs + 4` and `+ 256` do the same in
  `R_CheckBBox`, `R_AddLine` and `R_Subsector`, because an `ADDi` on
  the address pseudo is not trivially rematerializable.  The address
  case belongs to layer 3 of "G_GLOBAL_VALUE materialization defeats
  CSE and rematerialization".

Fix:
1. Mark `LLI`/`LLIS` rematerializable — DONE.
2. Select zero into a form the allocator can rematerialize — a
   `MOV rd, R0` that `isReMaterializableImpl` accepts, the way RISC-V
   accepts `ADDI rd, x0, imm`.
3. Full 32-bit constants — DONE.  `LUI` reads its destination, so
   `LLI; LUI` is a two-instruction chain that remat cannot copy.  A
   wide constant is now one `PseudoLI`, expanded after register
   allocation; static addresses, jump-table bases, block addresses
   and TLS offsets share it, so no `LLI; LUI` chain exists before
   allocation.

How aggressively the allocator uses remat is per-core policy — see
"Remat aggressiveness is per-microarchitecture, not a pseudo flag".

## Compiler: early MachineLICM hoists global addresses into callee-saved registers

Early MachineLICM hoists global address builds out of a loop containing
calls, and the allocator then keeps them in callee-saved registers
instead of recomputing them after each call.  Doom's `R_RenderBSPNode`
hoists `@viewx`/`@viewy` out of its far-child loop (3 calls).  With
`-disable-machine-licm` the frame shrinks from 28 to 20 bytes and the
saves from 7 to 5 — 24 cycles per call on penumbra1, 12 on
penumbra2_5.  riscv32 does the same, but its stores are not
write-through.  The allocator takes the trade because a callee-saved
register's first use costs it nothing (see item 1 of the
shrink-wrapping entry's open list).  Localizing constant-offset addresses does not
change this function.

## Compiler: pointer-minus-constant and integer MMIO addresses miss their folds

- **`p - K`.**  `G_PTR_ADD p, -K` selects as `LLIS t,-K; ADD p,t`, often
  with a `MOV`.  `ADDi`/`SUBi` take a zero-extended immediate, and
  `penumbra_neg_imm_to_opposite` rewrites only `G_ADD`/`G_SUB`.  322
  kernel sites (2026-09-13).  The hot ones are the backward loops of
  the C `memmove` from `lib/kern`.  A reduced copy of its byte loop is
  10 instructions per iteration where 6 do:

  ```
  mov r4,r1; mov r11,r2; ldb r2,[r11+0]; llis r1,-1; add r1,r4
  stb r2,[r4+-1]; llis r2,-1; add r2,r11; sub r3,1; bne
  ```

  The fix is the pointer analogue of the existing rule: select a
  `G_PTR_ADD` by a negative constant whose negation fits uimm16 as
  `SUBi`, or rewrite it through the integer subtract that
  `penumbra_ptr_add_neg_to_sub` already builds.
- **MMIO addresses built as integers.**  `bus_space_read_*` and
  `bus_space_write_*` (`bus_funcs.h`) add the handle and offset as
  integers and cast the sum, so the load sees
  `G_INTTOPTR(G_ADD h, C)`.  `selectAddrRegImm` looks only through
  `G_PTR_ADD` and frame indices, giving `ADDi; LDW [r+0]` — plus a
  `MOV` when `h` stays live — where `LDW [h+C]` does it.  187 kernel
  sites, 76 of them with the `MOV`, mostly in `pmci`, `pdisplay`,
  `pusbhc` and `paudio`.  Either look through
  `G_INTTOPTR(G_ADD x, C)` at selection, or do the macros' arithmetic
  on a `char *` — `bus_funcs.h` is MD code.  RISC-V's GlobalISel has
  the same selection gap.
- **A local array indexed at run time rebuilds its address.**  A
  frame object that does not escape stays in memory once a run-time
  index reaches it, and every access forms `SP + offset` afresh and
  loads through `[r + 0]`.  Reduced, with two accumulators so that
  one sits at a non-zero offset:

  ```c
  int g(const int *side, const int *val, int n) {
      int a[2] = { 0, 0 }, b[2] = { 0, 0 };
      for (int k = 0; k < n; k++) {
          a[side[k] & 1] += val[k];
          b[(side[k] >> 1) & 1] += val[k];
      }
      return a[0] - a[1] + b[0] - b[1];
  }
  ```

  In the loop, `a` at `SP + 8` is `mov r6, sp; add r6, 8; add r6,
  r5` and then `[r6 + 0]` for the load and the store, where
  `add r5, sp` and `[r5 + 8]` need two fewer; `b` at offset 0 is
  `mov r5, sp; add r5, r11`, where `add r11, sp` needs one fewer — 3
  of the loop's 23 instructions.  It fits `selectAddrRegImm` folding a
  frame index or a constant `G_PTR_ADD` but not a frame index under a
  `G_PTR_ADD` by a register: the frame offset belongs in the
  displacement, with the index added to `SP`.  In the frame-by-frame
  audio mixer, whose accumulator is indexed by the voice's side, it
  is 2 instructions per voice-sample.

## Compiler: G_GLOBAL_VALUE materialization defeats CSE and rematerialization — layer 1 DONE

Layer 1 below is done: a static address became one rematerializable
pseudo in `f863f06db486`, and every wide value, constants included, is
one `PseudoLI` since `5de5ff71902d`, expanded into `LLI`+`LUI` after
register allocation.  An identical address now rematerializes and
CSEs, and whether recomputing it counts as cheap is the tuned core's
answer.  What stays open is sharing a base among *different* addresses
(base sharing and the layer-3 offset merge, below).  The rest of this
opening describes the code before layer 1.

`PenumbraInstructionSelector` lowered `G_GLOBAL_VALUE` (and the
`@sym + offset` form) unconditionally to a raw `LLI rd, %lo ; LUI rd,
%hi` pair at selection time, in `emitLoadSymbolAddr` (the static/absolute
path; PIC takes the GOT path below). That eager two-instruction
expansion was a *double* defeat:

- **CSE can't see a shared base.** Each pair materializes the full
  `sym+offset`, so two references to the same symbol at different
  offsets — or two globals the linker happens to place near each other —
  look like unrelated constants. MachineCSE has nothing to share.
- **Rematerialization couldn't fire** (resolved by layer 1). An
  address is the textbook remat candidate (cheap to recompute, no
  inputs), but remat works on a single def, not a two-instruction
  dependent chain. So when an address had to survive a call, the
  allocator's only options were spill/reload or burning a callee-saved
  register — never "recompute it after the call," which is usually
  cheapest.

A representative kernel sequence (hot path) materializes four addresses
that are all `copyright + constant` through four independent pairs — two
of them 11 bytes apart:

```
lli r1, 0xF1DD ; lui r1, 0x8027   ; copyright+0x13dc3
lli r2, 0x6D84 ; lui r2, 0x8028   ; copyright+0x1b96a
lli r3, 0x0A7E ; lui r3, 0x8027   ; copyright+0x5664
lli r4, 0x0A89 ; lui r4, 0x8027   ; copyright+0x566f  (11 B past r3)
```

Dynamic share on Dhrystone (new ISS `+opstats` instruction-class
histogram): explicit `LLI/LUI` is ~5% of executed instructions, but the
absence of base-sharing also inflates the immediate-ALU bucket (~30% —
offset arithmetic that would otherwise fold into a load) and, in
disassembly, the pattern is pervasive on hot paths.

### Instruction-count vs footprint: which layer moves which

The "Dhrystone hot-path code shape" entry in workloads.md established that
I-footprint, not dynamic instruction count, gates HW Dhrystone —
count-*neutral* folds (the select-CMPi fold) measured flat because they
don't change which cache lines are touched. Be precise about which layer
of this fix is count-neutral and which actually removes instructions:

- **Layer 1 is count-neutral by construction.** `PseudoLI` expands
  back to the identical `LLI+LUI`, so the static instruction stream is
  unchanged unless CSE or remat fires. MachineCSE already deduped eager
  `LLI+LUI` pairs in straight-line code, so layer 1 adds little there;
  its real win is *rematerialization* — trading a spill/reload (or a
  burned callee-saved register) for a recompute when an address is live
  across a call under register pressure. That is a register-allocation
  *quality* win, not a footprint win, and it only engages where those
  cross-call-live address sites exist. It is the concrete mechanism
  behind that entry's finding #1 ("register-pressure-aware
  address-arithmetic CSE").
- **Layer 2 is infeasible on Penumbra; layer 3 still removes
  instructions (corrected 2026-06-22).** The textbook RISC-V fold —
  materialize a `%hi` base and let the load carry `%lo` in its offset
  field (`LUI + LD %lo`, 2 insns) — does **not** apply: Penumbra's `LUI`
  *ORs* the high half in (`Rd = Rd | (imm16<<16)`,
  `instruction-set.md`; ISS `case 2`), it does **not** clear the low 16
  bits, so a `%hi`-only base would first need `MOV rd,R0` — `MOV+LUI+LD`
  is three instructions, identical to `LLI+LUI+LD`. A single isolated
  global load is therefore structurally **3 instructions** with no
  compiler fold to 2 (closing that would need a clear-low load-upper
  instruction — an ISA change, weighed against the discrete-logic
  constraint; the OR-`LUI` exists to make the `LLI`-then-`LUI` idiom
  work without a separate combine). What *does* remove instructions is
  **base-sharing**: with the layer-1 pseudo, MachineCSE materializes a
  reused base once and each access folds its *residual constant offset*
  into the load's simm16 field when it fits ±32 KB (the layer-3
  mechanism), collapsing redundant `LLI+LUI` pairs and offset `ADD`s.
  That helps clustered/reused globals, not the isolated single-access
  case — which is most of Dhrystone's near-use globals, consistent with
  the measured-flat result below.

**Measured 2026-06-18 (HW): layer 1 alone is flat on Dhrystone** — as
expected from the above. Dhrystone accesses its globals near their use,
not held across calls, so remat has nothing to engage and CSE already
captured the straight-line case. Layer 1's value is the *enabler*
(single SSA def that layers 2-3 and the allocator build on) plus
RA-quality on cross-call, register-pressure-bound code — kernel-shaped,
not Dhrystone-shaped. To see layer 1 move a number, measure a workload
with cross-call address liveness (kernel syscall / fork+exec benches).
Footprint only shrinks where a base is *reused* (base-sharing collapses
the redundant `LLI+LUI`) or carries an offset that fits the load's
simm16; Dhrystone's isolated near-use globals largely lack that shape,
so even the full fix may stay flat there. The per-load `%lo`-into-load
shrink once hoped for here is infeasible (OR-`LUI`, above).

**Measured on Doom 2026-09-22 (ISS instruction profile, "Doom: where a
frame goes"): 11.1% of every instruction executed is an `lli`+`lui`
address pair, and it splits in two.** 6.2 points are the *same*
address materialized again within one call — mostly inside loops
(`R_RenderSegLoop` 4.0, `R_DrawMaskedColumn` 1.5) — which is a
cost-model question, not base sharing: recomputing is priced as free
pre-RA although it expands to two instructions, so the allocator
prefers it over keeping the base live. Layout-neutral, but it can
still lose, since a base kept across a call in those loops has to be
callee-saved. The other 3.5 points are base sharing proper, and are
**not** worth pursuing: they need distinct globals within ±32 KB of
each other, which makes them a bet on data placement, and with a split
L1 — where instruction fetch never seeds L1D — spreading or clustering
globals to shorten their reach can cost more in D-cache misses than it
saves in instructions. The same reasoning that retired GlobalMerge
("Global base sharing", measured net-negative on hardware) applies
here. Doom's real fix is its own source: drawer state passed as
arguments rather than written to `dc_*` globals around every call.

### Fix — three layers (layer 1 enables; base-sharing + layer-3 offset-merge carry the footprint win; the RISC-V `%lo`-into-load fold is infeasible)

1. **Rematerializable materialization pseudo.** Stop emitting `LLI+LUI`
   at selection; lower the static/absolute `G_GLOBAL_VALUE` to a single
   `PseudoLI rd, @sym` (`isReMaterializable`, `isPseudo`,
   `Size = 8`), expanded to `LLI+LUI` in a *post-RA*
   `PenumbraExpandPseudoInsts`. Now it is one SSA def: MachineCSE shares
   identical bases pre-RA, and the allocator owns the cross-call
   keep-vs-recompute decision via remat — exactly how AArch64 never
   "carries" a base over a call (its `ADRP` is a rematerializable single
   instruction it just recomputes after the call). This one change fixes
   the kernel example and the cross-call carrying; the rest is gravy.
   Expand it in a *post-RA* pass (a new `PenumbraExpandPseudoInsts`, or
   the `PenumbraInstrInfo::expandPostRAPseudo` hook) — **not** at
   AsmPrinter time: the pseudo must survive register allocation as one
   `MachineInstr` for the allocator's `reMaterialize` to copy it. Note
   the existing PIC carriers (`PICLLI`/`PICLUI`/`PICADDPC`) expand inside
   `PenumbraAsmPrinter` because their operands are label-difference,
   MC-level expressions — that is the right idiom for *PIC*, the wrong
   one for this expansion site. RISC-V's `PseudoLLA` + post-RA
   `RISCVExpandPseudoInsts` is the matching template.
2. **~~Fold `%lo` into the load offset~~ — infeasible on Penumbra
   (corrected 2026-06-22).** The RISC-V form (`LUI + LD %lo`) assumes a
   clear-low `LUI`; Penumbra's `LUI` ORs the high half without clearing
   the low 16 (`Rd = Rd | (imm16<<16)`, `instruction-set.md`; ISS
   `case 2`), so a `%hi`-only base cannot be built in one instruction and
   the fold saves nothing — there is no symbol-`%lo`-into-load
   instruction-count win (see "which layer moves which" above). The
   surviving instruction-removal is the **constant-offset** fold under
   layer 3, *not* the symbol-`%lo` fold: after base-sharing materializes
   a base once, fold a residual constant offset that fits the load's
   simm16 field directly into the memory op (`LDW [base + off]`), else
   keep the `ADD`. The `isLegalAddressingMode` base+simm16 advertisement
   is still worth fixing for that — the same gap as the "[R0 + offset]
   absolute addressing" entry, which remains independently valid (R0 is
   hardwired zero, so a small absolute address rides the load's offset
   with no `%hi` at all). `selectAddrRegImm` already folds a *resolved*
   base+simm16 into the Format-M offset field for frame indices and
   constant GEPs; (a) confirm the Format-M memory-offset operand can
   carry a *symbolic* relocation for the offset-merge case, and (b) drop
   the `HasBaseReg`/base+simm16 reject in `isLegalAddressingMode`.
3. **Merge residual constant offsets** into the pseudo's symbol operand
   (`%hi(sym+o)`) and into memory ops, modeled on RISC-V's
   `RISCVMergeBaseOffset` — for offsets that can't fold into a load
   (out of range, or an address materialized into a register, e.g. a
   pointer argument as in the kernel example).

Note the cross-call case is **not** a job for a greedy merge pass — it
is a register-allocation cost decision (remat vs CSR vs spill), and the
pseudo is precisely what hands it to the allocator. Don't hand-roll
cross-call liveness in a MIR pass.

### Remat aggressiveness is per-microarchitecture, not a pseudo flag

Split *capability* from *policy*.  `PseudoLI` states the capability
(`isReMaterializable`, `Size = 8`) unconditionally: every core can
recompute a wide value.  Whether a core should — rebuild the constant
near each use rather than keep it live, which is what
`isAsCheapAsAMove` decides for MachineCSE and the register coalescer —
is `PenumbraSubtarget::preferRecomputingWideConstants`, answered from
the tuned core's scheduling model rather than a feature bit or a CPU
test: recomputing wins where the `LLI` + `LUI` pair costs no more than
its two instructions.  Penumbra/1 (serial) and Penumbra/2.5
(forwarding) qualify; Penumbra/2 does not, since `LUI` waits three
cycles for the `LLI` result (`test/CodeGen/Penumbra/wide-constant-tuning.ll`).

For `generic` (Penumbra/2.5) over twelve of Doom's renderer, collision
and fixed-point files it costs 0.75% of text and 117 instructions, and
saves 41 of 1335 SP-relative loads and stores and 71 copies.

The models state cycles, not instruction-cache pressure, so a gen1
core still recomputes whenever the cycles say so, even where its 1 KB
direct-mapped I$ would favour the smaller sequence.  Cache-pressure
controls for constructs that trade instructions for cycles are
deferred until tuning for a gen1-style core matters — a discrete gen1
machine.

### TLS and PIC are orthogonal

Only the static/absolute path changes. PIC (`G_GLOBAL_VALUE` → GOT load)
is a *load*, not a materialization — CSE-able but **not** rematerializable
(it touches memory), so it gets its own pseudo with different properties.
TLS (GD/LD/IE/LE) is model-specific (thread pointer, possibly a
`__tls_get_addr` call) and stays its own path. The pseudo model actually
*simplifies* the current selector tangle: emit a distinct pseudo per
relocation/TLS model and move the byte sequences into the post-RA
expander, the way RISC-V's `PseudoLA` / `PseudoLA_TLS_*` family does.

### Templates and tooling

- RISC-V: `PseudoLLA`/`PseudoLA`/`PseudoLA_TLS_*` in `RISCVInstrInfo.td`,
  the post-RA `RISCVExpandPseudoInsts.cpp`, and `RISCVMergeBaseOffset.cpp`
  are near-direct templates (Penumbra's static expansion is absolute
  `LLI+LUI` where RISC-V's is PC-relative `auipc+addi`).
- ISS diagnosis tooling added this round: `+opstats` (instruction-class
  histogram), `+branchstats` (branch-direction histogram), and
  `sw/tools/branch_ceiling.py`. Re-run Dhrystone with `+opstats` after
  layer 1 to confirm the immediate-ALU / `LLI`+`LUI` buckets drop.

### 2026-06-19 empirical re-measurement — Layer 2 doesn't translate; base-sharing is the static lever

A disassembly study of the just-built binaries (kernel `MINIMAL`, an
`ET_EXEC` executable, libc.so) reshaped the plan above. Method:
`llvm-objdump -d` annotates every `lui` with its resolved 32-bit value
(the disassembler tracks GPR state), so address-forming `lli+lui` pairs
can be bucketed by target. Filtering the kernel to values that land
inside the kernel image (`0x80010000`–`0x802c0000`) drops 32-bit
*constant* materialisations and leaves **41,590 true absolute address
materialisations**. Findings:

- **Same-symbol multi-offset is already optimal.** Struct-field and
  array-element accesses (`s.a/s.b/s.d`, `g[2]/g[5]`) already materialise
  the base once and fold each offset into the load
  (`selectAddrRegImm`): `2 + N`, not `3N`. Layer 1's CSE-able pseudo plus
  the existing constant-offset fold delivers the base-amortisation win
  Layer 2 was reaching for — and the base is the *exact* `&s`, so the
  hardware's signed-offset add is correct with no carry handling.
- **Layer 2's per-access `LLI+LUI+LD → LUI+LD` win cannot exist here.**
  That reduction assumes a one-instruction high-only base. RISC-V's `lui`
  *replaces* the low bits; Penumbra's `LUI` **ORs** (`Rd = Rd |
  (imm16<<16)`, encoding-table opcode `0010`, tied `$Rd=$Rd_in`), so a
  clean `hi<<16` base needs `LLI rd,#0; LUI rd,%hi` — two instructions —
  and folding `%lo` into the load buys nothing. Layer 2 ports the RISC-V
  *mechanism* but not its *benefit*. **Treat Layer 2 (the `%lo`-into-
  offset fold) as not applicable; do not implement it as written.**
- **A "load-upper-and-clear" (`LUIC`) opcode is a minor lever, not the
  fix.** It would enable `LUIC %hi_carry; LD %lo` (3→2), but only **11.2%**
  of kernel address materialisations feed a single direct `[Rd+0]`
  load/store where the fold applies (~4,654 sites, ~18 KB `.text`); the
  other ~62% use the address as a pointer/base, where `LUIC %hi; ADDi
  %lo` = 2 = the existing `LLI;LUI`, no gain. It also does **not** merge
  unrelated-but-close symbols (different symbols → different `%hi_carry`
  relocations that don't CSE at compile time). Encoding space exists
  (Format-L opcodes `1101/1110/1111` reserved); RTL is trivial and
  actually drops the `Rd` read/tied operand vs OR-`LUI`. But it must be
  *additive* — the `LLI;LUI` 32-bit-constant idiom needs OR semantics, so
  `LUIC` cannot replace `LUI` — and it needs a carry-adjusted `%hi`
  relocation. Verdict: defer; sequence after base-sharing (which already
  converts many `[Rd+0]` loads to `[base+off]`, overlapping its win) and
  re-measure.
- **Base-sharing of unrelated-but-close absolute addresses is the real
  static lever** — the `copyright+const` shape, e.g. `phys_bias`
  (`0x80297000`), `bootinfo_store` (`0x80297020`), `penumbra_bootinfo`
  (`0x8029802c`) each rebuilt from scratch in the same `lui 32809` page.
  Of the 41,590 materialisations, **40.9% are shareable under a
  conservative model** (cluster targets within 60 KB inside a straight-
  line run split on calls), 78.1% function-wide. This is an *upper
  bound*: holding a base live across a call-free region still competes
  for one of ~12 allocatable registers in a 2-operand ISA, so the
  realisable share is lower; the clean first cut is call-free,
  same-section clusters.
- **The transform is layout-neutral, no ISA change needed.** For two
  *local same-section* symbols `A`,`B`, the distance `B-A` is an
  assembly-time label difference (the assembler folds it — no
  relocation, no carry, and the data never moves). So a cluster of K
  references becomes `LLI %lo(A); LUI %hi(A)` (exact `&A`) + K loads at
  `[&A + (sym_i - A)]` = `2 + K` vs `3K`. This is GlobalMerge's win
  *without* GlobalMerge's data-relocation cache penalty — GlobalMerge
  physically coalesces globals into one struct, which measured −2.4%
  DMIPS on gen1 Dhrystone (fewer instructions but more misses in the
  1 KB direct-mapped L1 from the relocated data). Label-difference
  base-sharing moves no data, so that penalty never arises, on either
  generation. (Whether the original GlobalMerge penalty still holds on
  gen2's 4 KB 2-way L1 is untested and moot for this approach.)
  Cross-section clusters need a link-time relocation but stay
  layout-neutral.
- **Implementation shape:** a target MIR pass over the `PseudoLI`
  defs (Layer 1 already gives CSE-able single-def bases), modelled on
  `RISCVMergeBaseOffset` but extended from same-symbol to same-section
  cross-symbol. Open scope decision: same-section-local only
  (assembly-time diffs, zero new relocations — recommended first cut) vs
  also cross-section (needs a base-relative `R_PENUMBRA_*` and the
  linker). The register-pressure call belongs in the allocator, not a
  greedy merge — same caution as the cross-call remat note above.

`fold_classify`/clustering scripts used for these numbers are throwaway
disassembly miners (not committed); the durable artifacts are the
numbers and conclusions here.

**In-loop address pairs in the demos are remat, not a missed hoist.**
Plasma's depth-3 pixel loop shows an `LLI`/`LUI` pair per iteration
re-materializing a table base.  Post-regalloc MIR holds a single
`PseudoLI`; the pair appears only after `expandPostRAPseudo`, so
Layer 1 is doing its job and LICM is not the lever.

What drives the remat is the destructive two-operand `ADD` forming
`base + index`: it consumes the base, so each address computation needs
either a private copy of a live base or a fresh materialization.  The
same loop shows both choices — `COPY` + `ADD` for a use with the base
still live, a bare `ADD` killing it at the last use, then
`PseudoLI` at the next block that needs it.  Rebuilding costs two
instructions where copying a live base costs one, so remat runs one
instruction per materialization *more* than pinning the base; the
allocator takes that trade because the loop is at near-total pressure
(~10 of 11 allocatable GPRs live across it) and a spill/reload would
cost more.

Nothing cheaper is available: an arbitrary global's address is
inherently a two-instruction materialization, so the +1 is structural
and the only alternative is spending a register to keep the base live.
That makes this a register-pressure trade for the allocator, not a
missing optimization.  Recorded so the shape isn't re-diagnosed as a
missed hoist.

### PC-anchor sharing for direct PIC accesses

Non-preemptible PIC globals are addressed PC-relative-direct since
`8c8c28df7f51`, which left the GOT with 299 entries in `libc.so`.  Each
of the ~5,000 now-direct accesses still carries its *own* per-site
`lli; lui; add pc` triple anchored to its own PC, so repeats do not CSE.
**PC-anchor sharing** (regional base-CSE: materialise one PC-derived
anchor per region, then `anchor + (sym - .Lanchor)` per access) collapses
those triples. It is **layout-neutral — it does not relocate data**, so
it is the *opposite* transform from GlobalMerge: the net-negative
Dhrystone result belongs to GlobalMerge (which packed hot scalars beside
a cold 10 KB array and thrashed the **gen1 1 KB direct-mapped** D-cache),
*not* to base-CSE, whose only cost is register pressure. That gen1 cache
number does not transfer to gen2 (4 KB 2-way) / gen3, which is where
userland runs. This is the same pass as absolute base-sharing for the
kernel + executables' own globals, differing only in base kind
(PC-derived vs absolute). Realized win is global-density
dependent (real code is far less global-dense than Dhrystone), so measure
on gen2/gen3 hardware before productionising.

**Priority across the remaining addressing investigation, by leverage:**
(1) regional base-CSE — one parameterised pass serving both PC-anchor
sharing (the ~5,000 direct PIC accesses above) and absolute base-sharing
for the kernel + executables' own globals (~41% of kernel
materialisations); layout-neutral, register-pressure-gated, measured on
gen2/gen3; (2) `LUIC` and other secondary folds that compose with it.

## Ruled out

- **Removing the last GOT slots.**  After PC-relative-direct
  addressing, `libc.so` keeps 56 non-preemptible GOT slots — symbols
  not provably `dso_local` at compile time that the linker bound
  locally — below the bar for a dedicated pass.  A GOT/GP base
  register for the ~237 preemptible slots would cost a pinned
  register on a 2-operand ISA for too few accesses.
