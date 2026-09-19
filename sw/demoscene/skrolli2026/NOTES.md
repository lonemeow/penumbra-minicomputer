# Engineering notes

Findings specific to this production. Anything of general interest to
the machine belongs in `doc/TODO.md` instead.

## Target

| | |
|---|---|
| Core | penumbra2_5 @ 37.5 MHz |
| Display | 320x240, 8bpp indexed, pixel-doubled to 640x480 @ 60 |
| Palette | 256 entries, `0x00RRGGBB`, one global set |
| Audio | `CLASS_AUDIO`, 4 channels with hardware `INC` and `VOL` |
| RAM | 32 MB SDRAM |
| Delivery | HDMI capture (video) + line-in (audio), cut to 1080p60 |
| Length | ~3 minutes; the compo reserves at least that per entry |

Output is DVI signalling with no audio islands, so video and audio are
captured separately and aligned in post.

## Audience and framing

Skrolli Party's audience splits roughly in two: people who care about
stall cycles, cache ratios and CPU internals, and people who are there
for the aesthetics. **This entry targets the first group.** The
hardware cannot out-render anything modern, so competing on spectacle
is a losing proposition; what it has that nothing else in the compo
has is that every layer of it was built from scratch, and that the
machine's internals are legible while it runs.

That is a design constraint, not a mood. It means the demo should
*expose* what it is doing rather than hide it, and it means anything
shown as telemetry has to be real — a decorative fake would be spotted
immediately by exactly the people the entry is aimed at.

## Structure

Three phases, in order. Detail within each is deliberately unresolved
until effects have been measured on hardware.

1. **Boot, filmed externally.** Phone footage of the whole setup —
   board, display, speakers, cabling, a terminal showing the ROM
   prompt — booting the demo. Establishes that this is real hardware
   rather than an emulator, which is the entry's whole premise.
2. **Explanatory section, text-heavy.** What the machine is, how it
   compares to its contemporaries, and what the telemetry overlay
   means. Cheap effects carry it visually so it is not a wall of
   text, but the text is the point. This is also where the overlay is
   introduced, so the technical audience can read it for the rest of
   the run.
3. **Effects section.** The heavier parts, with the overlay running.

## Telemetry overlay

A persistent bar showing what the machine is doing — stall
attribution, cache hit ratios, retired instructions — introduced in
phase 2 and running through phase 3.

Cost is negligible: twenty `RDSYS` reads (eight CPU counters from
device 1, four each from L1-D, L1-I and L2) is ~20-40 cycles, and a
320x24 bar is 1920 packed stores, ~9.4 K cycles — 0.75% of a 30 fps
frame. Redrawing only changed glyphs costs less.

### It draws a bar, not text

Rendering numbers would mean blitting glyphs, and an 8x8 CP437 font is
2 KB against a 4 KB L1-D — a text overlay would evict roughly half the
running effect's working set every frame. That cost never appears in
the overlay's own cycle count; it appears as the effect getting
slower. A color bar reads no memory at all: counters come from sysregs
rather than loads, and everything else is stores.

The stall counters make this exact rather than approximate.
`penumbra2_perfctr.sv` charges each non-retiring cycle to exactly one
of six mutually-exclusive buckets, with a hardware assertion, so

    CYCLES = INSNS_RETIRED + Sum(stall)

Seven quantities that partition every cycle. A stacked bar sums to
100% by construction — no remainder, no double-counting, no
normalization fudge. Attribution is by carried cause: the bubble is
tagged where it is injected and charged at commit, so a long stall's
latency tail lands on its true cause instead of leaking into the
front-end residual.

Costs, per frame:

| | |
|---|---|
| Summing 8 counter deltas into the window | 16 ALU ops |
| Recompute, 10 times a second: 7 divides and the CPI | ~400 cycles |
| Bar row, graphics (320 px, 80 segments) | 80 word stores |
| Bar row, text (80 cells) | 80 word stores |
| 8-pixel-tall graphics bar | 640 stores, 0.25% of a 30 fps frame |
| One cell-row text bar | 80 stores, 0.03% |
| Memory read | none |

Both modes come out at 80 steps for 80 stores — a 320-pixel bar in 80
segments is 4 pixels each, exactly one word, and a text bar is 80
cells, exactly one word each. The bar logic is therefore the same
shape in both and only the word encoding differs. Graphics mode can go
to 320 segments at 1-pixel resolution for the *same* store count by
composing each word from four color bytes; text mode caps at 80, or
160 using left/right half-blocks.

The bar shows six visible segments for the seven buckets: hazard and
flush share a colour and sit adjacent, so they read as one "pipeline
waiting on itself" segment. penumbra2_5's GPR forwarding leaves few
hazard stalls, so separating them would spend a colour on a
distinction the audience would rarely see. Phase 2 explains them as
one category.

Two requirements:

- **Sample around the part, not around the frame.** Snapshot the
  counters before and after the part's render and draw the bar
  outside that window, or the telemetry measures its own drawing.
- **Never fake or smooth it.** The audience this is aimed at will
  read the numbers against what is on screen.

A part fast enough to draw many frames a second makes a per-frame bar
flicker too fast to read, so counts are summed over a tenth-of-a-second
window and the bar recomputed once per window. That is not smoothing:
a sum of partitions is a partition, so the bar is exact for the
window. It is still redrawn every frame, since the part may have drawn
over those rows, and each part starts its own window so its first
frame shows its own cycles.

**Cache ratios are not cycle shares.** Hit rates are fractions of
accesses and do not sum with the stall partition; putting them in the
same stacked bar would be the one thing this audience would catch.
They need a separate element with its own scale.

The overlay must work in **both** modes. `FB_SEL` is whole-screen and
there is no mid-frame switch, so phase 3 needs a software-drawn bar
regardless — one interface, two backends, chosen by the director.

Because `FB_SEL` is exclusive, an overlay on top of a framebuffer
effect is software-drawn into the framebuffer; it cannot use the
character cells.

## Character console as a second scan-out source

`CTRL.FB_SEL` selects cells or pixels for the whole screen. On this
device the two have independent storage (`video_cell_ram` and
`video_fb_ram`) and selecting the framebuffer does not re-time the
output, so:

- A text screen loaded into the cells survives a framebuffer session
  untouched, and can be prepared in advance.
- Switching between them is one register write, instant, with no
  monitor resync — safe to do mid-demo on camera.
- The character console is 80x30, 16 colors, CP437, and costs **zero**
  rendering: the device scans the cells itself.

The cost is exclusivity — no pixel effect can share the screen with
cell text.

### What the built-in font gives

`font8x16.hex` is generated from NetBSD's `bold8x16`, which declares
`WSDISPLAY_FONTENC_IBM` — genuinely CP437, not a Latin-1 font mapped
into CP437 slots, so the block and shade glyphs are present. Worth
knowing because most wsfont faces are `WSDISPLAY_FONTENC_ISO`, where
0xB0 is a degree sign and none of this would work.

The **block** glyphs are exact. 0xDF and 0xDC split a cell into two
solid 8x8 halves, 0xDD and 0xDE split it vertically. Using 0xDF with
independent foreground and background gives an 80x60 grid of 8x8
blocks, each any of the 16 colors — 4800 addressable blocks for 2400
word stores, since one cell carries both halves.

The **shade** glyphs are sparse on their own, but the attribute byte
doubles them. Measured fill density:

| glyph | | fill |
|---|---|---|
| `0x20` | space | 0% |
| `0xB0` | light shade | 18.8% |
| `0xB1` | medium shade | 25.0% |
| `0xB2` | dark shade | 37.5% |
| `0xDB` | full block | 100% |

Taken literally that is a bad ramp — everything bunched at the dark
end, since this is a Boldface face rather than the standard VGA
dithers at 25/50/75. But density is a property of the glyph and which
color is ink is a property of the attribute, so swapping foreground
and background turns any density *f* into *1-f* at identical cost.
That gives a symmetric ramp:

**0 / 18.8 / 25 / 37.5 / 62.5 / 75 / 81.2 / 100**

No glyph in the font is a true 50% dither — everything between 40% and
62% fill is box-drawing, which reads as a line rather than a texture —
but 0xDF is an exact spatial 50%, so the midpoint is available as a
hard edge if not as a stipple. Nine usable levels per color pair.

Choosing foreground/background *pairs* rather than working within one
pair opens a much larger perceived-color space again, since a 37.5%
dither of two mid-palette colors lands somewhere entirely different
from 37.5% of white on black.

### Resolution: why text mode is not just a cheap framebuffer

Text mode is 80x30 cells of 8x16 pixels — **natively 640x480**, the
panel's real resolution. The pixel framebuffer is 320x240 doubled in
both axes, so one framebuffer pixel is a 2x2 screen block. Text mode
addresses four times as many screen pixels.

That advantage is only reachable through the *glyph shapes*, though.
Using the half-block trick gives an 80x60 grid of 8x8 screen blocks —
coarser than the framebuffer's 2x2 pixels by 16x in area. So for
raster and block content the framebuffer wins outright on detail, and
text mode's resolution buys nothing.

**Where the resolution is real is content built from glyphs that
already exist: text, and box-drawing line art.** Both render at full
8x16 detail at 640x480. Everything else in the default font is
letters, blocks and shades.

### What a soft font would change

A redefinable font is the classic route to high-resolution graphics on
hardware whose bitmap mode is too coarse — the cell grid becomes a
bitmap because every cell can carry arbitrary pixels.

The arithmetic is favourable. One word-strided font store carries 8
pixels at 1bpp against the pixel aperture's 4 at 8bpp:

| | stores | pixels | cyc/pixel |
|---|---|---|---|
| Font aperture, 1bpp | 4096 | 32768 | **0.61** |
| Pixel aperture, 8bpp | 19200 | 76800 | 1.22 |

A complete font rewrite is 4096 stores, ~20 K cycles, 1.6% of a 30 fps
frame. Cells only need rewriting when the layout changes.

The ceiling is glyph count. 256 glyphs x 128 pixels is 32768 pixels of
*unique* bitmap — **10.7% of a 640x480 screen**. An arbitrary
full-screen bitmap needs 2400 unique glyphs, so as a general
high-resolution framebuffer this does not work.

It works where unique cells scale with **outline length rather than
area**. A filled silhouette or a wireframe is mostly uniform: interior
cells are all solid, exterior cells all blank, and only cells the edge
crosses need their own bitmap. A figure occupying ~300x400 pixels
crosses roughly 200-250 cells, inside the budget — and deduplicating
identical cell patterns reduces it further, since many cells along a
near-vertical or near-horizontal edge share a bitmap. Hash each cell's
16 bytes, allocate one glyph per distinct pattern, fall back to the
nearest match on overflow so the failure mode is a slightly wrong edge
cell rather than a broken frame.

Worst case for an animating full-screen silhouette is 4096 font stores
plus 2400 cell stores — 6496 stores, ~31.8 K cycles, 2.5% of a 30 fps
frame, at 640x480.

**Not currently affordable.** `CAP.SOFTFONT` is clear and the EBR
budget has no room for a writable 4 KB font store alongside the audio
device's FIFOs and mixer. Recorded because the case is strong if
budget ever appears, and because it is the only route to
native-resolution output on this machine.

### What follows

**Draw straight into the aperture.** The display's BRAM is the fastest
memory in the machine — 29% quicker to write than SDRAM and 2.2x
quicker to read. Staging a frame in SDRAM and blitting costs more than
drawing directly, so there is no reason for a general backbuffer.

**Do not map the framebuffer cached.** Measured a net loss on every
row. An uncached read is only 6.75 cycles, so a 16-byte line fill
(~30 cycles through L1-L2-bus) has almost no latency to amortize. The
mechanism exists in `benchmark/common/crt0.S` for re-measuring, and is
off by default.

This conclusion is a property of the **write policy**, not the device:
under write-through/no-write-allocate a write miss streams straight
out while a write hit must also update both cached copies, so
residency costs more than it saves. A write-back/write-allocate L2
would invert it — re-run `fbtest` if that ever lands.

**Pack four pixels per store, always.** The single largest lever
available: byte stores cost 29% of a 30 fps frame against 7.5% for
packed words. That 21.6-point swing is larger than most effects'
entire compute budget.

**Read-modify-write is affordable.** A full-screen RMW pass is 25% of
a 30 fps frame, uncached, straight to the aperture. Fire, blur,
feedback and motion trails are all reachable.

## Frame budget

At 37.5 MHz a 30 fps frame is 1,250,000 cycles; 76800 pixels gives
**16.3 cycles per pixel**, of which the store takes 1.22.

| Full-frame operation | cycles | share of a 30 fps frame |
|---|---|---|
| packed word stores | 93.9 K | 7.5% |
| word stores, column walk | 112.5 K | 9.0% |
| uncached read | 129.8 K | 10.4% |
| uncached read-modify-write | 307 K | 24.6% |
| byte stores | 365 K | 29.2% |

Per-pixel budget at other rates: 8.1 cycles at 60 fps, 19.5 at 25,
24.4 at 20.

The framebuffer is not the bottleneck — compute is. Effects should be
priced by their arithmetic, with roughly 15 cycles per pixel available
at 30 fps after the store is paid.

## Arithmetic costs

Whether an operand is known at compile time decides the cost, because
constant operands are rewritten into ALU operations and variable ones
go to the divmul peer unit.

**Variable operands** take the hardware instruction: `MUL` is 10
cycles on penumbra2_5 and `DIV` 34, against 1 for an ALU operation
(per-core figures in `doc/internals/instruction-timing.md`). One
variable multiply per pixel is two thirds of a 30 fps budget, and a
variable divide per pixel is out entirely — the reciprocal-table form
is the substitute, for perspective division and voxel projection
alike. Per-span and per-vertex multiplies are cheap enough that
geometry-heavy parts are the ones that benefit most from this core.

**Constant operands** are strength-reduced. Measured on the demo's
tuning, `-O2`:

| Expression | instructions | cycles |
|---|---|---|
| `x * 80`, `x * 320` (row strides) | 4 | 4 |
| `fb + y * 320` | 5 | 5 |
| `x * 100`, `x * 1000` | 7 | 7 |
| `x * 0x01010101` (colour broadcast) | 8 | 8 |
| `x % 256`, `x / 8` unsigned | 1 | 1 |
| `x % 8` / `x / 8` signed | 7 / 5 | 7 / 5 |
| `x / 5`, `x / 320` unsigned | 5 | 14 |
| `x % 248` unsigned | 11 | 20 |
| `x * 0x12345679` (dense) | 3 | 12 — left as `MUL`, correctly |

So a stride or broadcast by a constant is fine inside a per-pixel
loop, and a power-of-two wrap is a single `ANDi`. A non-power-of-two
modulo still goes through a multiply-high and costs about twenty
cycles, so wrapping ramps mask to a power of two rather than dividing
by a palette size.

Three shapes come out an instruction or two longer than the ISA allows
— subtracting decompositions such as `x * 255`, the unfactored
broadcast constant (8 instructions where 6 would do), and the signed
power-of-two remainder. They are recorded in `doc/TODO.md` under the
constant strength-reduction entry; none of them is worth restructuring
an effect around.

The build pins `-mcpu=penumbra2_5` rather than relying on the
compiler's default tuning, which follows whichever core is most used
and may move. These decisions are per-core — penumbra1, for instance,
keeps constant divides as `DIV` because its multiply is no cheaper —
so the figures above hold only for the tuning the demo actually builds
with.

Fixed point is 16.16 through `runtime/fix16.h`. A multiply keeps the
middle of the full product, which `MUL` returns in one operation; a
divide has no 64/32 form to use, so its fraction comes from further
32/32 divides of the remainder, as many bits per `DIVU` as the divisor
leaves room for. Instructions on the path, and cycles before branch
penalties:

| Expression | instructions | cycles |
|---|---|---|
| `fix16_mul(a, b)` | 4 | 13 |
| `fix16_div(a, b)`, \|b\| < 1.0 | 38 | ~105 |
| `fix16_div(a, b)`, \|b\| < 256.0 | 51 | ~150 |
| `fix16_div(a, b)`, \|b\| < 4096.0 | 71 | ~235 |

Random numbers are `runtime/rand.h`, xorshift32 with a word of state
per part. There is no rotate instruction, so the rotate-based
generators cost three operations where this one costs one.

| Expression | instructions | cycles |
|---|---|---|
| `rt_rand(&r)` | 9 | 9 |
| `rt_rand_below(&r, n)` | 10 | 19 |
| `rt_rand_frac(&r)` | 10 | 10 |

Each part seeds its own stream, so draws in one part cannot shift
another's, and a seed replays exactly — what the capture shows is what
the machine does.

One `fix16_mul` per pixel is most of a 30 fps budget; a divide is per
span or per vertex. A part's progress through its cues divides by its
span, a whole number of rows, so it costs the 256.0 figure once a
frame for any part shorter than four patterns.

## Viewport size

The framebuffer is uncached, so its size does not interact with L2 at
all. A reduced viewport is worth having for the direct reason: fewer
pixels is a proportional saving in both the store and, more
importantly, the per-pixel compute that dominates.

| Viewport | pixels | vs full frame |
|---|---|---|
| 320x240 | 76800 | — |
| 320x192 | 61440 | 80% |
| 256x192 | 49152 | 64% |

Telemetry bars top and bottom are one way to arrive at a reduced
effect area for a reason the audience can see.

Where L2 residency *does* matter is SDRAM-side data — precalc tables
and source images, which are cached. That is sharpest for the fractal
part: a 320x240 keyframe is 76800 bytes and thrashes a 64 KiB L2 at
14.92 cycles per word, while a 256x192 keyframe is 49152 bytes, stays
resident, and reads at 8.15. Nearly 2x on that part's inner loop.

## Audio timing

Each track is one device channel. `INC`, `VOL` and `FLUSH` take effect
on the next output frame and only sample data queues, so the replayer
cannot run ahead: every tick runs from the timer interrupt when it is
due, and the replayer's song position *is* the heard position. Sync
needs no FIFO-lead compensation — only the analog path and capture
chain remain, a constant to measure once in post.

The timer period is one ProTracker tick at 125 BPM, 20 ms, and every
tick tops each queue up to full. 1024 frames last 32.6 ms at period
113, ProTracker's highest pitch, so a tick never drains a queue.
`tools/mod2song.py` rejects other tempos rather than play them wrong.

A refill is a word copy, 5 instructions per frame: samples are
expanded to device frames at startup, since widening per frame cost 4
more. The price is 4 bytes of cache per frame instead of 1 — a tick's
refill at typical pitches is ~5 KB, the whole L1-D, on every tick. The
telemetry bar is what says whether that trade holds up.

The mixer holds each source frame (no `INTERP`), which is Paula's
behaviour rather than a desktop player's default: compare against one
with interpolation off. Output is mono by construction — both lanes of
every frame carry the same sample, at equal gain.

Output is from the 48 kHz family; 44.1 kHz is unreachable from the
available PLL configurations. Amiga periods resample exactly through
the hardware `INC`, so no retuning is needed.

## Sync

Song time is the one clock parts see: rows since the song began, in
16.16, read as the heard row plus the elapsed share of it. Its
fraction comes from the timer's count within the current tick, so it
advances smoothly between ticks at any frame rate, and it is anchored
to the music rather than to when a frame happened to run. The three
uses need nothing more:

- **Ending a part on the music** — the cue table; a part runs from its
  cue's position to the next cue's.
- **Effect progress** — `(now - start) / (end - start)` against the
  part's cues.
- **On-beat events** — `now` masked to the beat's length in rows.

A cut lands at the first frame boundary after its cue, so it is late
by at most one frame of the outgoing part; with the framebuffer drawn
in place, nothing lands closer than that anyway. Time is
`order * 64 + row`, which is monotonic only while the song plays
straight through: the song is known, so cues are placed where it does.

## Load size

The ROM reads the ELF into a scratch buffer at 0x2000 and loads the
image directly above it, so the image ends near twice the file size.
`crt0.S` starts the stack at 1 MB growing down, so an ELF approaching
500 KB — less the stack's depth — runs its image into the stack;
`rt_mem_init()` panics when it does. Music and assets ride in the ELF,
so this is the budget they share until the stack moves.

Everything above the stack is the heap's, to the end of base RAM.
Anything derived at startup belongs there rather than in the ELF —
the replayer's samples, expanded from 8-bit bytes to device frames,
are 4x their size in the module.

The heap is a free list in address order, first fit, merging
neighbours as they come back. Parts take a few large buffers rather
than many small ones, so the list stays short and fragmentation comes
from the order parts hand blocks back, not from block sizes. That
order is never LIFO: a part frees from `leave()`, by which time the
next part's precalc has already allocated. Blocks start on an L2 line,
and a part that forgets to free surfaces as an out-of-memory panic
rather than as slow decay.

## Interrupt context

Ideas not yet acted on.

**R12 is free real estate.** The ABI reserves R12 as the thread
pointer and the compiler never allocates it, bare metal included
(`doc/system/abi.md`). Either make it allocatable for this build — one
more register for every function — or give it a job: compiled code
never writes it, so a value the runtime loads at startup survives
everywhere, including at whatever instruction an interrupt lands on.
An interrupt handler could reach its state as `[r12 + off]`, with no
PC-relative scratch and no address materialization.

**Pin what interrupt handlers touch.** Exceptions do not nest: entry
overwrites `EPC`/`ESR`, so a TLB miss taken inside an interrupt handler
destroys the interrupted context unless the handler saved both first.
Beyond correctness, a pinned handler costs the effect it interrupts no
TLB misses. The pinned TLB has 8 slots; `crt0.S` uses two (the handler
page and page 0). Ideally the handler code is fully pinned, plus one
data page holding its hottest state.

## Pinned data

`RT_PINNED` and `RT_PINNED_CONST` put a global in `.pinned`, whose
pages `crt0.S` maps through the pinned TLB slots left after the
handler page and page 0 — six, a limit `demo.ld` enforces at link
time. The main TLB is 64 entries in 2 ways, and a full-screen part
walks the framebuffer's 19 pages plus its own sources every frame, so
a hot table's entry is otherwise liable to be evicted and refilled
through the miss handler, a trap and 29 instructions. Pinning removes
that; it does nothing for the L1-D, where a 4 KB table would fill the
whole cache, so hot tables still want to be small.

The slots are written before the MMU is enabled, the one point where no
miss handler or interrupt can land between an entry's three `WRSYS`
writes. Pinning the interrupt handlers' C code would draw on the same
six slots.

## Precalc

Frames run free rather than at a target rate, but no faster than the
display refreshes, since a frame drawn inside one refresh of the last
is never seen. What a frame leaves of its refresh is precalc time, so
a part slower than the refresh donates none — the parts in front of
heavy precalc have to be cheap ones. Parts differ enormously in setup
cost, so the director runs two precalc lanes against that time:

- **next** — must complete before the upcoming transition.
- **background** — long-running, must complete by its own part.

The second lane exists for the fractal part. A Mandelbrot iteration is
three serialized multiplies plus the escape test, ~40 cycles, which
puts real-time recomputation out of reach by two orders of magnitude.
One deep 320x240 image at ~150 average iterations costs ~12 seconds,
so a keyframe-plus-resample zoom needs 40-50 seconds of precalc spread
across the whole demo.

`precalc_step()` must honor its deadline at fine granularity — a few
hundred microseconds of work per call — since there is no slack in a
frame to absorb an overrun.

## Open decisions

**Detail within phases 2 and 3.** Deliberately not fixed: several
candidate effects need measuring on hardware before it is known
whether they hold a frame rate, and the order depends on what
survives. Costed so far — plasma (cheap, no precalc),
repeating-pattern rotozoomer (cheap, 32x32 tile), voxel landscape
(expensive, heavy precalc), flat-shaded 3D with sprites (moderate,
low precalc), 2D vector silhouette (cheapest, data only), fractal
keyframes (free per frame, 40-50 s of background precalc).

Two constraints the eventual order has to satisfy: the voxel's tables
and the fractal's keyframes both need cheap parts in front of them,
and the fractal needs its 40-50 seconds spread across most of the
run. The palette is one global 256-entry set, so parts wanting
incompatible colors need a fade between them, or a partitioned range
to cross-fade instead.

**Title.** One string, decided whenever it lands; the directory is
named for the event so nothing waits on it.

**Music authorship.** Directed exception per [CLAUDE.md](CLAUDE.md);
the song gates the cue table, which gates every part's timing.
