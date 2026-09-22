#!/usr/bin/env python3
"""Symbolize a penumbra-iss +pcprof execution profile.

The ISS writes instruction counts per PC, exceptions per vector and TLB
misses per faulting page; this resolves the PCs against the binaries
that were running and ranks the functions behind them.

Usage:
    ./pcprof-report.py prof.txt build/doom/doom \\
        build/netbsd-obj/sys/arch/penumbra/compile/GENERIC.DEBUG/netbsd \\
        build/netbsd-dest/lib/libc.so.12@0x20106000

A shared library is mapped wherever the dynamic linker put it, so it
carries its load base after an `@'; a static executable and the kernel
are used as linked.

The counts are instructions executed, not cycles: the ISS models no
cache or memory latency.
"""

import argparse
import bisect
import collections
import os
import subprocess
import sys

# Anything at or above this virtual address is kernel text on this port.
KERNEL_BASE = 0x80000000


class Binary:
    """One ELF image and the symbols to attribute PCs to."""

    def __init__(self, path, base, nm):
        self.name = os.path.basename(path)
        self.base = base
        self.addrs, self.names, self.limit = self._symbols(path, nm)

    @staticmethod
    def _symbols(path, nm):
        run = subprocess.run([nm, "-n", "--defined-only", path],
                             capture_output=True, text=True)
        if run.returncode != 0:
            sys.exit(run.stderr.strip() or "%s: cannot read symbols" % path)
        out = run.stdout
        addrs, names = [], []
        for line in out.splitlines():
            fields = line.split()
            # "<addr> <type> <name>"; t/T is text, w/W a weak alias.
            if len(fields) == 3 and fields[1] in "tTwW":
                addrs.append(int(fields[0], 16))
                names.append(fields[2])
        if not addrs:
            sys.exit("%s: no text symbols" % path)
        # A profile PC past the last symbol belongs to no function, so
        # bound the image by its last symbol plus a generous tail.
        return addrs, names, addrs[-1] + 0x1000

    def contains(self, pc):
        return self.base + self.addrs[0] <= pc < self.base + self.limit

    def lookup(self, pc):
        i = bisect.bisect_right(self.addrs, pc - self.base) - 1
        return self.names[i] if i >= 0 else "?"


def read_profile(path):
    """Return (instructions by PC, misses by page, exceptions by vector)."""
    pcs = collections.Counter()
    misses = collections.Counter()
    vectors = collections.Counter()
    for line in open(path):
        if line.startswith("#"):
            continue
        kind, a, b = line.split()
        if kind == "P":
            pcs[int(a, 16)] += int(b)
        elif kind == "M":
            misses[int(a, 16)] += int(b)
        elif kind == "V":
            vectors[int(a)] += int(b)
    if not pcs:
        sys.exit("%s: no instruction counts" % path)
    return pcs, misses, vectors


VECTOR_NAMES = {0: "bus fault", 1: "timer", 2: "TLB miss", 3: "TLB protection",
                4: "illegal", 5: "syscall", 6: "privilege", 7: "alignment",
                8: "arithmetic", 9: "external IRQ"}


def main():
    ap = argparse.ArgumentParser(
        description="Symbolize a penumbra-iss +pcprof profile")
    ap.add_argument("profile", help="file written by +pcprof=")
    ap.add_argument("binary", nargs="+",
                    help="ELF image, optionally path@loadbase")
    ap.add_argument("-n", "--top", type=int, default=40,
                    help="functions to list (default 40)")
    ap.add_argument("--frames", type=float, default=0,
                    help="divide counts by this many iterations")
    ap.add_argument("--nm", default=os.environ.get(
        "LLVM_PREFIX", "build/llvm") + "/bin/llvm-nm",
        help="llvm-nm to read symbols with")
    args = ap.parse_args()

    binaries = []
    for spec in args.binary:
        path, _, base = spec.partition("@")
        binaries.append(Binary(path, int(base, 0) if base else 0, args.nm))

    pcs, misses, vectors = read_profile(args.profile)
    total = sum(pcs.values())

    by_function = collections.Counter()
    by_binary = collections.Counter()
    unclaimed = collections.Counter()
    for pc, n in pcs.items():
        for b in binaries:
            if b.contains(pc):
                by_function["%s:%s" % (b.name, b.lookup(pc))] += n
                by_binary[b.name] += n
                break
        else:
            unclaimed[pc] += n

    scale = args.frames if args.frames else 1.0
    per = "/iter" if args.frames else ""
    print("%d instructions counted%s" % (total, ", %.0f%s" % (total / scale, per)
                                         if args.frames else ""))
    print("\nby image:")
    for name, n in by_binary.most_common():
        print("  %-24s %12d  %5.1f%%" % (name, n, 100.0 * n / total))
    if unclaimed:
        n = sum(unclaimed.values())
        print("  %-24s %12d  %5.1f%%" % ("unattributed", n, 100.0 * n / total))
        # Which pages they sit on says what to map: a shared library, or
        # code copied to a runtime address such as the vector page.
        pages = collections.Counter()
        for pc, count in unclaimed.items():
            pages[pc & ~0xFFF] += count
        print("    pages, busiest first (pass the owning image as "
              "path@base):")
        for page, count in pages.most_common(6):
            print("      0x%08x  %12d" % (page, count))

    print("\ntop functions:")
    cumulative = 0
    for name, n in by_function.most_common(args.top):
        cumulative += n
        print("  %-40s %12d %6.2f%% %6.1f%%%s"
              % (name, n, 100.0 * n / total, 100.0 * cumulative / total,
                 "  %9.0f%s" % (n / scale, per) if args.frames else ""))

    if vectors:
        print("\nexceptions:")
        for v, n in vectors.most_common():
            print("  %-16s %12d%s" % (VECTOR_NAMES.get(v, "vector %d" % v), n,
                                      "  %9.0f%s" % (n / scale, per)
                                      if args.frames else ""))

    if misses:
        total_misses = sum(misses.values())
        print("\nTLB misses: %d over %d pages" % (total_misses, len(misses)))
        # The main TLB takes the set from the low 5 bits of the VPN, so a
        # skew across sets is a conflict, not a capacity, problem.
        sets = collections.Counter()
        for page, n in misses.items():
            sets[(page >> 12) & 31] += n
        worst = sets.most_common(4)
        print("  busiest sets: " + ", ".join(
            "%d (%.1f%%)" % (s, 100.0 * n / total_misses) for s, n in worst))
        print("  busiest pages:")
        for page, n in misses.most_common(12):
            owner = next((b.name for b in binaries if b.contains(page)),
                         "kernel" if page >= KERNEL_BASE else "data/stack/map")
            print("    0x%08x  %10d  %s" % (page, n, owner))


if __name__ == "__main__":
    main()
