#!/usr/bin/env python3
"""Print high-fanout nets from a yosys synth_ecp5 JSON output.

Useful for diagnosing nextpnr-ecp5 routing congestion. Nets with very
high fanout (hundreds to thousands of sinks) routed through general
fabric — rather than the chip's dedicated global routing — pay heavy
congestion cost. Common offenders: synchronous reset, wide CE/enable
broadcasts, hand-rolled clock-divider outputs.

Usage:
    list_fanout.py build/ulx3s_top.json
    list_fanout.py build/ulx3s_top.json -n 30 -m 10

Notes:
    * Only the design top module is analyzed by default (others are
      cell-library definitions that yosys emits for round-trip).
    * "Fanout" here means sink-port count: how many cell input ports
      drink from this net. Driver count (should be 1) is excluded.
    * For multi-bit busses, both per-bit and per-bus totals are shown
      so you can see whether an entire bus is wide-fanout or just one
      bit (e.g. bit 0 of a status flag often dominates).
"""
import json
import sys
import argparse
from collections import defaultdict


def _name_score(name):
    """Lower is better. Used to pick the most readable net name when
    yosys aliases multiple netnames to the same bit (common — e.g.
    `rst` and `ac_spi_sel_LUT4_Z_D_..._D_Z` may be the *same* bit
    because the LUT's input is rst). We want `rst`, not the LUT-named
    alias.

    Heuristic: $-prefixed names (synth temporaries) are worst, then
    names containing underscored synth artifacts (`_LUT4`, `_TRELLIS_`,
    `_CCU2C`, `_PFUMX`), then ordinary names — broken by length.
    """
    if not name:
        return (9, 9999, '')
    is_temp = 1 if name.startswith('$') or name.startswith('\\$') else 0
    has_synth_marker = 1 if any(s in name for s in
        ('_LUT4', '_TRELLIS_', '_CCU2C', '_PFUMX', '_DI_', '_LSR_')) else 0
    return (is_temp, has_synth_marker, len(name), name)


def analyze_module(mod_name, mod):
    # Map: bit index -> best wire name (yosys often aliases multiple
    # netnames to the same bit, especially user-RTL names alongside
    # synth-internal LUT-cascade names. Pick the most readable.)
    bit_to_net = {}
    net_width = {}
    for wname, w in mod.get('netnames', {}).items():
        bits = w.get('bits', [])
        net_width[wname] = sum(1 for b in bits if isinstance(b, int))
        for b in bits:
            if isinstance(b, int):
                prev = bit_to_net.get(b)
                if prev is None or _name_score(wname) < _name_score(prev):
                    bit_to_net[b] = wname

    # Count sinks per bit by walking cells and using port_directions
    bit_sinks = defaultdict(int)
    for cname, c in mod.get('cells', {}).items():
        pdir = c.get('port_directions', {})
        for port, conns in c.get('connections', {}).items():
            if pdir.get(port) == 'input':
                for b in conns:
                    if isinstance(b, int):
                        bit_sinks[b] += 1

    # Aggregate sink counts to net level
    net_sinks = defaultdict(int)
    for b, s in bit_sinks.items():
        n = bit_to_net.get(b)
        if n is not None:
            net_sinks[n] += s

    # Also produce per-bit max for each net (so we see if it's
    # uniform fanout or one-bit-dominates)
    net_max_bit = defaultdict(int)
    for b, s in bit_sinks.items():
        n = bit_to_net.get(b)
        if n is not None:
            net_max_bit[n] = max(net_max_bit[n], s)

    return net_sinks, net_max_bit, net_width


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('json', help='yosys JSON (e.g. build/ulx3s_top.json)')
    ap.add_argument('-n', '--top', type=int, default=25,
                    help='show top N nets (default 25)')
    ap.add_argument('-m', '--min-fanout', type=int, default=50,
                    help='only show nets with fanout >= this (default 50)')
    ap.add_argument('-M', '--module', default=None,
                    help='module to analyze (default: top, taken from '
                         'design last-module which is yosys convention)')
    ap.add_argument('--all-modules', action='store_true',
                    help='analyze every user module, not just top')
    args = ap.parse_args()

    d = json.load(open(args.json))

    # Yosys puts cell-library "blackbox" modules first and the user's
    # top last. Filter to user modules: those without 'blackbox'/'cell_library'
    # attributes, and skip the $abc/$paramod/$__-prefix synth helpers.
    mods = []
    for name, mod in d['modules'].items():
        if name.startswith('$') or name.startswith('\\$'):
            continue
        attrs = mod.get('attributes', {})
        if 'blackbox' in attrs or 'cell_library' in attrs:
            continue
        mods.append((name, mod))

    if args.module:
        mods = [(n, m) for (n, m) in mods if n == args.module]
        if not mods:
            print(f'No module named {args.module!r}', file=sys.stderr)
            sys.exit(1)
    elif not args.all_modules:
        # Convention: yosys emits user top last in the modules list
        mods = mods[-1:]

    for mod_name, mod in mods:
        net_sinks, net_max_bit, net_width = analyze_module(mod_name, mod)
        ranked = sorted(net_sinks.items(), key=lambda x: -x[1])

        print(f'=== {mod_name} (top {args.top}, min fanout {args.min_fanout}) ===')
        print(f'    {len(mod.get("cells", {}))} cells, {len(mod.get("netnames", {}))} named nets')
        print()
        print(f'  {"total":>7s}  {"width":>5s}  {"max_bit":>7s}  net')
        print(f'  {"-"*7}  {"-"*5}  {"-"*7}  {"-"*40}')
        shown = 0
        for n, s in ranked:
            if s < args.min_fanout:
                break
            if shown >= args.top:
                break
            w = net_width.get(n, 1)
            mx = net_max_bit.get(n, s)
            print(f'  {s:>7d}  {w:>5d}  {mx:>7d}  {n}')
            shown += 1
        if shown == 0:
            print(f'  (no nets with fanout >= {args.min_fanout})')
        print()


if __name__ == '__main__':
    main()
