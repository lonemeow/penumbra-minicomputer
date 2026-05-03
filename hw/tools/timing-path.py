#!/usr/bin/env python3
# Print one critical path from a nextpnr-ecp5 timing report in detail.
#
# Usage: timing-path.py <report.json> <path_index> [--no-rollup|--no-hops]
# path_index is 1-based, matching the indices `make timing` displays.

import argparse
import json
import re
import sys


# Module = everything before the last dot in the hierarchical cell name.
# Yosys-generated synthesis cells (LUT4, PFUMX, TRELLIS_FF_Q, ...) live
# inside the last user-written instance, so the dot before them gives the
# module the user actually authored.
def cell_module(cell):
    if not cell or '.' not in cell:
        return '(top)'
    return cell.rsplit('.', 1)[0]


# Strip Yosys synthesis suffixes so the leaf signal name is recognizable.
# Examples:
#   upc_TRELLIS_FF_Q_7                       → upc
#   o_uword_LUT4_Z_30_B_LUT4_C_Z_LUT4_Z_1    → o_uword
#   o_hit_LUT4_Z_C_LUT4_Z_D_LUT4_Z_1_D_...   → o_hit
def cell_signal(cell):
    leaf = cell.rsplit('.', 1)[-1] if cell else ''
    leaf = re.sub(r'_TRELLIS_FF_Q.*$', '', leaf)
    leaf = re.sub(r'_(LUT4|PFUMX|BLUT|ALUT)_.*$', '', leaf)
    return leaf


def fmt_path_endpoints(p):
    src = p['path'][0].get('from', {})
    dst = p['path'][-1].get('to', {})
    return (
        f"{src.get('cell','?')}.{src.get('port','?')}",
        f"{dst.get('cell','?')}.{dst.get('port','?')}",
    )


def print_rollup(events):
    # Group consecutive hops with the same module hierarchy.  This matches
    # how a path is read — "we enter the TLB here, leave there" — rather
    # than aggregating across non-contiguous re-entries.
    groups = []
    cur = None
    for e in events:
        mod = cell_module(e.get('to', {}).get('cell', ''))
        if cur is None or cur['module'] != mod:
            if cur is not None:
                groups.append(cur)
            cur = {'module': mod, 'ns': 0.0, 'hops': 0, 'exit_net': ''}
        cur['ns'] += e['delay']
        cur['hops'] += 1
        net = e.get('net', '')
        if net:
            cur['exit_net'] = net
    if cur is not None:
        groups.append(cur)

    print('── module rollup (consecutive hops) ──')
    print(f"  {'Δns':>5}  {'hops':>4}  {'module':<42}  exit net")
    for g in groups:
        mod = g['module']
        if len(mod) > 42:
            mod = '…' + mod[-41:]
        print(f"  {g['ns']:5.2f}  {g['hops']:4d}  {mod:<42}  {g['exit_net']}")
    print()


def print_hops(events):
    print('── full hop trace ──')
    for i, e in enumerate(events):
        cell = e.get('to', {}).get('cell', '')
        port = e.get('to', {}).get('port', '')
        net = e.get('net', '')
        # Show last two hierarchy segments + cleaned signal.  Full cell
        # paths are too long to read; signal name + parent module is enough
        # to locate the hop.
        parts = cell.split('.') if cell else ['?']
        parent = parts[-2] if len(parts) >= 2 else ''
        leaf = cell_signal(cell)
        ident = f"{parent}.{leaf}" if parent else leaf
        line = f"[{i:3d}] {e['type']:8s} {e['delay']:5.2f}ns  {ident}.{port}"
        if net:
            line += f"  ← {net}"
        print(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report')
    ap.add_argument('path_index', type=int)
    ap.add_argument('--no-rollup', action='store_true')
    ap.add_argument('--no-hops', action='store_true')
    args = ap.parse_args()

    with open(args.report) as f:
        data = json.load(f)

    paths = data.get('critical_paths', [])
    if not paths:
        sys.exit("error: no critical_paths in report — run with --detailed-timing-report")
    if args.path_index < 1 or args.path_index > len(paths):
        sys.exit(f"error: path index {args.path_index} out of range (1..{len(paths)})")

    p = paths[args.path_index - 1]
    events = p['path']
    total = sum(e['delay'] for e in events)
    start, end = fmt_path_endpoints(p)

    print(f"═══ Path #{args.path_index}: {p['from']} → {p['to']} ═══")
    print(f"  total: {total:.2f} ns  ({len(events)} hops)")
    print(f"  start: {start}")
    print(f"  end:   {end}")
    print()

    if not args.no_rollup:
        print_rollup(events)
    if not args.no_hops:
        print_hops(events)


if __name__ == '__main__':
    main()
