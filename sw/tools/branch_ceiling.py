#!/usr/bin/env python3
"""branch_ceiling.py — branch-prediction flush-recovery estimate from ISS +branchstats.

The Penumbra ISS (`+branchstats`) reports raw, microarchitecture-independent
branch counts: conditional PC-relative branches split by displacement sign and
outcome, unconditional PC-relative branches, and register/indirect branches.
The ISS deliberately stops there — it models no pipeline or predictor.

This script applies a *front-end-specific* cost model on top of those counts to
estimate how much of the taken-branch flush penalty a static BTFNT
(backward-taken / forward-not-taken) predictor would recover. Because the model
lives here and is parameterized, it tracks whatever front-end depth you land on
(e.g. as the IF2/ID fetch FIFO firms up) without touching the simulator.

Cost model (per branch, in flush bubbles)
-----------------------------------------
  penalty P            taken-branch flush cost today (static-not-taken,
                       resolved in EX). gen2 today: 3.
  predicted-residual R bubbles still lost on a *correctly* predicted-taken
                       branch (0 if the predicted redirect is fully hidden by
                       the front-end / fetch FIFO, 1 for a one-cycle
                       predict-to-fetch turnaround).

BTFNT predicts: conditional backward = taken, conditional forward = not-taken,
unconditional PC-relative = taken (target is the known displacement),
register/indirect = cannot be redirected statically (that is a BTB's job).

  bucket                today   BTFNT    effect
  cond bwd taken          P       R      recovered
  cond bwd not-taken      0       P      regression (loop-exit mispredict)
  cond fwd taken          P       P      residual (mispredicted either way)
  cond fwd not-taken      0       0      unchanged
  uncond PC-rel           P       R      recovered
  register/indirect       P       P      residual — BTB opportunity, not BTFNT

Two properties worth knowing:
  * With R = 0 the recovered *fraction* is independent of P (P cancels); P and R
    only scale the absolute bubble counts.
  * "today" (= P x taken) is also a prediction of the hardware STALL_FLUSH
    perfctr for the same workload — a cross-check once it is measured on FPGA.
"""

import argparse
import os
import re
import sys
from dataclasses import dataclass


@dataclass
class Counts:
    """Raw +branchstats buckets for one workload."""
    insns: int = 0
    bwd_taken: int = 0
    bwd_nottaken: int = 0
    fwd_taken: int = 0
    fwd_nottaken: int = 0
    uncond: int = 0
    indirect: int = 0

    @property
    def taken(self) -> int:
        return self.bwd_taken + self.fwd_taken + self.uncond + self.indirect

    @property
    def total(self) -> int:
        return self.taken + self.bwd_nottaken + self.fwd_nottaken

    def __add__(self, other: "Counts") -> "Counts":
        return Counts(
            self.insns + other.insns,
            self.bwd_taken + other.bwd_taken,
            self.bwd_nottaken + other.bwd_nottaken,
            self.fwd_taken + other.fwd_taken,
            self.fwd_nottaken + other.fwd_nottaken,
            self.uncond + other.uncond,
            self.indirect + other.indirect,
        )

    def __sub__(self, other: "Counts") -> "Counts":
        s = lambda a, b: max(0, a - b)  # clamp: boot nondeterminism can't go negative
        return Counts(
            s(self.insns, other.insns),
            s(self.bwd_taken, other.bwd_taken),
            s(self.bwd_nottaken, other.bwd_nottaken),
            s(self.fwd_taken, other.fwd_taken),
            s(self.fwd_nottaken, other.fwd_nottaken),
            s(self.uncond, other.uncond),
            s(self.indirect, other.indirect),
        )


_PAT = {
    "insns":    re.compile(r"instructions retired\s+(\d+)"),
    "bwd":      re.compile(r"cond backward\s+T/NT\s+(\d+)\s*/\s*(\d+)"),
    "fwd":      re.compile(r"cond forward\s+T/NT\s+(\d+)\s*/\s*(\d+)"),
    "uncond":   re.compile(r"uncond PC-rel[^\d]*(\d+)"),
    "indirect": re.compile(r"register/indirect\s+(\d+)"),
}


def parse(text: str):
    """Pull one +branchstats block out of an ISS output blob, or None if absent."""
    c = Counts()
    m = _PAT["bwd"].search(text)
    if not m:
        return None  # no branch block here
    c.bwd_taken, c.bwd_nottaken = int(m.group(1)), int(m.group(2))
    m = _PAT["fwd"].search(text)
    if m:
        c.fwd_taken, c.fwd_nottaken = int(m.group(1)), int(m.group(2))
    for key, attr in (("insns", "insns"), ("uncond", "uncond"), ("indirect", "indirect")):
        m = _PAT[key].search(text)
        if m:
            setattr(c, attr, int(m.group(1)))
    return c


@dataclass
class Ceiling:
    today: int            # flush bubbles today (static-not-taken)
    btfnt: int            # flush bubbles remaining under BTFNT
    net_saved: int        # today - btfnt
    regressed: int        # new loop-exit mispredict bubbles
    btb_residual: int     # indirect-branch bubbles BTFNT cannot reach

    @property
    def recov_frac(self) -> float:
        return self.net_saved / self.today if self.today else 0.0


def ceiling(c: Counts, P: int, R: int) -> Ceiling:
    today = P * c.taken
    btfnt = R * (c.bwd_taken + c.uncond) + P * (c.bwd_nottaken + c.fwd_taken + c.indirect)
    return Ceiling(
        today=today,
        btfnt=btfnt,
        net_saved=today - btfnt,
        regressed=P * c.bwd_nottaken,
        btb_residual=P * c.indirect,
    )


def _pct(a: int, b: int) -> float:
    return 100.0 * a / b if b else 0.0


def print_table(rows, P: int, R: int) -> None:
    hdr = ("workload", "insns", "br%", "taken%",
           "today", "BTFNT", "recov%", "regress", "BTB-resid")
    widths = (16, 10, 6, 7, 10, 10, 7, 9, 10)

    def fmt(cells):
        return "  ".join(str(c).rjust(w) if i else str(c).ljust(w)
                          for i, (c, w) in enumerate(zip(cells, widths)))

    print(f"\nbranch-flush ceiling   penalty={P}  predicted-residual={R}\n")
    print("  " + fmt(hdr))
    print("  " + "-" * (sum(widths) + 2 * (len(widths) - 1)))

    def row(label, c):
        cl = ceiling(c, P, R)
        return (label, c.insns, f"{_pct(c.total, c.insns):.1f}",
                f"{_pct(c.taken, c.total):.1f}", cl.today, cl.btfnt,
                f"{cl.recov_frac * 100:.1f}", cl.regressed, cl.btb_residual)

    agg = Counts()
    for label, c in rows:
        print("  " + fmt(row(label, c)))
        agg = agg + c

    if len(rows) > 1:
        print("  " + "-" * (sum(widths) + 2 * (len(widths) - 1)))
        print("  " + fmt(row("(all)", agg)))

    print("\n  today = P x taken (predicts the STALL_FLUSH perfctr); "
          "recov% is P-independent when predicted-residual=0.")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Estimate BTFNT flush recovery from ISS +branchstats output.")
    ap.add_argument("files", nargs="+",
                    help="captured ISS +branchstats output (one run per file; '-' = stdin)")
    ap.add_argument("--penalty", "-P", type=int, default=3,
                    help="taken-branch flush penalty in bubbles (default 3)")
    ap.add_argument("--predicted-residual", "-R", type=int, default=0,
                    help="residual bubbles on a correctly predicted-taken branch (default 0)")
    ap.add_argument("--baseline", metavar="FILE",
                    help="a +branchstats capture to subtract from each input "
                         "(e.g. a boot-only run, to window boot out of a boot+workload run)")
    args = ap.parse_args()

    base = None
    if args.baseline:
        with open(args.baseline) as fh:
            base = parse(fh.read())
        if base is None:
            print(f"baseline {args.baseline}: no +branchstats block", file=sys.stderr)
            return 1

    rows = []
    for f in args.files:
        if f == "-":
            text, label = sys.stdin.read(), "stdin"
        else:
            with open(f) as fh:
                text = fh.read()
            label = os.path.splitext(os.path.basename(f))[0]
        c = parse(text)
        if c is None:
            print(f"  {label}: no +branchstats block found", file=sys.stderr)
            continue
        if base is not None:
            c = c - base
        rows.append((label, c))

    if not rows:
        print("no parseable +branchstats blocks", file=sys.stderr)
        return 1

    print_table(rows, args.penalty, args.predicted_residual)
    return 0


if __name__ == "__main__":
    sys.exit(main())
