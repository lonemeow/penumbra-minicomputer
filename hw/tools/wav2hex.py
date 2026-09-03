#!/usr/bin/env python3
"""Convert a WAV file to a $readmemh image for the audio probe's sample ROM.

Mixes to mono, resamples to the device rate, scales to 16-bit signed and
pads or truncates to the ROM size. The clip plays as a loop, so the ends
are faded to avoid a click at the wrap.

With no input file it synthesizes a decaying arpeggio instead, so the
probe builds without anyone having to find source material. Decaying
notes are deliberate test material: the tail runs down through the codes
a converter idles around, which is where a resistor ladder's error is
the largest fraction of the signal.
"""

import argparse
import math
import struct
import sys
import wave


def read_wav(path, rate):
    """Read a WAV as mono float samples at the requested rate."""
    with wave.open(path, 'rb') as w:
        nch, width, srate, nframes = (w.getnchannels(), w.getsampwidth(),
                                      w.getframerate(), w.getnframes())
        raw = w.readframes(nframes)

    if width == 1:      # unsigned
        vals = [(b - 128) / 128.0 for b in raw]
    elif width == 2:
        vals = [v / 32768.0 for v in struct.unpack(f"<{len(raw)//2}h", raw)]
    elif width == 3:
        vals = []
        for i in range(0, len(raw), 3):
            v = raw[i] | (raw[i+1] << 8) | (raw[i+2] << 16)
            if v & 0x800000:
                v -= 1 << 24
            vals.append(v / 8388608.0)
    elif width == 4:
        vals = [v / 2147483648.0 for v in struct.unpack(f"<{len(raw)//4}i", raw)]
    else:
        sys.exit(f"{path}: unsupported sample width {width}")

    if nch > 1:
        vals = [sum(vals[i:i+nch]) / nch for i in range(0, len(vals) - nch + 1, nch)]

    if srate == rate:
        return vals

    # Linear interpolation is enough for listening material; the probe's
    # measurement stimulus is synthesized in hardware, not resampled here.
    ratio = srate / rate
    out = []
    for i in range(int(len(vals) / ratio)):
        pos = i * ratio
        k = int(pos)
        frac = pos - k
        a = vals[k]
        b = vals[k + 1] if k + 1 < len(vals) else a
        out.append(a + (b - a) * frac)
    return out


def synth(rate, count):
    """A decaying arpeggio: harmonically rich, with a long tail."""
    # Two bars of an Am - F - C - G progression, four notes each.
    chords = [(220.00, 261.63, 329.63), (174.61, 220.00, 261.63),
              (130.81, 164.81, 196.00), (196.00, 246.94, 293.66)]
    out = [0.0] * count
    note = int(rate * 0.22)
    idx = 0
    ci = 0
    while idx < count:
        for step in range(4):
            f = chords[ci % len(chords)][step % 3] * (2.0 if step == 3 else 1.0)
            # A few partials with per-partial decay gives a plucked
            # character and puts energy across the band.
            for h, amp in ((1, 1.0), (2, 0.45), (3, 0.22), (4, 0.11), (6, 0.05)):
                w = 2 * math.pi * f * h / rate
                decay = math.exp(-3.0 * h / (rate * 0.6))
                a = amp
                for n in range(note):
                    if idx + n >= count:
                        break
                    out[idx + n] += a * math.sin(w * n)
                    a *= decay
            idx += note
            if idx >= count:
                break
        ci += 1
    peak = max(abs(v) for v in out) or 1.0
    return [v / peak for v in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input', nargs='?', help='WAV file; synthesized if omitted')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('-n', '--count', type=int, default=1 << 17,
                    help='ROM depth in samples')
    ap.add_argument('-r', '--rate', type=int, default=48000)
    ap.add_argument('-l', '--level', type=float, default=-6.0,
                    help='peak level in dBFS')
    args = ap.parse_args()

    vals = read_wav(args.input, args.rate) if args.input else synth(args.rate, args.count)

    vals = vals[:args.count]
    vals += [0.0] * (args.count - len(vals))

    peak = max(abs(v) for v in vals) or 1.0
    gain = (10 ** (args.level / 20.0)) / peak

    # Fade the ends into each other; the ROM plays as a loop and a step
    # at the wrap is a click on every pass.
    fade = min(int(args.rate * 0.01), args.count // 4)
    with open(args.output, 'w') as f:
        for i, v in enumerate(vals):
            g = gain
            if i < fade:
                g *= i / fade
            elif i >= args.count - fade:
                g *= (args.count - i) / fade
            s = max(-32768, min(32767, int(round(v * g * 32767))))
            f.write(f"{s & 0xFFFF:04X}\n")

    src = args.input if args.input else 'synthesized arpeggio'
    print(f"wav2hex: {src} -> {args.output} "
          f"({args.count} samples, {args.count/args.rate:.2f} s at {args.rate} Hz)")


if __name__ == '__main__':
    main()
