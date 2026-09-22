# Penumbra LLVM documentation

How the Penumbra LLVM backend works, and the investigations behind it.
The backend's source map and build commands are in
`llvm/llvm/lib/Target/Penumbra/README.md` and its `CLAUDE.md`; the ISA
and ABI it targets are specified under [`doc/system/`](../system/).
Open compiler work is tracked in
[`doc/TODO/compiler.md`](../TODO/compiler.md).

## Backend design

- [strength-reduction.md](strength-reduction.md) — constant multiply,
  divide and remainder lowerings, and the arithmetic cost model.

## Upstream GlobalISel gaps

Generic LLVM issues found through Penumbra, written up for upstream.

- [gisel-freeze-known-bits.md](gisel-freeze-known-bits.md) — known-bits
  cannot see through `G_FREEZE`, which the combiner plants on every `||`.
- [gisel-pointer-minus-integer.md](gisel-pointer-minus-integer.md) —
  `p - n` keeps its negation as `p + (0 - n)`.
- [gisel-signed-pow2-remainder.md](gisel-signed-pow2-remainder.md) — no
  power-of-two signed remainder lowering.
- [gisel-wide-extload-legalization.md](gisel-wide-extload-legalization.md)
  — wide-memory extending loads do not legalize.
- [lowerBswap-bug.md](lowerBswap-bug.md) — `lowerBswap` miscompiles
  `G_BSWAP` wider than 32 bits.
- [mcp-zero-reg-forwarding.md](mcp-zero-reg-forwarding.md) —
  `MachineCopyPropagation` does not forward R0 through `mov` chains.

## Audits and investigations

- [gisel-upstream-miscompile-exposure.md](gisel-upstream-miscompile-exposure.md)
  — which known upstream GlobalISel miscompiles reach Penumbra.
- [flag-reuse-notes.md](flag-reuse-notes.md) — how flag reuse works on
  Penumbra, and what to add next.
- [divdf3-rounding-bug.md](divdf3-rounding-bug.md) — `__divdf3` rounding
  1 ULP short.
