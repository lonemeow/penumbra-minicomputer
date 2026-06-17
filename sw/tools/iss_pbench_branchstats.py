#!/usr/bin/env python3
"""Drive a NetBSD boot under the ISS, run a shell command, capture +branchstats.

The ISS counts branches from reset, so a kernel-workload measurement must window
boot out. This harness does it by *subtraction*: run twice and diff.

  --mode baseline  boot -> single-user shell -> `echo MARKER` -> trap out
  --mode full      boot -> single-user shell -> `<cmd>; echo MARKER` -> trap out

Then:  branch_ceiling.py --baseline boot.txt pbench.txt   (pbench = full - baseline)

Trap-out is a SIGINT sent the moment the marker appears *on its own line*
(`^MARKER$`), so the tty's immediate echo of the command line — which embeds the
marker mid-line — never trips it. The ISS's SIGINT handler stops the run and
prints the +branchstats block on the way out.
"""

import argparse
import os
import re
import select
import signal
import subprocess
import sys
import time

ROM = "program.hex"
ISS = "./sw/sim/penumbra-iss"
MARKER = "ZZQUITZZ"


def drive(mode, image, cmd, out, wall):
    argv = [ISS, ROM, f"+sdcard={image}", "+branchstats", "+quiet"]
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    console = out + ".console.log"
    log = open(console, "wb")
    text = ""
    stage, stage_t = 0, time.monotonic()

    def send(s):
        sys.stderr.write(f"  [send] {s!r}\n")
        proc.stdin.write(s.encode())
        proc.stdin.flush()

    def to_stage(s):
        nonlocal stage, stage_t, text
        stage, stage_t, text = s, time.monotonic(), ""

    shell_cmd = (f"{cmd}; echo {MARKER}\n" if mode == "full" else f"echo {MARKER}\n")
    marker_re = re.compile(rf"(?m)^{MARKER}\r?$")
    deadline = time.monotonic() + wall

    while True:
        if time.monotonic() > deadline:
            sys.stderr.write("  [harness] WALL TIMEOUT — interrupting\n")
            proc.send_signal(signal.SIGINT)
            break
        # Fallback: if the shell prompt never matches, send the command anyway.
        if stage == 2 and time.monotonic() - stage_t > 6:
            sys.stderr.write("  [harness] no '#' prompt seen — sending command on fallback\n")
            send(shell_cmd)
            to_stage(3)
        if not select.select([proc.stdout], [], [], 1.0)[0]:
            continue
        chunk = os.read(proc.stdout.fileno(), 65536)
        if not chunk:
            break  # EOF
        log.write(chunk); log.flush()
        text += chunk.decode(errors="replace")

        if stage == 0 and "Boot data:" in text:
            send("boot sd:0,0\n"); to_stage(1)
        elif stage == 1 and "RETURN for /bin/sh" in text:
            send("\n"); to_stage(2)               # RETURN -> /bin/sh
        elif stage == 2 and re.search(r"#\s*$", text):
            send(shell_cmd); to_stage(3)          # shell prompt -> run workload
        elif stage == 3 and marker_re.search(text):
            sys.stderr.write("  [harness] marker on its own line — trapping out\n")
            proc.send_signal(signal.SIGINT)
            break

    try:
        rest = proc.stdout.read()              # drain the +branchstats dump
        if rest:
            log.write(rest)
    except Exception:
        pass
    log.close()
    proc.wait()

    blob = open(console).read()
    m = re.search(r"── branch statistics.*?register/indirect\s+\d+", blob, re.S)
    if not m:
        sys.stderr.write(f"  [harness] NO +branchstats block captured (mode={mode}); see {console}\n")
        return 1
    with open(out, "w") as f:
        f.write(m.group(0) + "\n")
    sys.stderr.write(f"  [harness] wrote {out}\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["baseline", "full"], required=True)
    ap.add_argument("--image", default="build/boot.img")
    ap.add_argument("--cmd", default="/usr/local/bin/pbench",
                    help="workload command run at the shell in --mode full")
    ap.add_argument("--out", required=True, help="path for the extracted +branchstats block")
    ap.add_argument("--wall", type=int, default=1200, help="wall-clock timeout (s)")
    a = ap.parse_args()
    return drive(a.mode, a.image, a.cmd, a.out, a.wall)


if __name__ == "__main__":
    sys.exit(main())
