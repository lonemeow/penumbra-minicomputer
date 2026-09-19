#!/usr/bin/env python3
"""Print high-fanout nets from a yosys synth_ecp5 JSON output, per clock domain.

Useful for diagnosing nextpnr-ecp5 routing congestion. Nets with very
high fanout (hundreds to thousands of sinks) routed through general
fabric — rather than the chip's dedicated global routing — pay heavy
congestion cost. Common offenders: synchronous reset, wide CE/enable
broadcasts, hand-rolled clock-divider outputs.

Usage:
    list_fanout.py build/ulx3s_penumbra1_top.json
    list_fanout.py build/ulx3s_penumbra1_top.json -n 30 -m 10
    list_fanout.py build/ulx3s_penumbra2_top.json --domain clk_sdram
    list_fanout.py build/ulx3s_penumbra2_top.json --pnr-log build/ulx3s_penumbra2_top_pnr.log

Notes:
    * The design is analyzed as nextpnr places it: flattened through
      every keep_hierarchy instance, which synthesis leaves as separate
      module definitions in the JSON. Net names inside such an instance
      carry its instance path (u_sdram.u_ctrl.state, ...).
    * "Fanout" means sink-port count on leaf cells: how many primitive
      input ports drink from the net. Driver count (should be 1) is
      excluded, and hierarchy boundaries are transparent.
    * Each sink is attributed to the clock domain of the registers it
      feeds: a register's own inputs to its clock, a combinational
      cell's inputs to the domains of every register its outputs reach.
      Domains are named after their clock net. Sinks that reach no
      register are listed as (no register): pins, PLL inputs, and logic
      feeding only bits a keep_hierarchy module ignores, which synthesis
      cannot trim across the boundary.
    * Nets nextpnr routes on the global clock network are marked
      `global`: their fanout costs no general routing, so they are not
      congestion suspects. Which nets get a global is nextpnr's call,
      made while packing — seed-independent — so any nextpnr log of the
      same synthesized netlist says; --pnr-log names candidates and the
      newest one no older than the JSON is used. Without one the column
      reads `?`.
    * For multi-bit busses, both per-bit and per-bus totals are shown
      so you can see whether an entire bus is wide-fanout or just one
      bit (e.g. bit 0 of a status flag often dominates).
"""
import json
import os
import re
import sys
import argparse
from collections import defaultdict

NO_REGISTER = '(no register)'

# Leaf clock pins, per primitive. A cell with none of these connected to
# a real net is combinational (e.g. a MULT18X18D with every register
# bypassed).
CLOCK_PINS = {'CLK', 'CLKA', 'CLKB', 'CLK0', 'CLK1', 'CLK2', 'CLK3', 'WCK', 'SCLK'}

# Clock generation and buffering: their inputs are the clock tree's own
# roots, not logic in any domain, so they end domain traversal.
CLOCK_TREE_TYPES = {'EHXPLLL', 'DCCA'}

# Distributed RAM: the write side is clocked by WCK, the read address is
# combinational through to DO.
DPR_TYPE = 'TRELLIS_DPR16X4'
DPR_WRITE_PORTS = {'WAD', 'DI', 'WRE'}


def _name_score(local_name, full_name, hidden):
    """Lower is better. Used to pick the most readable net name when
    yosys aliases multiple netnames to the same bit (common — e.g.
    `rst` and `ac_spi_sel_LUT4_Z_D_..._D_Z` may be the *same* bit
    because the LUT's input is rst). We want `rst`, not the LUT-named
    alias. A flattened bit also carries a name at every hierarchy level
    it crosses; the shortest full path is the outermost, most familiar
    one (`clk_sdram`, not `u_sdram.u_ctrl.i_clk`).

    Heuristic: hidden / $-prefixed names (synth temporaries) are worst,
    then names containing underscored synth artifacts (`_LUT4`,
    `_TRELLIS_`, `_CCU2C`, `_PFUMX`), then ordinary names — broken by
    full-path length.
    """
    is_temp = 1 if hidden or local_name.startswith('$') else 0
    has_synth_marker = 1 if any(s in local_name for s in
        ('_LUT4', '_TRELLIS_', '_CCU2C', '_PFUMX', '_DI_', '_LSR_')) else 0
    return (is_temp, has_synth_marker, len(full_name), full_name)


def sections_for(domains):
    """Domain sections one sink counts under, given the set of clock
    domains its cell feeds.

    `domains` is a frozenset of domain names — clock net names such as
    'clk' or 'clk_sdram'. Each returned name is a report section whose
    per-net totals this one sink is added to, once per section; a name
    no clock uses (like NO_REGISTER) becomes a section of its own.
    """
    if not domains:
        return [NO_REGISTER]
    if len(domains) == 1:
        return list(domains)
    return sorted(domains) + ['(cross-domain)']


def read_globals(json_path, logs):
    """Nets nextpnr promoted to the global network, from the newest log
    in `logs` that is not older than the synthesized JSON and has got
    as far as promotion — a run still starting up must not read as
    "nothing is global".  Returns ({net name: global index, or None
    until routing assigns one}, log path), or (None, None)."""
    json_mtime = os.path.getmtime(json_path)
    fresh = sorted((l for l in logs
                    if os.path.exists(l) and os.path.getmtime(l) >= json_mtime),
                   key=os.path.getmtime, reverse=True)
    for log in fresh:
        promoted = {}
        for line in open(log, errors='replace'):
            m = re.search(r'promoting clock net (\S+) to global network', line)
            if m:
                promoted.setdefault(m.group(1), None)
            m = re.search(r'routing clock net (?:\$glbnet\$)?(\S+) using global (\d+)', line)
            if m:
                promoted[m.group(1)] = int(m.group(2))
        if promoted:
            return promoted, log
    return None, None


class Design:
    """The synthesized netlist flattened through keep_hierarchy
    instances: leaf cells with globally numbered bits, plus every
    net name seen for each bit."""

    def __init__(self, d):
        self.mods = d['modules']
        self.library = {n for n, m in self.mods.items()
                        if {'blackbox', 'cell_library', 'whitebox'}
                        & set(m.get('attributes', {}))}
        tops = [n for n, m in self.mods.items()
                if 'top' in m.get('attributes', {})]
        if len(tops) != 1:
            sys.exit('expected one module marked top, found %d' % len(tops))
        self.top = tops[0]
        self.cells = []          # (path, type, {port: [bit]}, {port: dir})
        self.names = defaultdict(list)   # bit -> [(local, full, hidden, width)]
        self.netbits = {}        # full name -> ([bit], offset, upto)
        self.instances = 0
        self._name_cache = {}
        self._next_bit = 1 + max(
            (b for m in self.mods.values()
               for w in m.get('netnames', {}).values()
               for b in w.get('bits', []) if isinstance(b, int)),
            default=0)
        self._parent = {}
        self._flatten(self.top, '', None)
        self._canonicalize()

    # A module that passes an input straight through to an output (an
    # MMU forwarding page-offset bits, say) uses one internal bit for
    # both ports, so the two parent nets it connects are one electrical
    # net.  Union-find merges them; every bit is then replaced by its
    # representative once flattening is done.
    def _find(self, b):
        root = b
        while self._parent.get(root, root) != root:
            root = self._parent[root]
        while b != root:
            self._parent[b], b = root, self._parent.get(b, b)
        return root

    def _union(self, a, b):
        ra, rb = self._find(a), self._find(b)
        if ra != rb:
            self._parent[rb] = ra

    def _canonicalize(self):
        def c(b):
            return self._find(b) if isinstance(b, int) else b
        self.cells = [(path, ctype, {p: [c(b) for b in bits]
                                     for p, bits in conns.items()}, pdir)
                      for path, ctype, conns, pdir in self.cells]
        merged = defaultdict(list)
        for b, cands in self.names.items():
            merged[c(b)].extend(cands)
        self.names = merged
        self.netbits = {n: ([c(b) for b in bits], off, upto)
                        for n, (bits, off, upto) in self.netbits.items()}

    def _hierarchical(self, cell_type):
        return cell_type in self.mods and cell_type not in self.library

    def _flatten(self, mod_name, prefix, port_map):
        mod = self.mods[mod_name]
        local_to_global = {}

        def g(b):
            if not isinstance(b, int):
                return b        # constant '0' / '1' / 'x' / 'z'
            if port_map is None:
                return b        # the top keeps yosys' numbering
            if b in port_map:
                return port_map[b]
            if b not in local_to_global:
                local_to_global[b] = self._next_bit
                self._next_bit += 1
            return local_to_global[b]

        for wname, w in mod.get('netnames', {}).items():
            bits = w.get('bits', [])
            width = sum(1 for b in bits if isinstance(b, int))
            hidden = bool(w.get('hide_name'))
            for b in bits:
                if isinstance(b, int):
                    self.names[g(b)].append((wname, prefix + wname, hidden, width))
            self.netbits[prefix + wname] = ([g(b) for b in bits],
                                            w.get('offset', 0), bool(w.get('upto')))

        for cname, c in mod.get('cells', {}).items():
            ctype = c['type']
            conns = {p: [g(b) for b in bits]
                     for p, bits in c.get('connections', {}).items()}
            if self._hierarchical(ctype):
                sub = self.mods[ctype]
                sub_map = {}
                for pname, p in sub.get('ports', {}).items():
                    for sb, pb in zip(p.get('bits', []), conns.get(pname, [])):
                        if not isinstance(sb, int):
                            continue
                        prev = sub_map.setdefault(sb, pb)
                        if prev != pb and isinstance(prev, int) and isinstance(pb, int):
                            self._union(prev, pb)
                self.instances += 1
                self._flatten(ctype, prefix + cname + '.', sub_map)
            elif conns:
                self.cells.append((prefix + cname, ctype, conns,
                                   c.get('port_directions', {})))

    def bits_named(self, names):
        """Bits nextpnr refers to by `names`: a whole net's full name, or
        one bit of a bus in nextpnr's `name[i]` form, where i counts in
        the bus's declared numbering (yosys `offset` / `upto`)."""
        out = set()
        for n in names:
            if n in self.netbits:
                out.update(b for b in self.netbits[n][0] if isinstance(b, int))
                continue
            m = re.match(r'(.*)\[(\d+)\]$', n)
            if m and m.group(1) in self.netbits:
                bits, off, upto = self.netbits[m.group(1)]
                pos = int(m.group(2)) - off
                if upto:
                    pos = len(bits) - 1 - pos
                if 0 <= pos < len(bits) and isinstance(bits[pos], int):
                    out.add(bits[pos])
        return out

    def net_name(self, bit):
        if bit in self._name_cache:
            return self._name_cache[bit]
        cands = self.names.get(bit)
        if not cands:
            result = (None, 1)
        else:
            local, full, hidden, width = min(
                cands, key=lambda n: _name_score(n[0], n[1], n[2]))
            result = (full, width)
        self._name_cache[bit] = result
        return result


def _real_bits(bits):
    return [b for b in bits if isinstance(b, int)]


def analyze(design):
    cells = design.cells

    # Sinks per bit: (cell index, port).
    sinks = defaultdict(list)
    for ci, (_, _, conns, pdir) in enumerate(cells):
        for port, bits in conns.items():
            if pdir.get(port) == 'input':
                for b in _real_bits(bits):
                    sinks[b].append((ci, port))

    def clock_label(bit):
        name, _ = design.net_name(bit)
        return name or ('$bit%d' % bit)

    def clocks_of(conns, pins):
        return {clock_label(b) for p in pins
                for b in _real_bits(conns.get(p, []))}

    # How each cell's input ports resolve to domains: either a fixed set
    # (register inputs, clock-tree roots) or "through" the cell's
    # combinational outputs.
    def port_role(ci, port):
        _, ctype, conns, _ = cells[ci]
        if ctype in CLOCK_TREE_TYPES:
            return ('fixed', frozenset())
        if port in CLOCK_PINS:
            return ('fixed', frozenset(clocks_of(conns, [port])))
        if ctype == DPR_TYPE:
            if port in DPR_WRITE_PORTS:
                return ('fixed', frozenset(clocks_of(conns, ['WCK'])))
            return ('through', None)
        if ctype == 'DP16KD':
            side = re.sub(r'\d+$', '', port)[-1]
            return ('fixed', frozenset(clocks_of(conns, ['CLK' + side])))
        cell_clocks = clocks_of(conns, sorted(CLOCK_PINS & set(conns)))
        if cell_clocks:
            return ('fixed', frozenset(cell_clocks))
        return ('through', None)

    # Domains reached from a combinational cell's outputs, memoized.
    # Iterative post-order so long carry / LUT chains can't hit the
    # recursion limit; a combinational loop contributes nothing.
    memo = {}

    def through_outputs(ci):
        _, ctype, conns, pdir = cells[ci]
        outs = ['DO'] if ctype == DPR_TYPE else \
               [p for p, d in pdir.items() if d == 'output']
        return [b for p in outs for b in _real_bits(conns.get(p, []))]

    def reached(ci0):
        if ci0 in memo:
            return memo[ci0]
        stack = [(ci0, False)]
        on_path = set()
        while stack:
            ci, expanded = stack.pop()
            if ci in memo:
                continue
            succ = []
            fixed = set()
            for b in through_outputs(ci):
                for sci, sport in sinks.get(b, []):
                    kind, doms = port_role(sci, sport)
                    if kind == 'fixed':
                        fixed |= doms
                    else:
                        succ.append(sci)
            if not expanded:
                on_path.add(ci)
                stack.append((ci, True))
                for s in succ:
                    if s not in memo and s not in on_path:
                        stack.append((s, False))
            else:
                on_path.discard(ci)
                for s in succ:
                    fixed |= memo.get(s, frozenset())
                memo[ci] = frozenset(fixed)
        return memo[ci0]

    # Per domain: sink count per bit.
    per_domain = defaultdict(lambda: defaultdict(int))
    for b, blist in sinks.items():
        for ci, port in blist:
            kind, doms = port_role(ci, port)
            if kind == 'through':
                doms = reached(ci)
            for sec in sections_for(doms):
                per_domain[sec][b] += 1
    return per_domain


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('json', help='yosys JSON (e.g. build/ulx3s_penumbra1_top.json)')
    ap.add_argument('-n', '--top', type=int, default=25,
                    help='show top N nets per domain (default 25)')
    ap.add_argument('-m', '--min-fanout', type=int, default=50,
                    help='only show nets with fanout >= this (default 50)')
    ap.add_argument('-d', '--domain', action='append', default=[],
                    help='only this clock domain (repeatable); names as '
                         'printed in the section headers')
    ap.add_argument('--pnr-log', action='append', default=[],
                    help='candidate nextpnr log for the global-net column '
                         '(repeatable); the newest not older than the JSON wins')
    args = ap.parse_args()

    design = Design(json.load(open(args.json)))
    per_domain = analyze(design)
    promoted, log = read_globals(args.json, args.pnr_log)
    global_bits = design.bits_named(promoted) if promoted else set()

    order = sorted(per_domain, key=lambda k: -sum(per_domain[k].values()))
    if args.domain:
        missing = [x for x in args.domain if x not in per_domain]
        if missing:
            sys.exit('no domain %s; domains: %s'
                     % (', '.join(missing), ', '.join(order)))
        order = [k for k in order if k in args.domain]

    print(f'=== {design.top}: {len(design.cells)} leaf cells, flattened through '
          f'{design.instances} kept-hierarchy instances ===')
    print(f'    domains: {", ".join(order)}')
    if promoted is None:
        print('    globals: unknown — no nextpnr log newer than the JSON '
              '(route column reads ?)')
    else:
        def readable(n):
            # nextpnr may name a net by any of its aliases; show ours.
            bits = design.bits_named([n])
            return design.net_name(min(bits))[0] if len(bits) == 1 else n
        glist = ', '.join(readable(n) if i is None else f'{readable(n)}→{i}'
                          for n, i in sorted(promoted.items(),
                                             key=lambda x: (x[1] is None, x[1] or 0)))
        print(f'    globals ({log}): {glist}')
    print()

    for dom in order:
        bit_sinks = per_domain[dom]
        net_sinks = defaultdict(int)
        net_max_bit = defaultdict(int)
        net_width = {}
        net_global = set()
        for b, s in bit_sinks.items():
            name, width = design.net_name(b)
            name = name or ('$bit%d' % b)
            net_sinks[name] += s
            net_max_bit[name] = max(net_max_bit[name], s)
            net_width[name] = width
            if b in global_bits:
                net_global.add(name)
        ranked = sorted(net_sinks.items(), key=lambda x: -x[1])

        print(f'── {dom}: {sum(bit_sinks.values())} sinks '
              f'(top {args.top}, min fanout {args.min_fanout}) ──')
        print(f'  {"total":>7s}  {"width":>5s}  {"max_bit":>7s}  {"route":6s}  net')
        print(f'  {"-"*7}  {"-"*5}  {"-"*7}  {"-"*6}  {"-"*40}')
        shown = 0
        for n, s in ranked:
            if s < args.min_fanout or shown >= args.top:
                break
            route = '?' if promoted is None else ('global' if n in net_global else '')
            print(f'  {s:>7d}  {net_width[n]:>5d}  {net_max_bit[n]:>7d}  {route:6s}  {n}')
            shown += 1
        if shown == 0:
            print(f'  (no nets with fanout >= {args.min_fanout})')
        print()


if __name__ == '__main__':
    main()
