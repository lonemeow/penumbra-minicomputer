# Text-Video Console — Programmer's Reference

A character-cell display for the Penumbra bus (`CLASS_TEXTVIDEO`). The
device holds a grid of character cells plus a built-in font; writing a
cell makes the device draw that glyph. It is the local-console analog of
the [UART](uart.md) — generic firmware or the OS can use any conformant
device as a console with no device-specific driver.

This document defines the **`CLASS_TEXTVIDEO` minimum protocol**: the
registers every device of the class implements, and the mode it powers up
in, after reset. A device may add richer features (color, additional
display modes, font upload, hardware scroll) behind capability flags or
`CFG_ID`, but must always present this interface — and power up in
[mode 0](#display-modes) — after reset.

## Register Interface

A small register block followed by a character-cell aperture. Base
address assigned by autoconfig.

**Access.** All registers and cells are accessed with 32-bit aligned
loads and stores only (`LDW`/`STW`); the device implements no byte-lane
enables. Every register and cell is *word-strided* — it occupies a
32-bit slot even where its value is narrower — the same convention the
[UART](uart.md) and [SPI](spi.md) controllers use. See the bus protocol's
[Access Width](../../hardware/bus-protocol.md#access-width).

| Offset    | Name         | R/W | Description                              |
|-----------|--------------|-----|------------------------------------------|
| `0x000`   | `CAP`        | R   | Version, capabilities, mode count        |
| `0x004`   | `INFO`       | R   | Active-mode geometry: columns and rows   |
| `0x008`   | `CTRL`       | R/W | Output enable, cursor enable             |
| `0x00C`   | `CURSOR`     | R/W | Cursor cell position                     |
| `0x010`   | `MODE_SEL`   | R/W | Active display mode (optional)           |
| `0x014`   | `MODE_QUERY` | W   | Mode index to query (optional)           |
| `0x018`   | `MODE_GEOM`  | R   | Geometry of the queried mode (optional)  |
| `0x1000…` | `CELLS`      | R/W | Character-cell aperture (one slot / cell)|

### Register Details

#### CAP (0x000)
- Bits [7:0]: Version (`1`)
- Bit [8]: `COLOR` — device renders the 16-color palette as distinct
  colors (see [Attributes](#attributes)); `0` = monochrome
- Bit [9]: `MODESWITCH` — more than one display mode, switchable via
  `MODE_SEL` (see [Display Modes](#display-modes))
- Bits [23:16]: `MODE_COUNT` — number of display modes (≥ 1; mode 0 is
  the mandated default)

Remaining bits reserved (read 0); further capabilities are advertised
through `CFG_ID`.

#### INFO (0x004)
- Bits [15:0]: `COLUMNS`
- Bits [31:16]: `ROWS`

Reports the geometry of the **active** mode. Geometry is discoverable so
one console back-end drives any text-video device — and any mode —
without hardcoding a grid size.

#### CTRL (0x008)
- Bit [0]: `ENABLE` — video output active
- Bit [1]: `CURSOR_EN` — show the cursor at `CURSOR`

#### CURSOR (0x00C)
- Bits [15:0]: `COL`
- Bits [31:16]: `ROW`

Every device implements a cursor (typically blinking) drawn at this cell
when `CTRL.CURSOR_EN` is set. Guaranteeing it in the minimum keeps
consumers simple — they never render one in software — at negligible
hardware cost: a cell-position comparator and a blink counter in the
scan-out path.

#### MODE_SEL / MODE_QUERY / MODE_GEOM (0x010–0x018)
Present when `CAP.MODESWITCH = 1`; see [Display Modes](#display-modes). On
a single-mode device these slots read 0.

#### CELLS (0x1000 …)
A row-major array of cells fixed at offset `0x1000`. The cell at
(`row`, `col`) is the slot at `0x1000 + (row * COLUMNS + col) * 4`,
accessed with `LDW`/`STW`, using the active mode's `COLUMNS`. The device's
`CFG_SIZE` window is sized for the largest mode's
`0x1000 + COLUMNS * ROWS * 4`.

A cell is **16 bits**, occupying the low half of its 32-bit slot; bits
[31:16] read 0 and are ignored on write. The cell value:
- Bits [7:0]: `GLYPH` — character code
- Bits [15:8]: `ATTR` — attribute (see below)

Storing 16-bit cells in word-strided slots keeps every access a single
aligned word (no read-modify-write, no byte enables) while the device's
backing memory is only two bytes per cell — half of a packed 32-bit
cell, which is what makes the larger grids affordable.

##### Attributes
`ATTR` is `FG` (bits [3:0]) and `BG` (bits [7:4]) — indices into the
standard 16-color palette (CGA/ANSI order: `0` black … `7` light grey …
`15` white). The mandated default is `FG=7, BG=0` (light grey on black),
i.e. `ATTR = 0x07`.

The encoding is uniform across all devices so generic firmware writes one
value everywhere, but **color rendering is not required**. A monochrome
device (`CAP.COLOR = 0`) collapses the palette to foreground/background —
index `0` is background, nonzero is foreground — so `0x07` (normal) and
`0x70` (inverse) still render distinctly. A color device
(`CAP.COLOR = 1`) renders all 16 indices. Color is thus optional and
discoverable, while a consumer that does not care writes `0x07` and works
on both.

## Display Modes

A **mode** is a console geometry — a `{columns, rows}` pair. The device
maps each mode internally to a pixel resolution and font; neither is
visible through this interface, which keeps the protocol resolution- and
font-agnostic.

**Mode 0 is mandatory.** After reset the device is active in mode 0 =
**640×480, 80×30** (an 8×16 cell at 640×480). 640×480@60 is the display
mode every monitor accepts, so a device powering up in it gives the boot
ROM the best chance of a visible picture before any driver runs — the
priority for a standalone machine. A device that supports only one mode
implements mode 0 and leaves `CAP.MODESWITCH = 0`; that is the whole of
the mandatory mode requirement.

**Additional modes are optional and discoverable.** When
`CAP.MODESWITCH = 1`, the device offers `MODE_COUNT` modes (mode 0 always
the default) and:

- `MODE_SEL` (R/W) — the active mode index. Writing it switches modes:
  video is re-timed, `INFO` updates to the new geometry, and **cell
  contents are undefined across a switch** (the aperture stride changes
  with `COLUMNS`), so the consumer re-initializes the screen. Reading
  returns the active index.
- `MODE_QUERY` (W) / `MODE_GEOM` (R) — write a mode index to `MODE_QUERY`,
  then read its `{columns, rows}` from `MODE_GEOM` (same layout as
  `INFO`) **without** changing the active mode, so a driver can build the
  mode list without disturbing the display.

This is how a richer device offers a roomier console while staying
boot-compatible: firmware and early boot run in mode 0; the OS driver
switches to a larger mode once it attaches. The same mechanism carries
forward across hardware — a device on a faster part simply lists higher
modes, with mode 0 unchanged and the driver unmodified.

## Scrolling

The minimum protocol has **no hardware scroll**: when output passes the
last row, the consumer scrolls by copying cell rows upward within `CELLS`
and clearing the freed row. A hardware scroll-offset register is a
permitted richer feature behind `CFG_ID`.

## Driver Flow

1. Read `INFO` for the active `COLUMNS` / `ROWS`; optionally read
   `CAP.COLOR`.
2. *(Optional, if `CAP.MODESWITCH`)* enumerate modes via
   `MODE_QUERY`/`MODE_GEOM`, write the chosen index to `MODE_SEL`, then
   re-read `INFO`.
3. Set `CTRL.ENABLE` (and `CURSOR_EN` for a visible cursor).
4. Print character `c` at (`row`, `col`):
   `CELLS[row * COLUMNS + col] = c | (0x07 << 8)`.
5. Advance the logical position; wrap at the last column, scroll past the
   last row, and write the new position to `CURSOR`.

## Relationship to the OS Console

The 16-bit `{glyph, attribute}` cell is deliberately the IBM-PC
text-buffer cell that NetBSD's character-cell wsdisplay back-end
(`pcdisplay`) already models, so the `CELLS` aperture maps directly onto
its character memory, and mode switching maps onto wsdisplay's
screen-type reconfiguration. Paired with a `wskbd` keyboard (see
[USB Host Controller](usb-host.md)), it forms a `wscons` local console.
