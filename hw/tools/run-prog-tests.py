#!/usr/bin/env python3
"""Assemble and run hardware test programs.

The execution half of the program-test convention
(doc/internals/build-system.md): programs self-check into R1 (1 = PASS)
and end with BREAK; per-program metadata rides in lit-style header tags
this script scans:

  ; RUNNER: tb_<name>     run under a bespoke testbench instead of the
                          default runner (rtl mode; in iss mode the
                          program is skipped -- the ISS cannot provide
                          testbench-driven stimulus)
  ; REQUIRES: <cap> ...   capabilities the executing integration must
                          provide; unmet => reported SKIP, never silent

The Makefile resolves which programs and which runner binaries; this
script owns tag scanning, assembly, execution, and the report.

rtl mode: runs "<command> +rom_hex=<hex>"; exit code 0 is PASS.
iss mode: feeds "break" on stdin to "<command> <hex> +halt-on-break
          +trace=<file>" and checks R1=00000001 on the final trace line.
"""

import argparse
import re
import shlex
import subprocess
import sys
from collections import Counter
from pathlib import Path

TAG_RE = re.compile(r"^\s*[;/]+\s*(RUNNER|REQUIRES):\s*(.*)$")


def scan_tags(path):
    """Collect header-tag values: {'RUNNER': [...], 'REQUIRES': [...]}."""
    tags = {"RUNNER": [], "REQUIRES": []}
    for line in path.read_text().splitlines():
        m = TAG_RE.match(line)
        if m:
            tags[m.group(1)] += m.group(2).split()
    return tags


def report(color, verdict, name, detail=""):
    print(f"  \033[{color}m{verdict}\033[0m  {name}{' ' + detail if detail else ''}")


def run_logged(cmd, log, **kwargs):
    """Run cmd with combined output captured to log.

    Returns the exit code, or None if the run timed out.
    """
    with open(log, "wb") as out:
        try:
            return subprocess.run(
                cmd, stdout=out, stderr=subprocess.STDOUT, **kwargs
            ).returncode
        except subprocess.TimeoutExpired:
            return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mode", required=True, choices=["rtl", "iss"])
    ap.add_argument("--asm", required=True,
                    help="assembler command, e.g. 'pasm --org 0xFFFF0000'")
    ap.add_argument("--hexdir", required=True, type=Path,
                    help="hex + log output directory")
    ap.add_argument("--provides", default="",
                    help="integration capability set, space-separated")
    ap.add_argument("--default-runner",
                    help="rtl: runner for programs without a RUNNER tag")
    ap.add_argument("--bin", action="append", default=[], metavar="NAME=CMD",
                    help="runner name -> run command (repeatable)")
    ap.add_argument("--timeout", type=float, default=5,
                    help="iss run timeout in seconds")
    ap.add_argument("programs", nargs="+", type=Path)
    args = ap.parse_args()

    bins = {}
    for b in args.bin:
        if "=" not in b:
            ap.error(f"--bin expects NAME=CMD, got {b!r}")
        name, cmd = b.split("=", 1)
        bins[name] = cmd

    provides = set(args.provides.split())
    asm = shlex.split(args.asm)
    args.hexdir.mkdir(parents=True, exist_ok=True)

    # Program basenames must be unique across suites: hex/log artifacts
    # and `make test-prog` are keyed by basename, so a duplicate would
    # silently shadow another program's results. Fail loudly instead.
    dups = [n for n, c in Counter(p.stem for p in args.programs).items() if c > 1]
    if dups:
        sys.exit("run-prog-tests.py: duplicate program basename(s) "
                 f"across suites: {' '.join(sorted(dups))}")

    passed, failed, skipped = [], [], []
    for src in args.programs:
        name = src.stem
        log = args.hexdir / f"{name}.log"
        hexfile = args.hexdir / f"{name}.hex"
        tags = scan_tags(src)
        runner = tags["RUNNER"][0] if tags["RUNNER"] else None

        missing = sorted(set(tags["REQUIRES"]) - provides)
        if missing:
            report(33, "SKIP", name, f"(missing: {' '.join(missing)})")
            skipped.append(name)
            continue
        if args.mode == "iss" and runner:
            report(33, "SKIP", name, f"(needs bespoke runner {runner})")
            skipped.append(name)
            continue

        if run_logged(asm + [str(src), "-o", str(hexfile)], log) != 0:
            report(31, "FAIL", name, f"(assembler error — see {log})")
            failed.append(name)
            continue

        if args.mode == "rtl":
            use = runner or args.default_runner
            cmd = bins.get(use)
            if not cmd:
                report(31, "FAIL", name, f"(no binary registered for runner {use})")
                failed.append(name)
                continue
            ok = run_logged(shlex.split(cmd) + [f"+rom_hex={hexfile}"], log) == 0
            detail = "" if ok else f"(see {log})"
        else:
            cmd = bins.get("iss")
            if not cmd:
                sys.exit("run-prog-tests.py: iss mode needs --bin iss=<command>")
            trace = args.hexdir / f"{name}.trace"
            run_logged(shlex.split(cmd) + [str(hexfile), "+halt-on-break",
                                           f"+trace={trace}"],
                       log, input=b"break\n", timeout=args.timeout)
            r1 = None
            if trace.exists() and (lines := trace.read_text().splitlines()):
                m = re.search(r"R1=([0-9a-f]+)", lines[-1])
                r1 = m.group(1) if m else None
            ok = r1 == "00000001"
            detail = "" if ok else (f"(R1={r1})" if r1 else "(no trace)")

        if ok:
            report(32, "PASS", name)
            passed.append(name)
        else:
            report(31, "FAIL", name, detail)
            failed.append(name)

    print()
    print(f"{len(passed)}/{len(passed) + len(failed)} tests passed "
          f"({len(skipped)} skipped)")
    if failed:
        print(f"  *** {len(failed)} FAILED: {' '.join(failed)} ***")
        sys.exit(1)


if __name__ == "__main__":
    main()
