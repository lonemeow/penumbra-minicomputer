#!/usr/bin/env python3
"""text2cells — plain-text screen image to display cell-RAM hex.

Reads a text file describing an 80x30 character screen and emits the
$readmemh image for video_cell_ram: 4096 lines of one 4-hex-digit
{attr, glyph} cell each, glyph-in-the-low-byte, row-major. Input
lines shorter than the grid pad with spaces; the attribute is uniform
(--attr, default 0x07 = light grey on black — the contract's default).
The file is UTF-8, the encoding every editor writes; each character
is transcoded to its CP437 glyph index, so box drawing, shades, and
blocks all map to the font the hardware carries. A character with no
CP437 equivalent is an error naming the offending glyph.

Usage: text2cells.py [--attr HH] <screen.txt> <output.hex>
"""

import argparse
import sys

COLS, ROWS, DEPTH = 80, 30, 4096


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--attr", default="07",
                    help="attribute byte for every cell (hex, default 07)")
    ap.add_argument("screen")
    ap.add_argument("output")
    args = ap.parse_args()

    attr = int(args.attr, 16)
    if not 0 <= attr <= 0xFF:
        sys.exit(f"error: attribute {args.attr} is not a byte")

    with open(args.screen, encoding="utf-8") as f:
        lines = f.read().split("\n")
    while lines and lines[-1] == "":
        lines.pop()
    lines = [l.rstrip("\r") for l in lines]

    if len(lines) > ROWS:
        sys.exit(f"error: {args.screen}: {len(lines)} lines exceed the "
                 f"{ROWS}-row grid")

    rows = []
    for n, l in enumerate(lines):
        try:
            raw = l.encode("cp437")
        except UnicodeEncodeError as e:
            sys.exit(f"error: {args.screen}:{n + 1}: {e.object[e.start]!r} "
                     "has no CP437 glyph")
        if len(raw) > COLS:
            sys.exit(f"error: {args.screen}:{n + 1}: {len(raw)} columns "
                     f"exceed the {COLS}-column grid")
        rows.append(raw)

    blank = (attr << 8) | 0x20
    cells = [blank] * DEPTH
    for r, raw in enumerate(rows):
        for c, glyph in enumerate(raw):
            cells[r * COLS + c] = (attr << 8) | glyph

    with open(args.output, "w") as f:
        f.write(f"// text2cells: {args.screen}, attr {attr:02x}\n")
        for cell in cells:
            f.write(f"{cell:04x}\n")


if __name__ == "__main__":
    main()
