#!/usr/bin/env python3
"""wsfont2hex — NetBSD wsfont header to $readmemh font-ROM hex.

Reads a kernel wsfont bitmap header (netbsd/sys/dev/wsfont/*.h) and
emits the video font ROM image: 256 glyphs x <height> rows, one
two-digit hex byte per line, glyph-major (address = glyph * height +
row). Glyphs outside the font's [firstchar, firstchar + numchars)
range are zero-filled, so a partial font (bold8x16 covers 1..254)
still yields a full 256-entry table with blank cells where CP437 has
blanks anyway.

The ROM contract this feeds (see video_font_rom.sv) wants bit 7 as
the leftmost pixel and one byte per row; a font whose metadata cannot
satisfy that contract by straight byte copy (width != 8, stride != 1,
or a bit order other than left-to-right MSB-first) is a hard error,
not a repack — every supported font already matches, and a loud
failure names the assumption a new font would break.

Usage: wsfont2hex.py <wsfont-header.h> <output.hex>
"""

import re
import sys


def parse_field(text, name):
    """Extract one struct field value, by its /* comment */ tag or a
    .designated initializer — the two styles the wsfont headers use."""
    m = re.search(r"([A-Za-z0-9_'\\ ]+?)\s*,\s*/\*\s*" + name, text)
    if m is None:
        m = re.search(r"\." + name.split()[0] + r"\s*=\s*([^,]+),", text)
    if m is None:
        sys.exit(f"error: struct field '{name}' not found")
    return m.group(1).strip()


def parse_value(tok):
    """A wsfont field value: an int literal or a C char literal."""
    if tok.startswith("'"):
        body = tok[1:-1]
        if body.startswith("\\"):
            return int(body[1:] or "0", 8)  # '\0' and friends
        return ord(body)
    return int(tok, 0)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().split("\n")[-1])
    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path) as f:
        text = f.read()

    firstchar = parse_value(parse_field(text, "firstchar"))
    numchars = parse_value(parse_field(text, "numchars"))
    width = parse_value(parse_field(text, "width"))
    height = parse_value(parse_field(text, "height"))
    stride = parse_value(parse_field(text, "stride"))
    bitorder = parse_field(text, "bit order")

    if width != 8 or stride != 1:
        sys.exit(f"error: {src_path}: width {width} / stride {stride} — "
                 "the font ROM contract is 8-wide, one byte per row")
    if "L2R" not in bitorder:
        sys.exit(f"error: {src_path}: bit order {bitorder} — the font ROM "
                 "contract is MSB = leftmost pixel (FONTORDER_L2R)")
    if firstchar < 0 or firstchar + numchars > 256:
        sys.exit(f"error: {src_path}: glyph range {firstchar}.."
                 f"{firstchar + numchars - 1} exceeds the 256-entry table")

    # Data bytes: everything from the array *definition* (not the
    # forward declaration) to its closing brace. Comments go first —
    # the glyph-separator markers (/* 0x41 */) would otherwise read as
    # data bytes.
    m = re.search(r"_data\[\]\s*=\s*\{(.*?)\};", text, re.DOTALL)
    if m is None:
        sys.exit(f"error: {src_path}: no _data[] initializer found")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.DOTALL)
    data = [int(t, 16) for t in re.findall(r"0x[0-9a-fA-F]{1,2}", body)]

    expect = numchars * height
    if len(data) != expect:
        sys.exit(f"error: {src_path}: {len(data)} data bytes, expected "
                 f"{numchars} glyphs x {height} rows = {expect}")

    rom = [0] * (256 * height)
    rom[firstchar * height:(firstchar + numchars) * height] = data

    with open(out_path, "w") as f:
        f.write(f"// wsfont2hex: {src_path} ({firstchar}..", )
        f.write(f"{firstchar + numchars - 1}), {height} rows/glyph\n")
        for byte in rom:
            f.write(f"{byte:02x}\n")


if __name__ == "__main__":
    main()
