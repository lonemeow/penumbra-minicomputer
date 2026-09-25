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


class Case:
    """One ISS run.  `regs` maps register number to its expected final
    value; `error` instead expects the ISS to refuse the options with a
    message matching the pattern."""

    def __init__(self, name, program, plusargs, regs=None, error=None):
        self.name, self.program, self.plusargs = name, program, plusargs
        self.regs, self.error = regs or {}, error


CASES = [
    Case(f"+core={core}", "core_identity.s", [f"+core={core}"],
         regs={**presented, **CTRL_READBACK})
    for core, presented in CORE_IDENTITY.items()
] + [
    Case("default core", "core_identity.s", [],
         regs={**CORE_IDENTITY["penumbra1"], **CTRL_READBACK}),
    Case("unknown core", "core_identity.s", ["+core=penumbra9"],
         error=r"unknown \+core generation 'penumbra9'"),
]


def final_registers(stderr):
    """The registers from the last 'SR=... R1=...' line the ISS prints."""
    lines = [l for l in stderr.splitlines() if l.startswith("SR=")]
    if not lines:
        return None
    return {int(n): int(v, 16) for n, v in re.findall(r"R(\d+)=([0-9a-fA-F]+)", lines[-1])}


def run_case(case, iss, asm, outdir):
    src = HERE / case.program
    hexfile = outdir / (src.stem + ".hex")
    subprocess.run(asm + [str(src), "-o", str(hexfile)], check=True,
                   stdout=subprocess.DEVNULL)
    proc = subprocess.run([iss, str(hexfile), "+halt-on-break"] + case.plusargs,
                          stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, timeout=30)
    if case.error:
        if proc.returncode != 0 and re.search(case.error, proc.stderr):
            return []
        return [f"expected refusal matching /{case.error}/, "
                f"got exit {proc.returncode}"]
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
