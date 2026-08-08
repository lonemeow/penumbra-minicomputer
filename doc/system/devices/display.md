# Display Adapter — Programmer's Reference

A display adapter for the Penumbra bus (`CLASS_DISPLAY`). The device
holds a grid of character cells plus a built-in font; writing a cell
makes the device draw that glyph. It is the local-console analog of
the [UART](uart.md) — generic firmware or the OS can use any conformant
device as a console with no device-specific driver.

This document defines the **`CLASS_DISPLAY` minimum protocol**: the
registers every device of the class implements, and the mode it powers up
in, after reset. The mandatory core is deliberately small — a cell grid
that renders the printable 7-bit ASCII glyphs (see
[Character Set](#character-set)). Everything else — color, a
programmable palette, the extended glyph range, a reloadable soft font,
additional display modes, hardware scroll, a pixel
[framebuffer](#framebuffer) — is an optional feature advertised through
a capability flag or `CFG_ID`. A device may add any of them, but must
always present this interface — and power up in
[mode 0](#display-modes), showing the character cells — after reset.
The character console is the class's identity: a device that renders
pixels but no cells is not a `CLASS_DISPLAY` device.

## Register Interface

A small register block followed by a character-cell aperture. Base
address assigned by autoconfig.

**Access.** All registers and cells are accessed with 32-bit aligned
loads and stores only (`LDW`/`STW`); the device implements no byte-lane
enables. Every register and cell is *word-strided* — it occupies a
32-bit slot even where its value is narrower — the same convention the
[UART](uart.md) and [SPI](spi.md) controllers use; the framebuffer
pixel aperture is the one exception, packed and byte-accessible (see
[`FB`](#fb-0x20000)). See the bus protocol's
[Access Width](../../hardware/bus-protocol.md#access-width).

| Offset    | Name         | R/W | Description                              |
|-----------|--------------|-----|------------------------------------------|
| `0x000`   | `CAP`        | R   | Version, capabilities, mode count        |
| `0x004`   | `INFO`       | R   | Active-mode geometry: columns and rows   |
| `0x008`   | `CTRL`       | R/W | Output/cursor enable, source/palette/font select|
| `0x00C`   | `CURSOR`     | R/W | Cursor cell position                     |
| `0x010`   | `MODE_SEL`   | R/W | Active display mode (optional)           |
| `0x014`   | `MODE_QUERY` | W   | Mode index to query (optional)           |
| `0x018`   | `MODE_GEOM`  | R   | Geometry of the queried mode (optional)  |
| `0x01C`   | `FB_GEOM`    | R   | Framebuffer geometry (optional)          |
| `0x020`   | `FB_FORMAT`  | R   | Framebuffer pixel format (optional)      |
| `0x024`   | `EDID_CTRL`  | —   | Reserved for display identification (see [EDID](#edid-reserved)) |
| `0x040…`  | `PALETTE`    | R/W | Custom-palette aperture, 16 slots (optional) |
| `0x400…`  | `EDID`       | —   | Reserved for the display-identification block (see [EDID](#edid-reserved)) |
| `0x1000…` | `CELLS`      | R/W | Character-cell aperture (one slot / cell)|
| `0x8000…` | `FONT`       | R/W | Soft-font aperture (optional)            |
| `0x10000…`| `FB_PALETTE` | R/W | Framebuffer palette aperture, 256 slots (optional) |
| `0x20000…`| `FB`         | R/W | Framebuffer pixel aperture, packed (optional) |

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
- Bit [13]: `FRAMEBUFFER` — device has a pixel
  [framebuffer](#framebuffer) alongside the character console, scanned
  out from the [`FB`](#fb-0x20000) aperture while `CTRL.FB_SEL` is set;
  geometry in [`FB_GEOM`](#fb_geom-0x01c), format in
  [`FB_FORMAT`](#fb_format-0x020)
- Bit [14]: reserved for `EDID` — a future revision defines querying
  the attached display for its identification data (see
  [EDID](#edid-reserved))
- Bits [23:16]: `MODE_COUNT` — number of display modes (≥ 1; mode 0 is
  the mandated default)

Remaining bits reserved (read 0); further capabilities are advertised
through `CFG_ID`.

#### INFO (0x004)
- Bits [15:0]: `COLUMNS`
- Bits [31:16]: `ROWS`

Reports the geometry of the **active** mode. Geometry is discoverable so
one console back-end drives any display device — and any mode —
without hardcoding a grid size.

#### CTRL (0x008)
- Bit [0]: `ENABLE` — picture output active. Resets to `1`. While
  clear the device drives black active video with sync timing still
  running, so the monitor stays locked and re-enabling is instant.
- Bit [1]: `CURSOR_EN` — show the cursor at `CURSOR`. Resets to `0`.
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
- Bit [4]: `FB_SEL` — scan-out source: `0` = the character cells, `1` =
  the [framebuffer](#framebuffer). Present only when
  `CAP.FRAMEBUFFER = 1`; reads `0` and is ignored otherwise. Resets to
  `0`, so every device powers up showing the character console. Whether
  a deselected source's contents survive is implementation-defined —
  see [Framebuffer](#framebuffer).

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

#### FB_GEOM (0x01C)
- Bits [15:0]: `WIDTH` — framebuffer width in pixels
- Bits [31:16]: `HEIGHT` — framebuffer height in lines

Present when `CAP.FRAMEBUFFER = 1`; reads 0 otherwise. The geometry is
a property of the device, discovered here — nothing in the class pins
any particular size. How the device presents it on the display —
scaling, borders, raster timing — is not visible through this
interface, exactly as a text mode's underlying pixel resolution is not.

#### FB_FORMAT (0x020)
- Bits [7:0]: `BPP` — bits per pixel

Present when `CAP.FRAMEBUFFER = 1`; reads 0 otherwise. Declares the
pixel format of the [`FB`](#fb-0x20000) aperture. This revision of the
class defines only `BPP = 8`, indexed through
[`FB_PALETTE`](#fb_palette-0x10000); every other value — and the
remaining bits, which read 0 — is reserved for future formats (deeper
indexed, direct color). A consumer that reads a format it does not
recognize leaves the framebuffer alone; the character console is
unaffected either way.

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
[`FONT`](#font-0x8000) aperture, and on a device with the framebuffer
to the end of the [`FB`](#fb-0x20000) aperture.

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

## Framebuffer

When `CAP.FRAMEBUFFER = 1` the device carries a pixel framebuffer
alongside the character console: a `WIDTH × HEIGHT` pixel array in the
format [`FB_FORMAT`](#fb_format-0x020) declares. This revision defines
one format — 8-bit indexed, each pixel an index into the 256-entry
framebuffer palette — with the rest of the format space reserved for
richer devices; geometry and format are always read from the device,
never assumed. The
framebuffer is a second scan-out **source**, not a display mode:
`CTRL.FB_SEL` selects which source drives the output, and the
registers of both stay live throughout.

How the two sources are **backed** is implementation-defined. A device
may give each its own storage — the text screen then survives a
framebuffer session untouched — or share one RAM between them, the
classic VGA arrangement, where each source's contents are undefined
while it is deselected. A portable consumer therefore selects the
framebuffer *before* drawing into it, and redraws the text screen
after a framebuffer session (cheap at `COLUMNS × ROWS` cells); a
driver matched to a specific device through `CFG_ID` may know its
storage is independent and skip both precautions. The cursor, the
fonts, and both palettes are dedicated state, never aliased with cell
or pixel storage.

**Timing across the switch.** Writing `FB_SEL` may re-time the video
output (the framebuffer's presentation raster need not match the active
text mode's), so the display may resync — the same visible effect a
`MODE_SEL` switch is allowed. `INFO` and `MODE_SEL` describe the
character console throughout; `FB_GEOM` describes the framebuffer.

#### FB (0x20000)
The pixel aperture, present when `CAP.FRAMEBUFFER = 1`; reads 0
otherwise. Pixels are **packed** with no row padding — in the 8-bit
indexed format, four per word: the pixel at (`x`, `y`) is byte
`y * WIDTH + x` of the aperture, and bytes map little-endian within
each word (byte `a` occupies bits `[8*(a mod 4) + 7 : 8*(a mod 4)]` of
the word at `a & ~3`).

Unlike the register apertures, `FB` follows the bus's **default**
access contract (see
[Access Width](../../hardware/bus-protocol.md#access-width)): reads
return the stored bytes and writes commit exactly the enabled byte
lanes, at every width the CPU can issue. Both departures from the
word-strided convention exist for the same reason — the aperture is
mapped directly into rendering processes, whose compiler-generated
stores arrive at every width, and packing makes a row `WIDTH`
contiguous bytes and a full frame one linear copy. In short: the
aperture behaves as plain memory. Contents are undefined at power-up —
and, on a shared-storage device, undefined whenever the framebuffer is
deselected (see [Framebuffer](#framebuffer)).

#### FB_PALETTE (0x10000)
Present when `CAP.FRAMEBUFFER = 1`; otherwise the slots read 0. 256
word-strided slots, one `0x00RRGGBB` color per slot — the same slot
format as [`PALETTE`](#palette-0x040) — where `FB_PALETTE[i]` is the
color rendered for pixel value `i`. Backing storage only: writes take
effect on the next scan-out read, whether the framebuffer is deselected
(staging) or live (palette animation is a legitimate technique).
Contents are undefined at power-up; load all 256 slots before first
setting `CTRL.FB_SEL`.

## EDID (reserved)

Monitors describe themselves — supported timings, physical size,
identity — as an EDID/DisplayID block readable over the connector's
DDC pins. This class reserves the space to surface that through the
device, so adding it later collides with nothing: **CAP bit [14]**,
the **`EDID_CTRL`** slot at `0x024`, and the **`EDID`** aperture at
`0x400`–`0x7FF` (128 word-strided slots — one 256-byte E-EDID block at
the device's usual one-byte-per-slot stride). The access mechanism — a
block the device snapshots itself versus a raw DDC master the driver
drives — is fixed by the first revision that implements it; until
then the bit and the slots read 0 like every unimplemented option.

No new userland interface rides on this: the kernel driver surfaces
the block through the standard wsdisplay path
(`WSDISPLAYIO_GET_EDID`), and the in-tree `dev/videomode` EDID parser
consumes it from there.

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

The framebuffer maps onto wsdisplay's dumb-framebuffer path — the
standard interface X's `wsfb` driver and SDL's wscons backend already
speak, so a game or demo needs no awareness of this device:
`WSDISPLAYIO_MODE_DUMBFB` sets `CTRL.FB_SEL` and exposes the packed
[`FB`](#fb-0x20000) aperture through `WSDISPLAYIO_GINFO` (geometry from
`FB_GEOM`, depth from `FB_FORMAT`, `linebytes` derived from both) and
`mmap`; `WSDISPLAYIO_PUTCMAP`
programs `FB_PALETTE`. The mapping is the device pages themselves —
the kernel keeps no system-RAM shadow of pixel contents, which is why
the aperture must behave as plain memory — and a client returning to
the screen after a wscons screen switch repaints its frame from its
own state (standard `wsfb` behavior), so graphics contents never need
to survive deselection. Reverting to `WSDISPLAYIO_MODE_EMUL` — which the
tty close path does even when the client crashes — clears `FB_SEL` and
redraws the console from the kernel's screen state, the ordinary
wsdisplay flow; a driver that knows through `CFG_ID` that the device
backs its sources independently may skip the redraw, since the cell
RAM was never disturbed. Like `CELLS`, the `FB` aperture begins on its
own page, so a mapped client gets pixel access with no reach to the
control registers.
