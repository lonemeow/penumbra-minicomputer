# Text-Video Console — Programmer's Reference

A character-cell display for the Penumbra bus (`CLASS_TEXTVIDEO`). The
device holds a grid of character cells plus a built-in font; writing a
cell makes the device draw that glyph. It is the local-console analog of
the [UART](uart.md) — generic firmware or the OS can use any conformant
device as a console with no device-specific driver.

This document defines the **`CLASS_TEXTVIDEO` minimum protocol**: the
registers every device of the class implements, and the mode it powers up
in, after reset. The mandatory core is deliberately small — a cell grid
that renders the printable 7-bit ASCII glyphs (see
[Character Set](#character-set)). Everything else — color, a
programmable palette, the extended glyph range, a reloadable soft font,
additional display modes, hardware scroll — is an optional feature
advertised through a capability flag or `CFG_ID`. A device may add any
of them, but must always present this interface — and power up in
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
| `0x008`   | `CTRL`       | R/W | Output/cursor enable, palette/font select|
| `0x00C`   | `CURSOR`     | R/W | Cursor cell position                     |
| `0x010`   | `MODE_SEL`   | R/W | Active display mode (optional)           |
| `0x014`   | `MODE_QUERY` | W   | Mode index to query (optional)           |
| `0x018`   | `MODE_GEOM`  | R   | Geometry of the queried mode (optional)  |
| `0x040…`  | `PALETTE`    | R/W | Custom-palette aperture, 16 slots (optional) |
| `0x1000…` | `CELLS`      | R/W | Character-cell aperture (one slot / cell)|
| `0x8000…` | `FONT`       | R/W | Soft-font aperture (optional)            |

### Register Details

#### CAP (0x000)
- Bits [7:0]: Version (`1`)
- Bit [8]: `COLOR` — device renders the 16-color palette as distinct
  colors (see [Attributes](#attributes)); `0` = monochrome
- Bit [9]: `MODESWITCH` — more than one display mode, switchable via
  `MODE_SEL` (see [Display Modes](#display-modes))
- Bit [10]: `EXTGLYPHS` — device renders the full `0x00`–`0xFF` glyph
  range as IBM CP437 (see [Character Set](#character-set)); `0` = only
  the baseline 7-bit ASCII glyphs are guaranteed
- Bit [11]: `PALETTE` — device has a programmable palette, loaded via
  the [`PALETTE`](#palette-0x040) aperture and selected by
  `CTRL.PAL_SEL`; only meaningful when `COLOR = 1`
- Bit [12]: `SOFTFONT` — device has a reloadable soft font, loaded via
  the [`FONT`](#font-0x8000) aperture and selected by `CTRL.FONT_SEL`
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
- Bit [2]: `PAL_SEL` — palette source: `0` = built-in default palette,
  `1` = the custom palette in the [`PALETTE`](#palette-0x040) aperture.
  Present only when `CAP.PALETTE = 1`; reads `0` and is ignored
  otherwise. Resets to `0`. The custom palette keeps its contents while
  deselected, so a consumer loads it once and toggles this bit to switch
  back and forth with no reload.
- Bit [3]: `FONT_SEL` — font source: `0` = built-in font, `1` = the
  soft font in the [`FONT`](#font-0x8000) aperture. Present only when
  `CAP.SOFTFONT = 1`; reads `0` and is ignored otherwise. Resets to `0`.
  The selection is global (the whole screen uses one font); the soft
  font keeps its contents while deselected, like `PAL_SEL`.

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

#### PALETTE (0x040)
Present when `CAP.PALETTE = 1`; otherwise the slots read 0. Sixteen
word-strided slots holding the custom palette, one `0x00RRGGBB` color
per slot: `PALETTE[i]` (at `0x040 + i*4`) is the color rendered for
attribute index `i`. The aperture is backing storage only — it drives
the screen exclusively when `CTRL.PAL_SEL = 1`. Writing it while
`PAL_SEL = 0` does not disturb the picture, so a consumer stages a new
palette and switches to it by setting one bit. Power-up contents are
undefined; load all 16 slots before selecting.

#### CELLS (0x1000 …)
A row-major array of cells fixed at offset `0x1000`. The cell at
(`row`, `col`) is the slot at `0x1000 + (row * COLUMNS + col) * 4`,
accessed with `LDW`/`STW`, using the active mode's `COLUMNS`. The device's
`CFG_SIZE` window covers the register block, every implemented aperture,
and the cell array for the largest mode (`0x1000 + COLUMNS * ROWS * 4`);
on a device with the soft font it extends to the end of the
[`FONT`](#font-0x8000) aperture.

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
active 16-color palette. The built-in default palette is CGA/ANSI order
(`0` black … `7` light grey … `15` white); a device with `CAP.PALETTE`
can substitute a custom palette through the [`PALETTE`](#palette-0x040)
aperture and `CTRL.PAL_SEL`. The mandated default is `FG=7, BG=0`
(`ATTR = 0x07`) — light grey on black in the built-in palette.

The encoding is uniform across all devices so generic firmware writes one
value everywhere, but **color rendering is not required**. A monochrome
device (`CAP.COLOR = 0`) collapses the palette to foreground/background —
index `0` is background, nonzero is foreground — so `0x07` (normal) and
`0x70` (inverse) still render distinctly. A color device
(`CAP.COLOR = 1`) renders all 16 indices. Color is thus optional and
discoverable, while a consumer that does not care writes `0x07` and works
on both.

## Character Set

Each cell's `GLYPH` is an 8-bit index into the device's font. The
**baseline guarantees only the printable 7-bit ASCII range**: codes
`0x20`–`0x7E` render as the corresponding ASCII characters. Codes
`0x00`–`0x1F` and `0x7F` are device-defined (typically blank), and the
high range `0x80`–`0xFF` is **not** part of the baseline — a minimal
device may render it blank. Generic firmware therefore stays within
`0x20`–`0x7E`.

**Extended glyphs (optional).** When `CAP.EXTGLYPHS = 1` the device
renders the full `0x00`–`0xFF` range as IBM **CP437** — the block,
shade, and box-drawing glyphs (`█ ▄ ▀ ░ ▒ ▓ ─ │ ┌ …`) that make
character-cell graphics and TUIs expressive. A program that wants those
glyphs checks `CAP.EXTGLYPHS` first; one that writes only ASCII runs on
either device.

**Soft font (optional).** When `CAP.SOFTFONT = 1` the device carries a
reloadable font alongside the built-in one, loaded through the `FONT`
aperture and selected globally by `CTRL.FONT_SEL`. The built-in font is
always present and cannot be overwritten, so `FONT_SEL = 0` is always a
working fallback — a demo uploads a custom glyph set, switches to it,
and reverts by clearing the bit with no reload.

#### FONT (0x8000)
Present when `CAP.SOFTFONT = 1`; otherwise the aperture reads 0. The
soft font is 256 glyphs × 16 rows; each row is one word-strided slot
holding 8 pixels in bits [7:0], **bit 7 = leftmost column** (the VGA
glyph-table convention, so a standard 8×16 font loads verbatim). The
row for glyph `g`, row `r` is the slot at `0x8000 + (g*16 + r) * 4`.
Like the cell aperture it is plain word storage — no byte enables, no
read-modify-write — so a consumer loads whole glyphs. Contents are
undefined at power-up, and the font drives the screen only while
`CTRL.FONT_SEL = 1`; staging it while `FONT_SEL = 0` leaves the picture
undisturbed.

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

The optional features map onto the standard wscons ioctls, so no
device-specific extension is needed: a custom palette is programmed
through `WSDISPLAYIO_PUTCMAP` / `WSDISPLAYIO_GETCMAP`, and a soft font
through `WSDISPLAYIO_LDFONT` plus `WSDISPLAYIO_SFONT`. `CTRL.PAL_SEL` and
`CTRL.FONT_SEL` are driver-internal: the driver sets them when a custom
colormap or font is installed and clears them to restore the built-ins,
which the retained-store semantics reduce to a single register write.

The `CELLS` aperture begins at a 4 KiB-aligned offset and device space is
uncached, so a driver can map it directly into a process under
`WSDISPLAYIO_MODE_MAPPED` — a full-screen program then writes cells
straight to the aperture, bypassing the terminal emulator, for
redraw-heavy output that the emulator's escape-sequence parsing would
otherwise bound. Emulation mode stays the path for ordinary console
text. The control registers and `PALETTE` share the first page, separate
from `CELLS`, so a mapped client gets cell access with no reach to the
control registers.
