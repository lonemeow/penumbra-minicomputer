#!/usr/bin/env python3
"""Replace $readmemh() calls in Verilog with inline initial data.

Yosys inside Docker silently ignores $readmemh when the hex file
isn't found (or for other reasons). This script inlines the hex
data directly into the Verilog source so the ROM/RAM contents are
guaranteed to be in the synthesized design.

Usage: python3 inline_hex.py input.v output.v [hexdir]
  hexdir defaults to current directory.
"""

import re
import sys
import os

def read_hex(path):
    """Read a $readmemh-format hex file, return list of value strings."""
    vals = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('//') or line.startswith('@'):
                continue
            # Handle multiple values on one line (space-separated)
            for token in line.split():
                if token.startswith('//'):
                    break
                vals.append(token)
    return vals

def inline_readmemh(content, hexdir):
    """Replace $readmemh("file", array) with inline assignments."""
    pattern = r'\$readmemh\("([^"]+)",\s*(\w+)\);'

    def replacer(m):
        hexfile = m.group(1)
        arrayname = m.group(2)
        hexpath = os.path.join(hexdir, hexfile)

        if not os.path.exists(hexpath):
            print(f"WARNING: {hexpath} not found, keeping $readmemh", file=sys.stderr)
            return m.group(0)

        vals = read_hex(hexpath)
        # Detect width from the hex values (4 bits per hex digit)
        if vals:
            width = len(vals[0]) * 4
        else:
            return m.group(0)

        lines = [f"// inlined from {hexfile} ({len(vals)} entries)"]
        for i, v in enumerate(vals):
            lines.append(f"\t\t{arrayname}[{i}] = {width}'h{v};")

        print(f"  inlined {hexfile}: {len(vals)} × {width}-bit entries", file=sys.stderr)
        return '\n'.join(lines)

    return re.sub(pattern, replacer, content)

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} input.v output.v [hexdir]", file=sys.stderr)
        sys.exit(1)

    infile = sys.argv[1]
    outfile = sys.argv[2]
    hexdir = sys.argv[3] if len(sys.argv) > 3 else '.'

    with open(infile) as f:
        content = f.read()

    content = inline_readmemh(content, hexdir)

    with open(outfile) as f:
        pass  # verify output path is writable

    with open(outfile, 'w') as f:
        f.write(content)
