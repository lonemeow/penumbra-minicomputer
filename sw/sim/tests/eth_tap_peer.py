#!/usr/bin/env python3
"""The other station for the eth_tap programs: run the ISS and exchange
frames with it.

Runs inside a user and network namespace of its own (run_iss_tests.py
starts it under `unshare -rn`), where it may make a TAP device.  It
makes one, starts the ISS given on its command line with that device as
its Ethernet, waits for the guest's frame, checks it, and answers with
a frame shorter than a wire carries: once to a guest that sent "PENU",
and BURST times back to back to one that sent "BRST".  The ISS's output
is passed on, so the caller reads the guest's registers as it would
from a plain run.

Usage: eth_tap_peer.py <penumbra-iss> <program.hex> [plusargs...]
"""

import socket
import subprocess
import sys

TAP = "tap0"
ETH_P_ALL = 0x0003
EXPERIMENTAL = b"\x88\xb5"
GUEST = bytes.fromhex("0250454e0001")
PEER = bytes.fromhex("020000000002")
BROADCAST = b"\xff" * 6
# How many answers each payload the guest may send asks for.  The burst
# is twice the four slots run_iss_tests.py gives the receive ring.
BURST = 8
ANSWERS = {b"PENU": 1, b"BRST": BURST}
# 18 bytes: the backend has to pad it before the device will take it.
ANSWER = GUEST + PEER + EXPERIMENTAL + b"PEER"
WAIT_S = 20


def main():
    iss = sys.argv[1:]
    for cmd in (["ip", "tuntap", "add", "dev", TAP, "mode", "tap"],
                # Keep the host's own neighbour discovery off the wire.
                ["sysctl", "-qw", f"net.ipv6.conf.{TAP}.disable_ipv6=1"],
                ["ip", "link", "set", TAP, "up"]):
        subprocess.run(cmd, check=True)

    wire = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    wire.bind((TAP, 0))
    wire.settimeout(WAIT_S)

    guest = subprocess.Popen(iss, stdin=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    problem = None
    try:
        while True:
            frame = wire.recv(2048)
            if frame[6:12] == GUEST and frame[12:14] == EXPERIMENTAL:
                break
        # The guest's frame is the 60-byte minimum, zero past its payload.
        payload = frame[14:18]
        expected = (BROADCAST + GUEST + EXPERIMENTAL + payload).ljust(60, b"\0")
        if payload not in ANSWERS or frame != expected:
            problem = f"guest sent {frame.hex()}"
        for _ in range(ANSWERS.get(payload, 0)):
            wire.send(ANSWER)
    except socket.timeout:
        problem = f"no frame from the guest within {WAIT_S} s"
    try:
        _, err = guest.communicate(timeout=WAIT_S)
    except subprocess.TimeoutExpired:
        guest.kill()
        _, err = guest.communicate()
        problem = problem or f"the ISS did not stop within {WAIT_S} s"
    sys.stderr.write(err)
    if problem:
        sys.exit(f"eth_tap_peer: {problem}")
    sys.exit(guest.returncode)


if __name__ == "__main__":
    main()
