# Skrolli Party 2026 entry — ground rules

A demoscene production for the Wild compo at Skrolli Party, running on
bare metal on the Penumbra hardware and submitted as a video capture.
This file governs everything under `sw/demoscene/skrolli2026/`.

## Why these rules exist

The compo rules permit generative AI as a tool but require that the
work not be wholly or mostly AI-produced, and place responsibility for
any AI-derived content's copyright on the author:

> *"Generatiiviseen AI:hin pohjautuvien työkalujen käyttö on sallittua,
> mutta teos ei saa olla kokonaan tai enimmäkseen AI:n tuottama.
> Tekijä on vastuussa siitä ettei AI:n tuottama sisältö loukkaa
> muiden tekijänoikeuksia."*

The rule is about the *teos* — the work — not the toolchain. A demo's
work is its effects, its look, its music and its timing. The line here
is therefore drawn at **what reaches the screen and the speakers**.

## Claude may, without asking

- Anything under `runtime/`, `tools/` and `capture/` — device access,
  the frame loop, the precalc scheduler, asset converters, encode
  scripts.
- Optimize a part Ilpo wrote, where the output is pixel-identical or
  the visual change was approved first.
- Explain a technique or the mathematics behind it, and cost it
  against the measured budget in [NOTES.md](NOTES.md).
- Transcribe Ilpo's own sketch from another language or tool.

## Claude may not, even if asked casually

- Invent effects, or choose which parts exist and in what order.
- Choose palettes, colors, motion, timing, transitions or
  choreography.
- Author the demo's title, on-screen text, logos, fonts or ASCII art.
- Generate asset data of any kind.

If a request drifts into these, say so and ask for the specification
rather than supplying one.

## Directed exceptions

Two areas where Ilpo directs and Claude executes. Both are logged in
[CREDITS.md](CREDITS.md), and both stay exceptions rather than
precedent.

**Music.** Ilpo does not use a tracker. Claude may author pattern data,
but only under explicit per-section direction — mood, energy,
instrumentation, structure, what changes where — with every iteration
judged by Ilpo listening to a render. Claude cannot hear; it supplies
candidates, Ilpo decides. Samples come from freeware packs with
recorded licenses, or Ilpo makes them himself with a synth or sample
editor. Claude never generates sample data.

**Effect implementation from specification.** Implementing an effect
whose algorithm, tables, palette and motion Ilpo has specified. The
test: would a viewer be seeing his decisions or Claude's?

## Provenance

Every part, every asset and every piece of music gets a line in
[CREDITS.md](CREDITS.md), recording who designed it and who
implemented it, updated in the same commit as the work. The entry's
info file is generated from it, and it states plainly where AI
assistance was used.

## Working here

- Engineering findings specific to this production go in
  [NOTES.md](NOTES.md), not the global `doc/TODO/`.
- `runtime/` is this production's own framework. It is deliberately
  not shared with `benchmark/` or with a future production, which may
  target different hardware and want different assumptions.
- The measured cost of every framebuffer access pattern is in
  NOTES.md. Cost claims are checked against it, not estimated.
