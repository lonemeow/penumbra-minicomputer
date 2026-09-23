#!/usr/bin/env python3
"""Check a board top's XDC against the board's vendored master XDC.

The master (hw/third_party/digilent-xdc/) is board truth: pin
locations, I/O standards and the oscillator clock, every line
commented out, plus a few uncommented board-wide settings. A top's XDC
copies the lines for the pins it uses, uncommented and otherwise
unchanged, keeping the master's port names. This fails when a top's
XDC states a board fact the master does not state, or leaves out a
board-wide setting:

- a line setting PACKAGE_PIN or IOSTANDARD, or a create_clock on a
  top-level port, that is not one of the master's lines;
- a missing master line that is active in the master itself (such as
  CONFIG_VOLTAGE), which every design on the board needs.

Lines compare with whitespace collapsed and any trailing `; #...`
schematic comment dropped. Everything else in a top's XDC (generated
clocks, timing exceptions, CDC constraints) is design intent and
passes unchecked.

Usage: python3 xdc_check.py master.xdc top.xdc [top.xdc ...]
"""

import re
import sys

BOARD_FACT = re.compile(r'\b(PACKAGE_PIN|IOSTANDARD)\b|^create_clock\b.*\bget_ports\b')
TRAILING_COMMENT = re.compile(r';\s*#.*$')


def normalize(line):
    """The constraint itself: no schematic comment, whitespace collapsed."""
    line = TRAILING_COMMENT.sub('', line.strip())
    return ' '.join(line.rstrip(';').split())


def load_master(path):
    """Return (board facts, board-wide lines) from a master XDC."""
    facts = set()
    board_wide = []
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if line.startswith('#set_property') or line.startswith('#create_clock'):
                facts.add(normalize(line[1:]))
            elif line and not line.startswith('#'):
                board_wide.append(normalize(line))
    return facts, board_wide


def check(master_path, top_path, facts, board_wide):
    errors = []
    present = set()
    with open(top_path) as f:
        for number, raw in enumerate(f, 1):
            line = normalize(raw)
            if not line or line.startswith('#'):
                continue
            present.add(line)
            if BOARD_FACT.search(line) and line not in facts:
                errors.append(f'{top_path}:{number}: not a line of {master_path}: {line}')
    for line in board_wide:
        if line not in present:
            errors.append(f'{top_path}: missing board-wide line from {master_path}: {line}')
    return errors


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    if len(argv) == 2:
        print(f'no top XDC to check against {argv[1]}: an xc7 top needs '
              f'its own constraints (XDC_<top> in the Makefile)', file=sys.stderr)
        return 1
    master_path, top_paths = argv[1], argv[2:]
    facts, board_wide = load_master(master_path)
    errors = []
    for top_path in top_paths:
        errors += check(master_path, top_path, facts, board_wide)
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
