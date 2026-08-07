#!/usr/bin/env python3
"""Decode a Penumbra USBHC line-capture dump.

Input: the console dump from pusbhc ("%04x" samples, 16 per line,
between the "line capture frozen, trig=T depth=D" header and the
"capture end" trailer) — pass the captured console text on stdin or
as a file argument; the header is parsed for the trigger index and
non-sample lines are skipped.  Sample layout: CAP_DATA in
doc/system/devices/usb-host.md.

Output: a run-compressed timeline of the receive chain (line state,
strobes, decoded bits), the recovered bit stream, and the assembled
bytes, with the trigger point marked.
"""

import re
import sys

LINE_NAMES = ["SE0", "J", "K", "SE1"]

F_DP, F_DN = 0, 1
F_BIT_EN, F_BIT, F_DATA_BIT, F_ACTIVE = 4, 5, 6, 7
F_SYNC_DONE, F_PAYLOAD_EN, F_UNSTUFF_VALID, F_STUFF_ERR = 8, 9, 10, 11
F_BYTE_VALID, F_TX_OE, F_SQUELCH = 12, 13, 14


def bit(s, n):
    return (s >> n) & 1


def line_of(s):
    return (s >> 2) & 3


def main():
    text = (open(sys.argv[1]).read() if len(sys.argv) > 1
            else sys.stdin.read())

    m = re.search(r"line capture frozen, trig=(\d+) depth=(\d+)", text)
    trig = int(m.group(1)) if m else None
    # Scan samples only after the header line — its trig/depth numbers
    # would otherwise match the hex-word pattern and shift the stream.
    scan = text[m.end():] if m else text

    samples = []
    for tok in re.findall(r"\b[0-9a-fA-F]{4}\b", scan):
        samples.append(int(tok, 16))
    if m:
        depth = int(m.group(2))
        samples = samples[:depth]
    n = len(samples)
    if n == 0:
        sys.exit("no samples found")

    # Unroll the ring: the oldest sample is one past the write pointer,
    # which stopped POST samples after the trigger.  Without a header,
    # assume the dump is already oldest-first.
    if trig is not None:
        post = n // 8
        newest = (trig + post) % n
        order = [(newest + 1 + i) % n for i in range(n)]
        trig_pos = order.index(trig)
    else:
        order = list(range(n))
        trig_pos = None

    # Run-compressed timeline: one row per change in the level fields,
    # with strobe events (bit_en / byte_valid / sync_done / stuff_err)
    # listed as they occur inside the run.
    print(f"{n} samples"
          + (f", trigger at timeline offset {trig_pos}" if trig_pos
             is not None else ""))
    print(f"{'idx':>6} {'len':>5}  line dp dn act sq oe  events")

    bits = []          # (index, data_bit) at each bit_en
    bytes_out = []     # (index, assembled) at each byte_valid — value
                       # not in the sample; count and place only
    run_start = 0

    def levels(s):
        return (line_of(s), bit(s, F_DP), bit(s, F_DN), bit(s, F_ACTIVE),
                bit(s, F_SQUELCH), bit(s, F_TX_OE))

    def flush(start, end, s, events):
        ln, dp, dn, act, sq, oe = levels(s)
        marks = []
        if trig_pos is not None and start <= trig_pos < end:
            marks.append("<TRIGGER>")
        marks.extend(events)
        print(f"{start:>6} {end - start:>5}  {LINE_NAMES[ln]:<4} "
              f"{dp}  {dn}  {act}   {sq}  {oe}  {' '.join(marks)}")

    events = []
    for pos in range(n):
        s = samples[order[pos]]
        if pos > 0 and levels(s) != levels(samples[order[pos - 1]]):
            flush(run_start, pos, samples[order[pos - 1]], events)
            run_start, events = pos, []
        if bit(s, F_BIT_EN):
            b = bit(s, F_DATA_BIT)
            bits.append((pos, b))
            tags = "" + ("P" if bit(s, F_PAYLOAD_EN) else "") \
                      + ("V" if bit(s, F_UNSTUFF_VALID) else "")
            events.append(f"bit={b}{('/' + tags) if tags else ''}")
        if bit(s, F_SYNC_DONE):
            events.append("SYNC_DONE")
        if bit(s, F_STUFF_ERR):
            events.append("STUFF_ERR")
        if bit(s, F_BYTE_VALID):
            bytes_out.append(pos)
            events.append("BYTE")
    flush(run_start, n, samples[order[n - 1]], events)

    # The recovered bit stream, and bytes assembled from the payload
    # bits (LSB first, as the deserializer does).
    print(f"\n{len(bits)} recovered bits, {len(bytes_out)} byte strobes")
    payload = [b for pos, b in bits
               if bit(samples[order[pos]], F_UNSTUFF_VALID)]
    out, cur = [], []
    for b in payload:
        cur.append(b)
        if len(cur) == 8:
            out.append(sum(v << i for i, v in enumerate(cur)))
            cur = []
    if out:
        print("payload bytes:",
              " ".join(f"{v:02x}" for v in out)
              + (f"  (+{len(cur)} residual bits)" if cur else ""))

    # Our own transmissions, reconstructed from the raw pins (the
    # squelch hides them from the decoded chain).  Full-speed polarity:
    # J = D+ high.  Symbols quantize at the nominal 5 clocks per bit.
    for start, end in tx_regions(samples, order):
        disasm_tx(samples, order, start, end)


PID_NAMES = {0x1: "OUT", 0x9: "IN", 0x5: "SOF", 0xD: "SETUP",
             0x3: "DATA0", 0xB: "DATA1", 0x2: "ACK", 0xA: "NAK",
             0xE: "STALL", 0xC: "PRE"}


def tx_regions(samples, order):
    regions, start = [], None
    for pos in range(len(order)):
        oe = bit(samples[order[pos]], F_TX_OE)
        if oe and start is None:
            start = pos
        elif not oe and start is not None:
            regions.append((start, pos))
            start = None
    if start is not None:
        regions.append((start, len(order)))
    return regions


def disasm_tx(samples, order, start, end):
    # Symbol stream from the raw pins: J/K by D+, SE0 ends the packet.
    CPB = 5
    levels = []
    for pos in range(start, end):
        s = samples[order[pos]]
        dp, dn = bit(s, F_DP), bit(s, F_DN)
        levels.append("0" if (dp == 0 and dn == 0) else
                      "J" if dp else "K")
    # Run-length quantize to bits, NRZI-decode against the previous
    # level (idle J before the packet).
    bits, prev, i = [], "J", 0
    while i < len(levels):
        lv = levels[i]
        n = 1
        while i + n < len(levels) and levels[i + n] == lv:
            n += 1
        if lv == "0":
            if n >= 3:
                break                   # real EOP (two bit times of SE0)
            i += n                      # transition skew sliver: skip
            continue
        nbits = max(1, round(n / CPB))
        bits.append(0)                  # the transition itself
        bits.extend([1] * (nbits - 1))  # held level: 1s
        if prev == lv:                  # no transition: all 1s
            bits[-nbits] = 1
        prev = lv
        i += n
    # Strip leading idle (decodes as 1s), then SYNC (0s ended by its
    # terminating 1), unstuff, split into bytes.
    try:
        first0 = bits.index(0)
        sync_end = bits.index(1, first0)
    except ValueError:
        return
    body, ones = [], 1
    for b in bits[sync_end + 1:]:
        if ones == 6:
            ones = 0
            if b:
                body.append(None)       # stuff violation marker
            continue
        body.append(b)
        ones = ones + 1 if b else 0
    if None in body:
        print(f"tx@{start}: stuff violation in our own stream")
        return
    byts = [sum(v << k for k, v in enumerate(body[j:j + 8]))
            for j in range(0, len(body) - 7, 8)]
    if not byts:
        return
    pid = byts[0] & 0xf
    ok = ((byts[0] >> 4) ^ 0xf) == pid
    desc = PID_NAMES.get(pid, f"pid{pid:x}")
    detail = ""
    if desc in ("OUT", "IN", "SETUP", "SOF") and len(byts) >= 3:
        fld = byts[1] | (byts[2] << 8)
        if desc == "SOF":
            detail = f" frame={fld & 0x7ff}"
        else:
            detail = f" addr={fld & 0x7f} ep={(fld >> 7) & 0xf}"
    print(f"tx@{start}..{end}: {desc}{'' if ok else ' (BAD PID CHECK)'}"
          f"{detail}  bytes: " + " ".join(f"{v:02x}" for v in byts))


if __name__ == "__main__":
    main()
