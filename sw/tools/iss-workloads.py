#!/usr/bin/env python3
"""Run NetBSD workloads under the ISS, unattended.

Boots an SD image from the ROM monitor, reaches a root shell -- the
multiuser login or the single-user prompt, whichever the image stops at
-- and runs each workload command in turn, then ends the run.

The modelled UART holds one byte, so input fed faster than the guest
reads it is lost: every character is typed only after the previous one
has echoed.  With --windows, each workload gets its own model window
(+cachemodel and friends): the ISS is signalled just before the command
is entered and again the moment it finishes, and the driver says which
window belongs to which workload.

Example:

    sw/tools/iss-workloads.py --sdcard build/image-copy.img \\
        --iss-arg +core=penumbra2_5 --iss-arg +cachemodel \\
        --iss-arg +model_report=build/cache.report --windows \\
        '/usr/local/bin/pbench kernel'

The run writes to the image like a real boot does; drive a copy when the
image must stay pristine.
"""

import argparse
import os
import pty
import select
import signal
import sys
import time

MONITOR_PROMPT = b"> "
SHELL_PROMPT = b"# "
DONE_MARKER = "ISS-WORKLOAD-DONE"


class Console:
    """The ISS's console through a pty: expect output, type input."""

    def __init__(self, argv, log):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(argv[0], argv)
        self.log, self.buf = log, b""

    def pump(self, timeout):
        """Read what is available; False once the ISS has gone."""
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return True
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            return False
        if not data:
            return False
        self.buf += data
        self.log.write(data)
        self.log.flush()
        return True

    def expect(self, patterns, timeout):
        """Wait for the first of `patterns`; return its index."""
        deadline = time.time() + timeout
        while True:
            hits = [(self.buf.find(p), i) for i, p in enumerate(patterns) if p in self.buf]
            if hits:
                pos, i = min(hits)
                self.buf = self.buf[pos + len(patterns[i]):]
                return i
            if time.time() > deadline:
                sys.exit(f"iss-workloads: timed out waiting for {patterns}; "
                         f"last output: {self.buf[-200:]!r}")
            if not self.pump(1.0):
                sys.exit("iss-workloads: the ISS exited")

    def type(self, text, echo_timeout=30):
        """Type `text` a character at a time, each after the last echoed."""
        for ch in text.encode():
            os.write(self.fd, bytes([ch]))
            deadline = time.time() + echo_timeout
            while bytes([ch]) not in self.buf[-8:]:
                if time.time() > deadline:
                    sys.exit(f"iss-workloads: no echo of {chr(ch)!r}")
                self.pump(0.2)
        self.buf = b""

    def enter(self):
        os.write(self.fd, b"\r")

    def signal(self, sig):
        os.kill(self.pid, sig)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[2:]))
    ap.add_argument("--iss", default="sw/sim/penumbra-iss")
    ap.add_argument("--rom", default="program.hex", help="boot ROM image (make simulate builds it)")
    ap.add_argument("--sdcard", required=True)
    ap.add_argument("--iss-arg", action="append", default=[], help="extra ISS option, repeatable")
    ap.add_argument("--boot", default="boot sd:0,0", help="ROM monitor command")
    ap.add_argument("--user", default="root", help="login name at a multiuser prompt")
    ap.add_argument("--windows", action="store_true",
                    help="give each workload its own model window (SIGUSR1)")
    ap.add_argument("--timeout", type=int, default=7200, help="seconds allowed per step")
    ap.add_argument("--log", default="iss-workloads.log", help="console transcript")
    ap.add_argument("workloads", nargs="+", help="shell commands, one per workload")
    args = ap.parse_args()

    with open(args.log, "wb") as log:
        con = Console([args.iss, args.rom, f"+sdcard={args.sdcard}"] + args.iss_arg, log)
        con.expect([MONITOR_PROMPT], 120)
        con.type(args.boot)
        con.enter()
        # A multiuser image asks for a login; one whose rc is not
        # configured drops to the single-user prompt instead.
        if con.expect([b"login:", b"RETURN for /bin/sh:"], args.timeout) == 0:
            con.type(args.user)
        con.enter()
        con.expect([SHELL_PROMPT], args.timeout)

        window = 1
        for n, cmd in enumerate(args.workloads, 1):
            # Split the marker so the echoed command line cannot match it.
            con.type(f'{cmd}; echo {DONE_MARKER[:4]}""{DONE_MARKER[4:]}')
            if args.windows:
                con.signal(signal.SIGUSR1)
                window += 1
            start = time.time()
            con.enter()
            con.expect([DONE_MARKER.encode()], args.timeout)
            if args.windows:
                con.signal(signal.SIGUSR1)
            took = time.time() - start
            where = f" -> model window {window}" if args.windows else ""
            print(f"workload {n}: {took:.0f} s{where}: {cmd}", flush=True)
            if args.windows:
                window += 1
            con.expect([SHELL_PROMPT], args.timeout)

        con.signal(signal.SIGINT)
        deadline = time.time() + 30
        while time.time() < deadline and con.pump(1.0):
            pass


if __name__ == "__main__":
    main()
