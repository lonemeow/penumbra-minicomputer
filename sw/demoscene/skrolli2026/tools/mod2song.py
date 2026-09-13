#!/usr/bin/env python3
"""Convert a 4-channel ProTracker MOD into the replayer's song blob.

The replayer implements a subset of ProTracker.  Effects outside it are
dropped here and listed on stderr, so what the build leaves out is known
before listening rather than discovered by ear.

Blob layout, little-endian, offsets from the start of the blob:

  0x000  u32  magic "SONG"
  0x004  u8   song length, u8 restart order, u8 pattern count, u8 pad
  0x008  u32  offset of the pattern cells
  0x00C  u8[128] order list
  0x08C  31 x { u32 data, u32 length, u32 loop_start, u32 loop_len,
               u32 volume }        -- byte counts; loop_len 0 is one-shot
  ...    s8 sample data, each sample padded to a word
  ...    u32 cells, 64 rows x 4 tracks per pattern:
         [11:0] period  [19:12] sample  [23:20] effect  [31:24] param
"""

import argparse
import collections
import struct
import sys

MAGIC = b"SONG"
TRACKS = 4
ROWS = 64
SAMPLES = 31

# Effects the replayer implements.  Fxx is split: speeds are kept, and
# the timer runs at the default tempo, so a tempo other than it is fatal.
SUPPORTED = {0x1, 0xA, 0xB, 0xC, 0xF}
DEFAULT_TEMPO = 125

NAMES = {
    0x0: "0xy arpeggio", 0x1: "1xx porta up", 0x2: "2xx porta down",
    0x3: "3xx tone porta", 0x4: "4xy vibrato", 0x5: "5xy tone porta+vol slide",
    0x6: "6xy vibrato+vol slide", 0x7: "7xy tremolo", 0x8: "8xx panning",
    0x9: "9xx sample offset", 0xA: "Axy volume slide", 0xB: "Bxx position jump",
    0xC: "Cxx set volume", 0xD: "Dxx pattern break",
}
EXT_NAMES = {
    0x0: "E0x filter", 0x1: "E1x fine porta up", 0x2: "E2x fine porta down",
    0x3: "E3x glissando", 0x4: "E4x vibrato waveform", 0x5: "E5x finetune",
    0x6: "E6x pattern loop", 0x7: "E7x tremolo waveform", 0x8: "E8x",
    0x9: "E9x retrigger", 0xA: "EAx fine vol up", 0xB: "EBx fine vol down",
    0xC: "ECx note cut", 0xD: "EDx note delay", 0xE: "EEx pattern delay",
    0xF: "EFx invert loop",
}


def fail(msg):
    sys.exit(f"mod2song: {msg}")


def convert(mod):
    if len(mod) < 1084 or mod[1080:1084] not in (b"M.K.", b"M!K!", b"4CHN"):
        fail("not a 4-channel ProTracker module")

    length = mod[950]
    restart = mod[951] if mod[951] < length else 0
    orders = mod[952:1080]
    npatterns = max(orders) + 1
    if not 1 <= length <= 128:
        fail(f"song length {length} out of range")

    # Sample headers, then the data that follows the patterns.
    headers = []
    for i in range(SAMPLES):
        o = 20 + i * 30
        words, finetune, volume, loop_w, looplen_w = struct.unpack(
            ">HBBHH", mod[o + 22:o + 30])
        headers.append((words * 2, finetune & 0xF, min(volume, 64),
                        loop_w * 2, looplen_w * 2))

    pat_base = 1084
    data_base = pat_base + npatterns * ROWS * TRACKS * 4
    if data_base > len(mod):
        fail("truncated pattern data")

    notes = []
    dropped = collections.Counter()

    # Cells.
    cells = []
    for p in range(npatterns):
        for i in range(ROWS * TRACKS):
            o = pat_base + (p * ROWS * TRACKS + i) * 4
            b0, b1, b2, b3 = mod[o:o + 4]
            period = ((b0 & 0x0F) << 8) | b1
            sample = ((b0 & 0xF0) | (b2 >> 4))
            effect, param = b2 & 0x0F, b3
            if sample > SAMPLES:
                fail(f"pattern {p}: sample {sample} out of range")

            if effect == 0xF and param >= 32:
                if param != DEFAULT_TEMPO:
                    fail(f"pattern {p}: tempo {param} (only {DEFAULT_TEMPO})")
                effect = param = 0
            elif effect == 0xF and param == 0:
                dropped["F00 stop"] += 1
                effect = 0
            elif effect == 0xE:
                dropped[EXT_NAMES[param >> 4]] += 1
                effect = param = 0
            elif effect not in SUPPORTED and (effect or param):
                dropped[NAMES[effect]] += 1
                effect = param = 0
            cells.append(period | sample << 12 | effect << 20 | param << 24)

    # Samples: ProTracker plays a looped sample to its loop end, never
    # past it, so that is where the first pass stops.
    table = []
    blob_data = bytearray()
    data_off = 0x8C + SAMPLES * 20
    src = data_base
    finetuned = 0
    for i, (length_b, finetune, volume, loop, looplen) in enumerate(headers):
        raw = mod[src:src + length_b]
        src += length_b
        if len(raw) < length_b:
            fail(f"sample {i + 1} truncated")
        if length_b and finetune:
            finetuned += 1
        if looplen > 2 and loop < length_b:
            end = min(length_b, loop + looplen)
            loop_len = end - loop
        else:
            end, loop, loop_len = length_b, 0, 0
        table.append((data_off + len(blob_data), end, loop, loop_len, volume))
        blob_data += raw[:end]
        blob_data += bytes(-len(blob_data) % 4)

    if finetuned:
        notes.append(f"finetune ignored on {finetuned} samples")

    patterns_off = data_off + len(blob_data)
    out = bytearray()
    out += MAGIC
    out += struct.pack("<BBBBI", length, restart, npatterns, 0, patterns_off)
    out += bytes(orders)
    for entry in table:
        out += struct.pack("<5I", *entry)
    out += blob_data
    out += struct.pack(f"<{len(cells)}I", *cells)

    title = mod[:20].split(b"\0")[0].decode("latin-1")
    report = [f"mod2song: '{title}', {length} orders, {npatterns} patterns,"
              f" {len(blob_data)} bytes of samples"]
    for name, count in sorted(dropped.items()):
        report.append(f"  dropped {name}: {count} cells")
    report += [f"  {n}" for n in notes]
    return bytes(out), report


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mod")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    with open(args.mod, "rb") as f:
        blob, report = convert(f.read())
    with open(args.output, "wb") as f:
        f.write(blob)
    print("\n".join(report), file=sys.stderr)


if __name__ == "__main__":
    main()
