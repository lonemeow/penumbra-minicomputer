# Penumbra/4 — The register file: four reads, two writes

> **Applies to:** Penumbra/4 · register file (ideas and measurements;
> nothing is built).

Two ops a cycle read up to four registers and write up to two. gen3's
file has two read ports and one write port: entries 1–13 in distributed
RAM, one copy of the array per read port, with R0 and the two stack
pointers supplied by a mux behind the array
([gen2 register file](../penumbra2/regfile.md#storage-organisation), the
layout gen3 keeps). Copying the array buys read ports. A second write
port needs a different structure, and this records which.

## The candidates

For R read ports and W write ports, after "Composing Multi-Ported
Memories on FPGAs" (LaForest, Li, O'Rourke, Liu and Steffan) and the
[overview of it](https://tomverbeure.github.io/2019/08/03/Multiport-Memories.html)
that prompted this:

| Structure | How | Arrays | At 4 reads, 2 writes |
|---|---|---|---|
| Flip-flops | Every bit a flop, a mux a port | none | 13 × 32 flops |
| Live value table | A bank per write port, each copied per read port; a table names the bank that wrote an entry last, and the read selects by it | R × W | 8, and a 16-flop table |
| XOR | A bank per write port. A write stores its data XORed with the other bank's word; a read XORs the banks | W × (W − 1 + R) | 10 |

The paper's comparisons are for block RAM thousands of entries deep,
read through a register. This file is 16 entries of distributed RAM,
read combinationally in the stage that decodes, so they do not carry
over.

## Measurement setup

Measured 2026-10-03 with scratch RTL, not in the tree: each structure
at 16 × 32 with gen3's entry layout, alone between registers on every
input and output.

- **ECP5:** yosys and nextpnr for the ULX3S's part, five seeds.
- **Artix-7:** Vivado for the Nexys Video's part, out of context,
  against a clock no structure meets, once with the default flow and
  once with one that spends more effort on timing.

The read path and the write path are timed apart, each on a clock of
its own: read index to read data, and write index and data to the
arrays and flops. The read path is the one that counts. Its data feeds
the rest of the stage that reads the file, while a write ends at the
array with its stage to itself. Each figure is the period the path
closes at, register to register, in nanoseconds.

The three two-write structures read the same over 200,000 cycles of
random traffic; when both ports write one entry, the younger op's write
stands. Every array mapped to distributed RAM under both tools.

## Measured on the ECP5

| Structure | LUT4s, packed | Flops in the file | Read path, ns | Write path, ns |
|---|---|---|---|---|
| 2 reads, 1 write: gen3's shape | 275 | 64 | 4.2–4.7 | 3.3–4.4 |
| 4 reads, 1 write: four copies | 503 | 64 | 4.5–4.7 | 3.6–4.2 |
| 4 reads, 2 writes: flip-flops | 2,097 | 480 | 6.9–7.4 | 5.6–6.1 |
| 4 reads, 2 writes: live value table | 1,066 | 80 | 8.2–9.0 | 6.1–6.7 |
| 4 reads, 2 writes: XOR | 992 | 64 | 5.2–5.6 | 5.1–5.4 |

Each LUT4 count includes about 40 of test wrapper.

- **XOR reads within a nanosecond of a one-write file,** and is the
  smallest of the three. Its read is the array read and one XOR level,
  which packs in with the mux that supplies R0 and the stack pointers.
- **The live value table reads 4 ns slower.** Sixteen entries make its
  table as deep to read as the data, and that read sits ahead of the
  output mux.
- **Flip-flops read 2.5 ns slower and take twice the LUTs:** a
  sixteen-way mux is several LUT4 levels.

## Measured on the Artix-7

| Structure | LUTs | of them RAM | Flops in the file | Read path, ns | Write path, ns |
|---|---|---|---|---|---|
| 2 reads, 1 write: gen3's shape | 155 | 44 | 64 | 2.8–2.9 | 2.4–2.7 |
| 4 reads, 1 write: four copies | 265 | 88 | 64 | 2.7–2.9 | 2.5–2.7 |
| 4 reads, 2 writes: flip-flops | 819 | 0 | 480 | 2.4–2.5 | 3.1 |
| 4 reads, 2 writes: live value table | 558 | 176 | 80 | 3.2–3.3 | 3.0 |
| 4 reads, 2 writes: XOR | 625 | 220 | 64 | 3.2–3.4 | 3.6–4.1 |

Each LUT count includes about 40 of test wrapper. The flip-flop file
also takes 384 of the slices' wide-mux primitives.

- **The order is not the ECP5's.** Flip-flops read fastest here, faster
  than a one-write file in distributed RAM: a LUT6 and the slice's wide
  muxes make a sixteen-way mux in one LUT level. The live value table
  is the smallest.
- **XOR and the live value table read alike,** about half a
  nanosecond behind a one-write file.
- **XOR's write is its slowest path:** the other bank's word is read,
  XORed and fanned out to five copies of the array.
- **The primitive's three read ports buy nothing.** One array read at
  four addresses maps to the same 24 primitives as four copies of it,
  with the same timing, so the copy per read port that the coding
  standards ask of distributed RAM costs nothing here.

## Design

**XOR banks over distributed RAM, the same RTL on both parts.**

- **The ECP5 is where the read path has no room,** and there XOR costs
  it under a nanosecond where the other two cost 2.5 and 4.
- **On the Artix-7 any of the three fits.** The spread between them is
  about a nanosecond of read path, and XOR's write path, the slowest
  measured, is 4 ns alone.
- **It is the smallest on the ECP5,** which is routing-bound, and
  within 70 LUTs of the smallest on the Artix-7.

What would reopen it: the read path of the file placed inside a full
top, beside the stage that reads it.

### What XOR asks for

- **A write reads the other bank first:** one more copy of each bank,
  addressed by the other port's write index, and a LUT level ahead of
  the write. Distributed RAM reads combinationally, so it costs no
  cycle.
- **Two writes to one entry** have to become one: the older op's is
  dropped.
- **The banks need no reset.** Whatever they hold, a register reads as
  the XOR of its two words, and both start as the bitstream leaves
  them.

## Ruled out

- **A live value table.** The slowest read on the ECP5 by 3 ns, for a
  structure no smaller than XOR there.
- **Flip-flops.** Twice the LUTs on the ECP5 and a read nearly 2 ns
  behind XOR's. They read fastest on the Artix-7, where the read path
  has room to spare.
- **Multi-pumping:** clocking the array at twice the core's rate and
  writing in turns. It puts a second clock inside the core.
- **Banking as a pairing rule:** one write port for each half of the
  registers, and no pair whose two writes land in one half. In the
  either-order stage of [issue-pairing](./issue-pairing.md) with the
  fusions, the younger op of a pair writes a register in 78–88 % of
  pairs, and the older one nearly always does. A split down the middle
  would refuse a large share of them; how many was not counted.
- **One write port,** for the same reason.
- **A third write port.** Four reads and two writes are already the
  expensive part of the file on both parts. What would write a third
  register in a cycle pairs with less or takes a cycle more instead,
  as a fused op that writes two registers does, and as `MUL` and `DIV`
  may ([structure](./structure.md#open)).

## Open

- **Inside a full top.** A file placed alone closes far above what it
  does beside the logic that reads it, and the order may not survive.
- **MUL and DIV** write two registers and sequence them through one
  port today; with two ports they could write both at once.
