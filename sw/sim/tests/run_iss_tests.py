#!/usr/bin/env python3
"""Run the ISS option tests: programs with known answers under chosen options.

Each case assembles a program from this directory, runs the ISS on it
with a set of plusargs until the program's BREAK, and checks the
registers the ISS prints at exit.  Expected values are written here from
the hardware's own parameters rather than read back from the ISS's
tables, so a case pins the ISS to the machine it claims to present.

Usage: run_iss_tests.py --iss <penumbra-iss> --asm "<assembler command>"
                        --outdir <dir>
"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Every case runs a few thousand instructions; a run still going after this
# long is stuck, not slow.
RUN_TIMEOUT = 30


def cache_info(sets, ways, line_bytes, vipt):
    """A cache INFO word in penumbra_pkg.sv's unified layout."""
    return (int(vipt) << 26) | (ways << 21) | (sets << 6) | (line_bytes // 4)


# What each generation presents, from its RTL.  penumbra1: cpu_core's two
# cache_vipt instances at their defaults (64 sets, direct-mapped, 16-byte
# lines) and cpuid's default name "Penumbra/1".  penumbra2 and penumbra2_5:
# machine_penumbra2's cache_bram_vipt pair (4 KiB, 2 ways), with the name
# tagging the variant.  Every generation shares l2_cache's defaults.
L2_INFO = cache_info(1024, 4, 16, vipt=False)
PENUMBRA1_L1 = cache_info(64, 1, 16, vipt=True)
PENUMBRA2_L1 = cache_info(128, 2, 16, vipt=True)
CORE_IDENTITY = {
    "penumbra1":   {2: 0x0000312F, 3: PENUMBRA1_L1, 4: PENUMBRA1_L1, 5: L2_INFO},
    "penumbra2":   {2: 0x0000322F, 3: PENUMBRA2_L1, 4: PENUMBRA2_L1, 5: L2_INFO},
    "penumbra2_5": {2: 0x352E322F, 3: PENUMBRA2_L1, 4: PENUMBRA2_L1, 5: L2_INFO},
}
CTRL_READBACK = {6: 0, 7: 1}


def hierarchy(l1i=(0, 0), l1d=(0, 0, 0, 0), l2=(0, 0, 0, 0), fills=None, prediction=None):
    """A cache model's expected counts for one hierarchy over one window:
    L1I (read hits, read misses), L1D and L2 (read hits, read misses,
    write hits, write misses), and the L1 fills by (kind, mode) as
    (from L2, from memory).  Fills not listed must be zero.  `prediction`,
    when given, is the next-line buffer's (served once, served continue,
    look-ups, missing L2, unused)."""
    want = {"L1I": tuple(l1i) + (0, 0), "L1D": tuple(l1d), "L2": tuple(l2),
            "fills": fills or {}}
    if prediction is not None:
        want["prediction"] = tuple(prediction)
    return want


# cache_data.s, phase by phase (see the program's header).  A 16-byte line
# is 4 words, so an L2-served fill counts 4 L2 read hits and a
# memory-served one 1 read miss and 3 read hits.  Phase E's L2 sequence
# takes 5 misses and 3 hits under the L2's tree-PLRU.
DATA_PENUMBRA1 = hierarchy(                 # 1 KiB direct-mapped L1-D
    l1d=(0, 128 + 128 + 1 + 8, 0, 2),       # A, B, D, E all miss; C both miss
    l2=(128 * 3 + 128 * 4 + 4 + (5 * 3 + 3 * 4), 128 + 5, 1, 1),
    fills={("D", "super"): (128 + 1 + 3, 128 + 5)})
DATA_PENUMBRA2 = hierarchy(                 # 4 KiB 2-way L1-D
    l1d=(128, 128 + 1 + 8, 1, 1),           # B hits; C hits 0x1000
    l2=(128 * 3 + 4 + (5 * 3 + 3 * 4), 128 + 5, 1, 1),
    fills={("D", "super"): (1 + 3, 128 + 5)})

# cache_ifetch.s: 33 fetches.  Sharing a set, the loop's two lines miss
# twice a round for ten rounds and the exit hits; apart, each misses once.
IFETCH_PENUMBRA1 = hierarchy(
    l1i=(10 + 3, 20), l2=(2 * 3 + 18 * 4, 2, 0, 0), fills={("I", "super"): (18, 2)})
IFETCH_PENUMBRA2 = hierarchy(
    l1i=(31, 2), l2=(2 * 3, 2, 0, 0), fills={("I", "super"): (0, 2)})

# cache_nextline.s, step by step (see the program's header).  Each pass
# of the 8-line run fetches 29 instructions and misses 8 times; each
# one-instruction snippet misses once.  Misses: run 8 + r1 + run 8 + r0 +
# r1 + last; the passes' other 21 fetches hit.  Look-ups follow every
# miss but last's (19), missing L2 in pass 1 (8), after r1 twice and
# after the warm run's final line; the one r0 loads is invalidated unused.
NEXTLINE = hierarchy(
    l1i=(2 * 21, 8 + 1 + 8 + 1 + 1 + 1),
    l2=(8 * 3 + 3 + 8 * 4 + 3 + 4 + 3, 8 + 1 + 1 + 1, 0, 0),
    fills={("I", "super"): (8 + 1, 8 + 1 + 1 + 1)},
    prediction=(4, 7, 8 + 1 + 8 + 1 + 1, 8 + 1 + 1 + 1, 1))

# cache_window.s: inside the window, 16 re-loaded lines hit and 16 new
# ones miss in both levels.
WINDOW_PENUMBRA1 = hierarchy(
    l1d=(16, 16, 0, 0), l2=(16 * 3, 16, 0, 0), fills={("D", "user"): (0, 16)})


def tlb_misses(user=0, supervisor=0):
    """A shadow TLB's expected misses over one window, or with the label
    "real" the refills the program's own miss handler took."""
    return {"tlb": (user, supervisor)}


# tlb_basic.s: the same in every layout, and what the real TLB takes.
TLB_BASIC = tlb_misses(user=2, supervisor=6)
TLB_BASIC_LAYOUTS = ["penumbra1", "penumbra2_5", "32x2/lru", "64x2/hash", "16x4", "1x64/lru"]
# tlb_assoc.s, whose real TLB conflicts on every set it shares.
TLB_ASSOC_LAYOUTS = ["32x4/lru", "64x2/lru", "16x4/lru", "1x64/lru"]


class Case:
    """One ISS run.  `regs` maps register number to its expected final
    value; `windows` lists the model windows expected in the report, each
    as (how it opened and closed, {label: expected counts}), a cache
    model's labelled by its hierarchy with hierarchy(...) and a TLB
    model's by "tlb <shadow>" with tlb_misses(...); `error` instead
    expects the ISS to refuse the options with a message matching the
    pattern."""

    def __init__(self, name, program, plusargs, regs=None, windows=None, error=None):
        self.name, self.program, self.plusargs = name, program, plusargs
        self.regs, self.windows, self.error = regs or {}, windows, error


CASES = [
    Case(f"+core={core}", "core_identity.s", [f"+core={core}"],
         regs={**presented, **CTRL_READBACK})
    for core, presented in CORE_IDENTITY.items()
] + [
    Case("default core", "core_identity.s", [],
         regs={**CORE_IDENTITY["penumbra1"], **CTRL_READBACK}),
    Case("unknown core", "core_identity.s", ["+core=penumbra9"],
         error=r"unknown \+core generation 'penumbra9'"),
    Case("cachemodel data", "cache_data.s",
         ["+cachemodel=penumbra1,penumbra2,penumbra2_5,1k/1:1k/1:64k/4,4k/2:4k/2:64k/4"],
         windows=[("reset to exit", {
             "penumbra1": DATA_PENUMBRA1, "penumbra2": DATA_PENUMBRA2,
             "penumbra2_5": DATA_PENUMBRA2, "1k/1:1k/1:64k/4": DATA_PENUMBRA1,
             "4k/2:4k/2:64k/4": DATA_PENUMBRA2})]),
    Case("cachemodel follows +core", "cache_data.s", ["+core=penumbra2_5", "+cachemodel"],
         windows=[("reset to exit", {"penumbra2_5": DATA_PENUMBRA2})]),
    Case("cachemodel ifetch", "cache_ifetch.s", ["+cachemodel=penumbra1,penumbra2"],
         windows=[("reset to exit", {"penumbra1": IFETCH_PENUMBRA1,
                                     "penumbra2": IFETCH_PENUMBRA2})]),
    Case("cachemodel next-line prediction", "cache_nextline.s",
         ["+cachemodel=penumbra1,penumbra2_5"],
         windows=[("reset to exit", {"penumbra1": NEXTLINE, "penumbra2_5": NEXTLINE})]),
    Case("model window", "cache_window.s",
         ["+cachemodel", "+model_start=0xFFFF0800", "+model_stop=0xFFFF0900"],
         windows=[("start PC to stop PC", {"penumbra1": WINDOW_PENUMBRA1})]),
    Case("tlbmodel layouts", "tlb_basic.s", ["+tlbmodel=" + ",".join(TLB_BASIC_LAYOUTS)],
         windows=[("reset to exit", {f"tlb {label}": TLB_BASIC
                                     for label in ["real"] + TLB_BASIC_LAYOUTS})]),
    Case("tlbmodel follows +core", "tlb_basic.s", ["+core=penumbra2", "+tlbmodel"],
         windows=[("reset to exit", {"tlb real": TLB_BASIC, "tlb penumbra2": TLB_BASIC})]),
    Case("tlbmodel associativity", "tlb_assoc.s",
         ["+tlbmodel=32x2/lru," + ",".join(TLB_ASSOC_LAYOUTS)],
         windows=[("reset to exit", {
             "tlb real": tlb_misses(supervisor=15), "tlb 32x2/lru": tlb_misses(supervisor=13),
             **{f"tlb {label}": tlb_misses(supervisor=10) for label in TLB_ASSOC_LAYOUTS}})]),
    Case("both models, one window", "cache_window.s",
         ["+cachemodel", "+tlbmodel", "+model_start=0xFFFF0800", "+model_stop=0xFFFF0900"],
         windows=[("start PC to stop PC", {"penumbra1": WINDOW_PENUMBRA1,
                                           "tlb real": tlb_misses(), "tlb penumbra1": tlb_misses()})]),
    Case("tlbmodel bad sets", "tlb_basic.s", ["+tlbmodel=48x2"],
         error=r"sets must be a power of two"),
    Case("tlbmodel unknown option", "tlb_basic.s", ["+tlbmodel=64x2/fifo"],
         error=r"unknown option '/fifo'"),
    Case("cachemodel bad ways", "cache_data.s", ["+cachemodel=4k/3:4k/2:64k/4"],
         error=r"ways must be a power of two"),
    Case("cachemodel unknown item", "cache_data.s", ["+cachemodel=penumbra7"],
         error=r"neither a generation nor L1I:L1D:L2"),
]


def final_registers(stderr):
    """The registers from the last 'SR=... R1=...' line the ISS prints."""
    lines = [l for l in stderr.splitlines() if l.startswith("SR=")]
    if not lines:
        return None
    return {int(n): int(v, 16) for n, v in re.findall(r"R(\d+)=([0-9a-fA-F]+)", lines[-1])}


def parse_report(text):
    """The models' windows: [(span, {label: counts})]."""
    windows, counts = [], None
    for line in text.splitlines():
        m = re.match(r"== model window \d+: \d+ instructions, (.+) ==$", line)
        if m:
            windows.append((m.group(1), {}))
            continue
        m = re.match(r"-- cache (\S+): ", line)
        if m:
            counts = windows[-1][1].setdefault(m.group(1), {"fills": {}})
            continue
        if line.startswith("-- tlb: "):
            counts = None
            continue
        m = re.match(r"\s+(\S+)\s+\d+x\d+/\S+\s+\d+\s+(\d+)\s+(\d+)\s", line)
        if m and counts is None:
            windows[-1][1][f"tlb {m.group(1)}"] = {"tlb": (int(m.group(2)), int(m.group(3)))}
            continue
        m = re.match(r"\s+(L1I|L1D|L2)\s+(\d+)\s+(\d+)\s+(\d+|-)\s+(\d+|-)", line)
        if m:
            counts[m.group(1)] = tuple(0 if v == "-" else int(v) for v in m.groups()[1:])
            continue
        m = re.match(r"\s+([ID]) (user|super)\s+(\d+)\s+(\d+)", line)
        if m:
            counts["fills"][(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))
            continue
        m = re.match(r"\s+next-line\s+buffer serves (\d+) \(once\) and (\d+) \(continue\)"
                     r".* looks up (\d+), (\d+) missing L2, (\d+) unused", line)
        if m:
            counts["prediction"] = tuple(int(v) for v in m.groups())
    return windows


def compare_windows(expected, got):
    problems = []
    if [span for span, _ in got] != [span for span, _ in expected]:
        return [f"windows {[s for s, _ in got]}, expected {[s for s, _ in expected]}"]
    for (span, want_cfgs), (_, got_cfgs) in zip(expected, got):
        for label, want in want_cfgs.items():
            have = got_cfgs.get(label)
            if have is None:
                problems.append(f"{span}: no report for {label}")
                continue
            if "tlb" in want:
                if have.get("tlb") != want["tlb"]:
                    problems.append(f"{label} misses (user, supervisor) {have.get('tlb')}, "
                                    f"expected {want['tlb']}")
                continue
            for level in ("L1I", "L1D", "L2"):
                if have.get(level) != want[level]:
                    problems.append(f"{label} {level} {have.get(level)}, expected {want[level]}")
            for key in set(want["fills"]) | set(have["fills"]):
                w, h = want["fills"].get(key, (0, 0)), have["fills"].get(key, (0, 0))
                if w != h:
                    problems.append(f"{label} fills {key[0]} {key[1]} {h}, expected {w}")
            if "prediction" in want and have.get("prediction") != want["prediction"]:
                problems.append(f"{label} next-line {have.get('prediction')}, "
                                f"expected {want['prediction']}")
    return problems


def run_case(case, iss, asm, outdir):
    src = HERE / case.program
    hexfile = outdir / (src.stem + ".hex")
    subprocess.run(asm + [str(src), "-o", str(hexfile)], check=True,
                   stdout=subprocess.DEVNULL)
    report = outdir / (re.sub(r"\W+", "_", case.name) + ".report")
    plusargs = case.plusargs + ([f"+model_report={report}"] if case.windows else [])
    try:
        proc = subprocess.run([iss, str(hexfile), "+halt-on-break"] + plusargs,
                              stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return [f"no BREAK within {RUN_TIMEOUT} s"]
    if case.error:
        if proc.returncode != 0 and re.search(case.error, proc.stderr):
            return []
        return [f"expected refusal matching /{case.error}/, "
                f"got exit {proc.returncode}"]
    if case.windows is not None:
        if proc.returncode != 0 or not report.exists():
            return [f"no model report (exit {proc.returncode}): {proc.stderr.strip()[-200:]}"]
        return compare_windows(case.windows, parse_report(report.read_text()))
    regs = final_registers(proc.stderr)
    if regs is None:
        return [f"no register dump (exit {proc.returncode})"]
    return [f"R{r} = 0x{regs.get(r, 0):08X}, expected 0x{want:08X}"
            for r, want in sorted(case.regs.items()) if regs.get(r) != want]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--iss", required=True)
    ap.add_argument("--asm", required=True)
    ap.add_argument("--outdir", required=True, type=Path)
    args = ap.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)

    failed = 0
    for case in CASES:
        problems = run_case(case, args.iss, shlex.split(args.asm), args.outdir)
        verdict = "\033[31mFAIL\033[0m" if problems else "\033[32mPASS\033[0m"
        print(f"  {verdict}  {case.name}")
        for p in problems:
            print(f"          {p}")
        failed += bool(problems)
    print(f"{len(CASES) - failed}/{len(CASES)} ISS option tests passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
