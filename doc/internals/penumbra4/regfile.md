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

## Measured on the ECP5

Measured 2026-10-03 with scratch RTL, not in the tree: each structure
at 16 × 32 with gen3's entry layout, synthesized by yosys and placed
and routed alone on the ULX3S's part between registers on every input
and output, over five seeds. The three two-write structures read the
same over 200,000 cycles of random traffic; when both ports write one
entry, the younger op's write stands.

| Structure | LUT4s, packed | Flops in the file | fmax, MHz |
|---|---|---|---|
| 4 reads, 1 write: four copies | 503 | 64 | 211–224 |
| 4 reads, 2 writes: flip-flops | 2,097 | 480 | 139–144 |
| 4 reads, 2 writes: live value table | 1,066 | 80 | 116–128 |
| 4 reads, 2 writes: XOR | 992 | 64 | 174–193 |

Each LUT4 count includes about 40 of test wrapper. gen3's file alone is
about 240.

- **XOR is the smallest and the fastest of the three.** Its read is the
  array read and one XOR level, which packs in with the mux that
  supplies R0 and the stack pointers.
- **The live value table is the slowest.** Sixteen entries make its
  table as deep to read as the data, and that read sits ahead of the
  output mux.
- **Flip-flops take twice the LUTs** for a speed between the two.
- **A second write port costs about 500 LUT4s** over four read ports
  alone, and four read ports about 220 over two.

The fmax figures order the structures and nothing more: a module placed
alone closes far above what it does inside a full top.

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

- **Multi-pumping:** clocking the array at twice the core's rate and
  writing in turns. It puts a second clock inside the core.
- **Banking as a pairing rule:** one write port for each half of the
  registers, and no pair whose two writes land in one half. In the
  either-order stage of [issue-pairing](./issue-pairing.md) with the
  fusions, the younger op of a pair writes a register in 78–88 % of
  pairs, and the older one nearly always does. A split down the middle
  would refuse a large share of them; how many was not counted.
- **One write port,** for the same reason.

## Open

- **Artix-7.** Its distributed RAM primitive has three read ports
  beside its write port, which changes the number of copies each
  structure needs. gen3's file maps to twelve of them there. The
  comparison needs its own run under Vivado.
- **Inside a full top.** The ordering may not survive placement beside
  the stage that reads the file.
- **MUL and DIV** write two registers and sequence them through one
  port today; with two ports they could write both at once.
