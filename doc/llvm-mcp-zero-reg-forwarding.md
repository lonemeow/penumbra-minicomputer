# LLVM `MachineCopyPropagation` does not forward Penumbra R0 through `mov rN, r0` chains

## TL;DR

Penumbra's R0 is a hardwired-zero register: `isConstant = true` in
`PenumbraRegisterInfo.td`, reserved in `PenumbraRegisterInfo.cpp`.
The instruction selector lowers `G_CONSTANT 0` to `COPY $r0`.  When
the register allocator can't fold the COPY (e.g. the vreg is later
overwritten so it can't be coalesced with $r0 directly), the COPY
survives RA and is lowered by `postrapseudos` to `mov rN, r0`.

The intent was that `MachineCopyPropagation` (MCP) would forward
`$r0` through these copies, leaving the `mov` dead for DCE.  In
practice **MCP does no forwarding** and the `mov` survives to the
final asm.  Dhrystone shows 43 such surviving `mov rN, r0` sites.

This document records what we learned while trying to fix this and
where to resume.  Nothing in the backend was changed — the override
attempt described below was reverted.

## What we tried (and why it didn't work)

The natural-looking fix was to override
`PenumbraInstrInfo::isCopyInstrImpl` to teach MCP that
`Penumbra::MOV` is a register copy, modeled on RISC-V's override
which recognizes `ADDI Rd, Rs, 0`, `ADD/OR/XOR` with `X0`, etc.

This override **compiles cleanly and is architecturally correct**,
but produces a byte-for-byte identical Dhrystone binary.  Two
reasons:

### Reason 1: MCP never sees `MOV` instructions

The codegen pass order on Penumbra (and most upstream targets) is:

```
greedy regalloc → virtregrewriter → machine-cp (1st)
  → machinelicm → … → branch-folder → tailduplication
  → machine-cp (2nd) → postrapseudos → post-RA-sched → emit
```

Both MCP invocations run **before** `postrapseudos`.  `postrapseudos`
is the pass that calls `TII::copyPhysReg` to lower `COPY` pseudos
into real `MOV` instructions.  So MCP only ever observes abstract
`COPY` pseudos, never lowered `MOV`s — and the `isCopyInstrImpl`
override (whose whole point is to teach MCP about target-specific
copy *instructions*) has nothing to do.

A target that wanted the override to fire would have to either
schedule a third MCP pass *after* `postrapseudos` (via
`TargetPassConfig::addPreEmitPass`) or rely on a different
copy-aware consumer of `isCopyInstr` running after lowering.  No
upstream LLVM pass currently does the latter.

### Reason 2: even on `COPY` pseudos, MCP refuses to forward — the `renamable` flag is missing

Even at the COPY level (where MCP runs), forwarding doesn't happen
for our zero-register chain.  Trace of `multi_use_zero(int *p)`
returning 0 after three stores:

```
# After virtregrewriter:
$r2 = COPY $r0
STW $r0, $r1, 0
STW $r0, $r1, 4
STW $r0, killed $r1, 8
$r1 = COPY killed $r2          ; could be: $r1 = COPY $r0
RET implicit $r1
```

MCP's forward-propagation in
`llvm/lib/CodeGen/MachineCopyPropagation.cpp:forwardUses` (around
line 800 of the LLVM 22 source we're shipping) bails on this:

```cpp
// Check that the register is marked 'renamable' so we know it is safe to
// rename it without violating any constraints that aren't expressed in the
// IR (e.g. ABI or opcode requirements).
if (!MOUse.isRenamable())
  continue;
```

The use being considered for forwarding is `$r2` in the final
`$r1 = COPY killed $r2`.  `$r2` is **not** marked `renamable`
after `virtregrewriter`, so MCP gives up.  The reserved-physreg
check at line ~830
(`isReserved(CopySrcReg) && !isConstantPhysReg(CopySrcReg)`) does
correctly let `$r0` through — that gate is not the blocker.

### Why isn't `$r2` renamable?

This is the unanswered question.  `$r2` was chosen by greedy
regalloc for a vreg whose only def was `COPY $r0` — the choice
should be renamable.  Hypothesis (unverified): `VirtRegRewriter`
or an earlier pass declines to set the renamable bit on a COPY
destination whose source is a reserved physreg, out of conservatism
about reserved-register semantics.

AArch64 and RISC-V's `copyPhysReg` overrides take `RenamableDest`/
`RenamableSrc` parameters but neither passes them to `BuildMI`
either, so they would have the same blind spot **if** their
upstream pipeline produced renamable-less COPYs of constant
physregs in the same shape.  Whether they do is the next thing to
check.

## What was actually byte-identical?

```text
build/benchmark/DHRYSTON.ELF  pre-override:  120046 bytes, 43 'mov rN, r0' sites
build/benchmark/DHRYSTON.ELF  post-override: 120046 bytes, 43 'mov rN, r0' sites
diff: zero lines
```

## What the 43 surviving `mov rN, r0` sites actually are

Bucketed by what follows them — none of them are easy MCP wins:

```text
followed by `stw rN, [...]` with matching reg:   1 site
followed by `bl <callee>`:                       many sites — preserving 0 in a
                                                 callee-saved reg across a call
followed by other ALU ops:                       many — initializing an
                                                 accumulator that's then
                                                 written by an `add` etc.
```

The `bl`-following sites are structurally mandatory: R0 isn't
callee-saved (no register is "callee-saved zero" — R5+ are
callee-saved general-purpose), so a function that holds 0 across a
call must materialize it in R5–R10/R13.  Forwarding $r0 in won't
help; those movs would have to be replaced by re-materializing zero
*after* the call, which is a different optimization (rematerialization,
not copy propagation).

So even a perfect MCP fix would catch at most a handful of sites
in Dhrystone.  The deeper opportunity is in the
**rematerialization** path — teaching the regalloc that "zero from
$r0" is trivially rematerializable, so it doesn't need to preserve
a copy across calls at all.

## Where to resume

In rough order of likely payoff:

1. **Verify the AArch64/RISC-V comparison.**  Take the same
   `multi_use_zero` pattern and compile for `riscv32` and
   `aarch64`.  If their codegen also leaves the redundant mov,
   this is a generic LLVM-22 behavior and our investment should
   go into rematerialization (#3 below) instead of chasing the
   renamable flag.  If their codegen is clean, the difference
   points us at what they do that we don't.

2. **Trace the renamable-flag plumbing.**  Build LLVM with
   `-DLLVM_ENABLE_ASSERTIONS=ON` and a debug build so
   `-debug-only=regalloc,virtregrewriter,machine-cp` works, then
   step the `multi_use_zero` case and find the exact code path
   that drops the renamable flag for `$r2 = COPY $r0`.  Likely
   in `lib/CodeGen/VirtRegMap.cpp` (`addMBBLiveIns`,
   `rewrite()`) or `lib/CodeGen/RegAllocBase.cpp`.

3. **Rematerialization for constant zero.**  Tell LLVM that the
   `LLI Rd, 0` (or `MOV Rd, R0`) materialization of zero is
   rematerializable so the allocator inserts a fresh `mov rN, r0`
   right before each use instead of preserving one across calls.
   The TableGen flag is `let isReMaterializable = 1, isAsCheapAsAMove = 1`
   on either MOV or a dedicated "load zero" pseudo.  This
   addresses the structural sites that MCP can't reach.

4. **Custom post-`postrapseudos` MCP pass.**  Only worth doing
   *after* fixing the renamable flag (#2), since otherwise it
   inherits the same blocker.  If we ever do go this route, add
   it via `PenumbraPassConfig::addPreEmitPass`.

## Don't be misled by

- The CLAUDE.md / TableGen comments that imply MCP forwards through
  constant physregs "unconditionally."  The `renamable` gate
  invalidates that claim in practice.  This doc is the corrected
  story.
- Test cases like `void f(int *p) { *p = 0; }` — these have a
  short-lived vreg that the register coalescer *can* merge with
  $r0 directly, bypassing MCP entirely.  They look like proof
  MCP is working when actually MCP isn't even involved.  Use the
  longer `multi_use_zero` shape to exercise the failing path.
