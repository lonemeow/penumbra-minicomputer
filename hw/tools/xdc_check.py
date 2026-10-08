#!/usr/bin/env python3
"""Check a board top's XDC against the board's vendored board facts.

The masters are board truth, as the manufacturer states it:

- a master XDC (hw/third_party/digilent-xdc/): pin locations, I/O
  standards and the oscillator clock, every line commented out, plus
  a few uncommented board-wide settings;
- a memory interface generator project (a `.prj`,
  hw/third_party/digilent-boards/): the pins and I/O standards of the
  board's DRAM, which the master XDC leaves out. Each pin stands for
  the line `set_property -dict { PACKAGE_PIN <pad> IOSTANDARD <std> }
  [get_ports { <name> }]`.

A top's constraints (one XDC, or several, such as a file its board's
DRAM tops share) copy the lines for the pins it uses, uncommented and
otherwise unchanged, keeping the masters' port names. This fails when
one of a top's files states a board fact no master states, or the files
together leave out a board-wide setting:

- a line setting PACKAGE_PIN or IOSTANDARD that is not one of the
  masters' lines;
- a create_clock on a port the masters give a clock, an oscillator's,
  that is not the masters' line for it;
- a create_clock whose ports are not written out by name (a pattern or
  a variable), since nothing here can tell which ports it clocks;
- a missing master line that is active in the master itself (such as
  CONFIG_VOLTAGE), which every design on the board needs.

A clock on any other port is one a part on the board sends, such as an
Ethernet PHY's receive clock. The masters say nothing of it, so its
period is the top's to state, from the part's datasheet.

Lines compare with whitespace collapsed and any trailing `; #...`
schematic comment dropped. Everything else in a top's XDC (generated
clocks, timing exceptions, CDC constraints, termination and slew) is
design intent and passes unchecked.

Usage: python3 xdc_check.py --master FILE [--master FILE ...] top.xdc [more.xdc ...]
"""

import argparse
import re
import sys
import xml.etree.ElementTree as ET

PIN_FACT = re.compile(r'\b(PACKAGE_PIN|IOSTANDARD)\b')
# What a create_clock hands get_ports: a braced list, or one bare word.
CLOCK_PORTS = re.compile(r'^create_clock\b.*\bget_ports\s+(?:\{([^}]*)\}|([^\s\]]+))')
PORT_NAME = re.compile(r'^\w+(\[\d+\])?$')
TRAILING_COMMENT = re.compile(r';\s*#.*$')


def normalize(line):
    """The constraint itself: no schematic comment, whitespace collapsed."""
    line = TRAILING_COMMENT.sub('', line.strip())
    return ' '.join(line.rstrip(';').split())


def clock_ports(line):
    """What a create_clock line names as its ports, each as written;
    empty for any other line, and for a clock on no port."""
    match = CLOCK_PORTS.search(line)
    if not match:
        return []
    return (match.group(1) if match.group(1) is not None else match.group(2)).split()


def load_master_xdc(path):
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


def load_master_prj(path):
    """Return (board facts, board-wide lines) from a MIG project: the
    pin lines its pins stand for, and no board-wide lines."""
    facts = set()
    for pin in ET.parse(path).iter('Pin'):
        pad = pin.get('PADName')
        std = pin.get('IOSTANDARD')
        name = pin.get('name')
        if pad and std and name:
            facts.add(normalize(f'set_property -dict {{ PACKAGE_PIN {pad} '
                                f'IOSTANDARD {std} }} [get_ports {{ {name} }}]'))
    return facts, []


def load_master(path):
    return load_master_prj(path) if path.endswith('.prj') else load_master_xdc(path)


def check(masters, top_paths, facts, board_wide):
    """Check one top's constraint files, which together must carry every
    board-wide line."""
    errors = []
    present = set()
    # The ports the board itself clocks: the masters' clock lines name them.
    board_clocked = {port for fact in facts for port in clock_ports(fact)}
    for top_path in top_paths:
        with open(top_path) as f:
            for number, raw in enumerate(f, 1):
                line = normalize(raw)
                if not line or line.startswith('#'):
                    continue
                present.add(line)
                ports = clock_ports(line)
                unnamed = [port for port in ports if not PORT_NAME.match(port)]
                if unnamed:
                    errors.append(f'{top_path}:{number}: a clock on ports not written out by '
                                  f'name ({" ".join(unnamed)}): {line}')
                elif ((PIN_FACT.search(line) or board_clocked.intersection(ports))
                        and line not in facts):
                    errors.append(f'{top_path}:{number}: not a board fact of {masters}: {line}')
    tops = ', '.join(top_paths)
    for line in board_wide:
        if line not in present:
            errors.append(f'{tops}: missing board-wide line from {masters}: {line}')
    return errors


def main(argv):
    parser = argparse.ArgumentParser(usage=__doc__.strip().splitlines()[-1].split(': ', 1)[1])
    parser.add_argument('--master', action='append', required=True)
    parser.add_argument('tops', nargs='*')
    args = parser.parse_args(argv[1:])
    masters = ', '.join(args.master)
    if not args.tops:
        print(f'no top XDC to check against {masters}: an xc7 top needs '
              f'its own constraints (XDC_<top> in the Makefile)', file=sys.stderr)
        return 1
    facts, board_wide = set(), []
    for path in args.master:
        more_facts, more_board_wide = load_master(path)
        facts |= more_facts
        board_wide += more_board_wide
    errors = check(masters, args.tops, facts, board_wide)
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
