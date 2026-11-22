# Penumbra CPU — Dhrystone Comparison

DMIPS/MHz of each Penumbra generation beside cores of the same
microarchitecture class, how the peer figures were chosen, and what
the scores are made of.

Each Penumbra generation stands for an era of CPU design
([project context](./internals/penumbra2/overview.md#project-context-learning-cpu-design-evolution)).
This document places each one among the cores of its era, and among
today's soft cores of the same shape, by the one benchmark nearly all
of them have a published figure for.

DMIPS/MHz is Dhrystone 2.1 iterations per second, over the 1757 a
VAX-11/780 runs, per MHz of CPU clock. The clock cancels:

```
DMIPS/MHz = 1,000,000 / (1757 × CPU cycles per iteration)
```

so a score is path length (instructions per iteration, which the ISA,
the compiler and the string routines decide) times CPI (which the core
decides), and nothing else.

## Penumbra's figures

Samples of 2026-10-03, and of 2026-11-21 for Penumbra/3 on the ULX3S
at `0cb08b8`, 100,000 iterations each, from the harness's
performance-counter line. Like every figure of this kind in the
repository they are dated samples, not reference numbers.

The harness reads the counters over the interval Dhrystone itself
times, from `Begin_Time` to `End_Time`. The 2026-10-03 samples come
from a harness that read them around the whole program, so they also
hold the wait for Dhrystone's 1,815 characters of output at 115200
baud and the instructions of the loop that polls for it: about 39
cycles an iteration at Penumbra/1's 25 MHz and 59 at the others'
37.5 MHz, which puts Penumbra/1 1.6 % low, Penumbra/2 3.3 % and
Penumbra/2.5 4.6 %.

| Generation | Cycles / iteration | Instructions / iteration | CPI | DMIPS/MHz |
|---|---|---|---|---|
| Penumbra/1 | 2475 | 611 | 4.05 | 0.230 |
| Penumbra/2 | 1772 | 612 | 2.90 | 0.321 |
| Penumbra/2.5 | 1276 | 619 | 2.06 | 0.446 |
| Penumbra/3 | 723 | 599 | 1.21 | 0.787 |

To re-measure, run `DHRYSTON.ELF` on the board and put the cycle count
of its `CPU perfctrs` line, over the iteration count, into the formula
above. That takes neither the timer nor the clock frequency.

## How the peer figures were chosen

Most rows come from the Dhrystone table of the Performance Database
Server at netlib, 328 results collected by Alfred Aburto up to
November 1996. The Source column gives the row number in that table,
and [the netlib rows](#the-netlib-rows) lists each row's machine,
clock and compiler.

- **The best Dhrystone 2.1 row of each CPU.** Rows for one CPU differ
  by compiler far more than by machine: one SPARCstation 2 spans 0.25
  to 0.72. The best row is the one closest to what the core can do.
- **Dhrystone 1.1 only where a CPU has no 2.1 row.** Version 1.1 lets
  a compiler delete more of the benchmark, so those figures read high
  against 2.1.
- **Rows the table's maintainer marked "too high"** are passed over,
  and noted where one exists.
- **Two rows are left out.** Row 006, at 1.44, is filed under the
  21064 but is an AlphaServer 2100 5/250, a 21164 machine. Row 100,
  at 1.38, gives an SGI Crimson's R4000 its 50 MHz external clock;
  the pipeline runs at 100 MHz.
- **MHz is the pipeline's clock**, so a clock-doubled part counts at
  its internal rate.

Rows from any other source are that vendor's or project's own figure,
from a current compiler and mostly under Dhrystone's no-inlining rule.
They compare with each other better than with the 1990s rows.

## Microcoded and non-pipelined cores

| Core | Defining characteristics | DMIPS/MHz | Source |
|---|---|---|---|
| MC68000 | microcoded, prefetch only, no cache, 16-bit bus | 0.075 ¹ | netlib 306 |
| Nios II/e | non-pipelined soft core, no cache | 0.15 | Altera |
| MC68020 | microcoded, 3-stage instruction pipe, 256 B I-cache | 0.16 ¹ | netlib 269 |
| MC68030 | 68020 core, 256 B D-cache, MMU on chip | 0.18 | netlib 262 |
| VAX-11/780 | microcoded TTL CISC, 200 ns cycle | 0.19 ¹ | netlib 285 |
| **Penumbra/1** | horizontal microcode, non-pipelined, 1 KiB direct-mapped L1s | **0.23** | [measured](#penumbras-figures) |
| 80386DX | microcoded, multi-cycle execute, no on-chip cache | 0.35 ² | netlib 237 |
| PicoRV32 | non-pipelined multi-cycle RV32IM soft core, no cache | 0.52 ³ | PicoRV32 README |

¹ Dhrystone 1.1: the table has no 2.1 row for this CPU.

² An Am386DX-40. The one Intel-labelled 386 row, netlib 251, reads
0.29.

³ At a CPI of 4.1. Without its look-ahead memory interface, 0.31.

## Single-issue in-order pipelines

| Core | Defining characteristics | DMIPS/MHz | Source |
|---|---|---|---|
| **Penumbra/2** | 6-stage, flag-only forwarding, no prediction, 4 KiB 2-way L1s | **0.32** | [measured](#penumbras-figures) |
| ARM3 | 3-stage, 4 KB unified cache | 0.42 | netlib 241 |
| **Penumbra/2.5** | Penumbra/2 plus result forwarding and a BTB for direct branches | **0.45** | [measured](#penumbras-figures) |
| VexRiscv | 5-stage RV32IM soft core | 0.52–1.44 ¹ | VexRiscv README |
| i486DX | 5-stage pipelined CISC, 8 KB unified cache | 0.58 ² | netlib 205 |
| ARM610 | 3-stage, 4 KB unified cache, MMU | 0.61 | netlib 214 |
| SPARC MB86900 | 4-stage, register windows, gate array, off-chip cache | 0.63 | netlib 254 |
| SPARC CY7C601 | 4-stage, register windows, 64 KB off-chip cache | 0.72 | netlib 165 |
| Nios II/s | 5-stage soft core | 0.74 | Altera |
| MC68040 | 6-stage pipelined CISC, 4 KB + 4 KB caches | 0.77 | netlib 197 |
| **Penumbra/3** | 7-stage, full forwarding, branch prediction at fetch, store buffer, 8 KiB 2-way L1s | **0.79** | [measured](#penumbras-figures) |
| Cortex-M0 | 3-stage, no cache | 0.84–0.9 | Arm |
| R3000 | 5-stage, delayed branches, off-chip caches | 0.89 ³ | netlib 194 |
| ARM7TDMI | 3-stage, no cache | 0.9 | Arm |
| microSPARC-II | 5-stage, 16 KB I + 8 KB D caches, MMU on chip | 0.91 | netlib 112 |
| StrongARM SA-110 | 5-stage, 16 KB + 16 KB caches | 1.02 | netlib 016 |
| R4000 | 8-stage superpipelined, 8 KB + 8 KB L1, off-chip L2 | 1.09 | netlib 062 |
| MicroBlaze v5 | 5-stage soft core | 1.15 | Microprocessor Report |
| Nios II/f | 6-stage soft core | 1.16 | Altera |
| Cortex-M3 | 3-stage, no cache | 1.25 | Arm |
| LEON3 | 7-stage SPARC V8 soft core | 1.4 | Gaisler |
| PA-7000 | 5-stage, large off-chip caches | 1.43 | netlib 074 |
| PA-7100 | 5-stage; one integer and one FP instruction a cycle | 1.53 | netlib 123 |
| Cortex-A5 | 8-stage | 1.57 | Arm |
| SiFive E31 | RV32IMAC | 1.61 | SiFive |

¹ By configuration, built with GCC and no inlining: 0.52 "small" with
no bypass, 0.82 "small and productive", 1.21 "full" with 4 KB caches
and static branch prediction, 1.44 with nearly every feature on.

² Row 155, MetaWare High C -O4 on the same 33 MHz part, reads 1.01
and carries the maintainer's "too high" mark.

³ Row 194 records no compiler. Row 163, a DECstation 5000/133 with
cc -O4, reads 0.88.

## In-order dual issue

| Core | Defining characteristics | DMIPS/MHz | Source |
|---|---|---|---|
| **Penumbra/4** | two pipelines in lockstep, fusion at pre-decode; not built | — | — |
| MC68060 | instruction converter feeding two 4-stage pipelines | 0.9 ¹ | Wikipedia |
| Alpha 21064 | 7-stage, 8 KB + 8 KB caches, 64-bit | 1.07 | netlib 014 |
| HyperSPARC | SPARC V8, off-chip cache | 1.09 | netlib 041 |
| PA-7100LC | 5-stage, two integer ALUs | 1.48 | netlib 081 |
| Pentium (P5) | 5-stage, U and V pipelines in lockstep, 8 KB + 8 KB caches, BTB | 1.53 ² | netlib 066 |
| Cortex-A7 | 8-stage, partial dual issue | 1.9 | Arm |
| Cortex-A8 | 13-stage | 2.0 | Arm |
| Cortex-A53 | 8-stage | 2.3 | Arm |

¹ About 80,000 Dhrystones 2.1 a second at 50 MHz, on Amiga
accelerator cards. Motorola's own figures come to 1.3–1.5 "MIPS" per
MHz and name no benchmark.

² From Intel's performance brief of March 1993. The best independent
row, netlib 078 (a P5-60 with MetaWare High C -O4), reads 1.52.

## Wider in-order cores

Past anything on the Penumbra roadmap, for scale.

| Core | Defining characteristics | DMIPS/MHz | Source |
|---|---|---|---|
| UltraSPARC-I | 4-issue, 9-stage | 1.07 | netlib 026 |
| PowerPC 601 | 3-wide dispatch, one integer instruction a cycle | 1.42 | netlib 058 |
| SuperSPARC | 3-issue | 1.46 | netlib 116 |
| Alpha 21164 | 4-issue, 7-stage | 1.55 | netlib 001 |

The out-of-order cores in the same table: Pentium Pro 2.23 (netlib
003), PowerPC 604 1.49 (netlib 019, built with `-Ainline=%all`),
R10000 1.10 (netlib 015), K5 1.01 (netlib 092).

## What the comparison says

**The generations differ by CPI alone.** All four run the same loop
of 599 instructions an iteration, the 611–619 of the older samples
counting the output's polling as well, so the step from 0.23 to 0.32
to 0.45 to 0.79 is the step in CPI from 4.05 to 2.90 to 2.06 to 1.21.
Penumbra/3 passes the i486DX and the MC68040.

**Path length sets the ceiling.** At CPI 1.0 those instructions score
0.95, which is the most a single-issue Penumbra can reach, a little
above the R3000. Penumbra/3 is at 83 % of it.

**The peers above 1.0 run shorter paths.** VexRiscv's 1.44 is 395
cycles an iteration in all. PicoRV32 has Penumbra/1's CPI and scores
0.52, because its build of Dhrystone runs about 270 instructions an
iteration. That gap is the ISA's, the compiler's and the string
routines', not the core's.

**Dual issue is what moves the ceiling.** The
[gen4 pairing measurements](./internals/penumbra4/issue-pairing.md#fusion-and-dual-issue-together)
put 1000 instructions at 617–681 issue cycles on `pbench kernel` and
Doom. If Dhrystone paired the same way, the same path length would
reach about 1.4–1.55, among the Pentium and the PA-7100LC.

## The netlib rows

The machine, clock and compiler behind each netlib row the tables
take a figure from.

| Row | Machine | Clock | Compiler |
|---|---|---|---|
| 001 | AlphaServer 8400 5/300 | 300 MHz | DEC cc -O5 -ifo -tune ev5 |
| 014 | DEC 10000/610 | 200 MHz | not recorded |
| 016 | Acorn RiscPC | 200 MHz | Norcroft C 5.06 |
| 026 | Sun Ultra 1 | 167 MHz | gcc 2.6.0 -O2 -msupersparc |
| 041 | SPARCstation 20/HS21 | 125 MHz | gcc 2.6.0 -O2 -msupersparc |
| 058 | Power Macintosh 8100/80 | 80 MHz | xlc 1.0.2 -O3 -Q |
| 062 | SGI Indigo2 | 100 MHz | cc 3.10.1 -O -mips2 |
| 066 | Intel Pentium P5-66 | 66 MHz | not recorded |
| 074 | HP 9000/750 | 66 MHz | HP C 9.61 +O4 +Oall |
| 081 | HP 9000/712 | 60 MHz | HP C 9.61 +O4 +Oall |
| 112 | SPARCstation Voyager | 60 MHz | gcc 2.5.8 -O2 -finline-functions |
| 116 | SPARCstation 10/30 | 36 MHz | gcc 2.4.5 -O2 -mv8 |
| 123 | HP 9000/715 | 33 MHz | HP C 9.61 +O4 +Oall |
| 165 | SPARCstation 2 | 40 MHz | gcc 2.5.6 -O2 |
| 194 | MIPS 3230 | 25 MHz | not recorded |
| 197 | Amiga 2000 with FusionForty | 28 MHz | SAS C 6.2, inlining on |
| 205 | 80486DX/33 | 33.3 MHz | SCO cc 3.2.4 -O |
| 214 | Acorn RiscPC | 30 MHz | gcc 2.7.2 -O3 |
| 237 | Am386DX-40 clone | 40 MHz | Watcom C 10.5 -otexan |
| 241 | Acorn A5000 | 33 MHz | Norcroft C 5.06 |
| 254 | Sun 4/280 | 16.7 MHz | Sun C 1.0 -fast -O4 |
| 262 | Sony NWS-1750 | 25 MHz | gcc 2.4.5 -O2 -finline-functions |
| 269 | MASSCOMP 5600 | 16.7 MHz | cc 4.0 |
| 285 | VAX-11/780 | 5 MHz | cc 4.1.1.31 |
| 306 | Amiga 1000 | 7.16 MHz | Manx C 2.30a, 16-bit `int` |

## Sources

- [netlib Dhrystone table](https://www.netlib.org/performance/html/dhrystone.data.col0.html), its [compiler notes](https://www.netlib.org/performance/html/dhrystone.notes.html) and [introduction](https://www.netlib.org/performance/html/dhrystone.intro.html)
- [PicoRV32 README](https://github.com/YosysHQ/picorv32)
- [VexRiscv README](https://github.com/SpinalHDL/VexRiscv)
- Altera, *Nios II Processor Reference Handbook*, March 2009, "Nios II Core Implementation Details"
- Tom R. Halfhill, ["Xilinx Revs Up MicroBlaze"](https://halfhill.com/mpr/2006/204601.pdf), *Microprocessor Report*, 13 November 2006
- [LEON3 processor](https://www.gaisler.com/index.php/products/processors/LEON3), Gaisler
- [List of ARM processors](https://en.wikipedia.org/wiki/List_of_ARM_processors) and [Instructions per second](https://en.wikipedia.org/wiki/Instructions_per_second), Wikipedia, for Arm's published figures
- [Motorola 68060](https://en.wikipedia.org/wiki/Motorola_68060), Wikipedia
- [SiFive E31 launch](https://www.cnx-software.com/2017/05/05/sifive-launches-32-bit-e31-coreplex-64-bit-e51-coreplex-risc-v-processors-reveals-pricing/), CNX Software
- [AlphaServer](https://en.wikipedia.org/wiki/AlphaServer) and [R4000](https://en.wikipedia.org/wiki/R4000), Wikipedia, for the two rows left out
