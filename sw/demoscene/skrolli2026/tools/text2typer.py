#!/usr/bin/env python3
"""Compile a part's typed text into the stream runtime/typer.c plays.

The text file is UTF-8, laid out as it appears on screen, with markup:

  {yellow}         foreground, by the RT_COL_* name in lower case
  {on blue}        background
  {/}              back to grey on black
  {pause 4}        hold for 4 rows of the song
  {speed 1.5}      from here on, 1.5 characters per row
  {clear}          blank the window in the current colours, and start
                   again from its corner
  {{ }}            a literal '{' or '}'

Every character must exist in CP437 at 0x20 or above, so box drawing
and shades can be written as themselves (for example U+2592 for 0xB1).
One trailing newline is dropped.  Anything else is an error naming the
line and column, so a typo stops the build rather than the capture.

The output header holds the stream as <name>_text[] and its extent as
<NAME>_TEXT_COLS and <NAME>_TEXT_ROWS, for the part to check against
the window it types into: the cells that receive a character, so a
line as wide as the window fits it, and {clear} starts a page.

Stream: glyph bytes 0x20-0xFF, and
  0x00 end    0x01 n fg    0x02 n bg    0x03 default colours
  0x04 n pause n rows      0x05 s0 s1 s2 s3 speed, rows per character
                           as 16.16, little-endian
  0x06 clear  0x0A newline
"""

import argparse
import os
import re
import sys

COLOURS = [
    "black", "blue", "green", "cyan", "red", "magenta", "brown", "grey",
    "darkgrey", "ltblue", "ltgreen", "ltcyan", "ltred", "ltmagenta",
    "yellow", "white",
]

OP_END, OP_FG, OP_BG, OP_RESET, OP_PAUSE, OP_SPEED, OP_CLEAR = range(7)
OP_NEWLINE = 0x0A


class TextError(Exception):
    pass


def compile_text(src):
    out = bytearray()
    line, col = 1, 1            # source position, for messages
    x, y = 0, 0                 # where the next glyph lands on the page
    cols = rows = 0             # cells typed into, over all pages
    i = 0

    def fail(msg, at_col=None):
        raise TextError(f"{line}:{at_col or col}: {msg}")

    if src.endswith("\n"):
        src = src[:-1]

    while i < len(src):
        ch = src[i]

        if src.startswith("{{", i) or src.startswith("}}", i):
            out.append(ord(ch))
            x += 1
            cols, rows = max(cols, x), max(rows, y + 1)
            i += 2
            col += 2
        elif ch == "{":
            end = src.find("}", i)
            if end < 0 or "\n" in src[i:end]:
                fail("'{' starts a tag that is never closed")
            tag = src[i + 1:end].strip()
            words = tag.split()
            if tag == "/":
                out.append(OP_RESET)
            elif tag == "clear":
                out.append(OP_CLEAR)
                x, y = 0, 0
            elif tag in COLOURS:
                out += bytes([OP_FG, COLOURS.index(tag)])
            elif len(words) == 2 and words[0] == "on" and words[1] in COLOURS:
                out += bytes([OP_BG, COLOURS.index(words[1])])
            elif len(words) == 2 and words[0] == "pause":
                if not re.fullmatch(r"[0-9]+", words[1]) or not 1 <= int(words[1]) <= 255:
                    fail(f"pause takes whole rows, 1 to 255, not '{words[1]}'")
                out += bytes([OP_PAUSE, int(words[1])])
            elif len(words) == 2 and words[0] == "speed":
                try:
                    cps = float(words[1])
                except ValueError:
                    cps = 0.0
                step = round(65536 / cps) if cps > 0 else 0
                if not 1 <= step < 1 << 31:
                    fail(f"speed takes characters per row, above 0, not '{words[1]}'")
                out += bytes([OP_SPEED]) + step.to_bytes(4, "little")
            else:
                fail(f"unknown tag {{{tag}}}")
            col += end + 1 - i
            i = end + 1
        elif ch == "\n":
            out.append(OP_NEWLINE)
            x = 0
            y += 1
            line += 1
            col = 1
            i += 1
        else:
            try:
                b = ch.encode("cp437")[0]
            except UnicodeEncodeError:
                fail(f"U+{ord(ch):04X} is not in CP437")
            if b < 0x20:
                fail(f"U+{ord(ch):04X} encodes below 0x20, where the stream's controls are")
            out.append(b)
            x += 1
            cols, rows = max(cols, x), max(rows, y + 1)
            i += 1
            col += 1

    out.append(OP_END)
    return out, cols, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("text")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    name = os.path.splitext(os.path.basename(args.text))[0]
    if not re.fullmatch(r"[a-z_][a-z0-9_]*", name):
        sys.exit(f"{args.text}: the name must be a C identifier in lower case")

    try:
        with open(args.text, encoding="utf-8") as f:
            stream, cols, rows = compile_text(f.read())
    except UnicodeDecodeError as e:
        sys.exit(f"{args.text}: not UTF-8 ({e.reason} at byte {e.start})")
    except TextError as e:
        sys.exit(f"{args.text}:{e}")

    upper = name.upper()
    body = ",\n".join(
        "    " + ", ".join(f"0x{b:02X}" for b in stream[i:i + 12])
        for i in range(0, len(stream), 12))
    with open(args.output, "w") as f:
        f.write(f"/* Generated by tools/text2typer.py from {args.text}; do not edit. */\n\n"
                f"#define {upper}_TEXT_COLS  {cols}\n"
                f"#define {upper}_TEXT_ROWS  {rows}\n\n"
                f"static const unsigned char {name}_text[] = {{\n{body}\n}};\n")


if __name__ == "__main__":
    main()
