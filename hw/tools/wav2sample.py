#!/usr/bin/env python3
"""Convert a WAV file into a sample image the build can embed.

Resamples to the device rate, folds to the requested channel count and
scales to 16-bit signed. The image plays as a loop, so the ends are
faded to avoid a click at the wrap.

Two output formats: `hex` feeds $readmemh in the FPGA probe's sample
ROM, `bin` feeds .incbin in a bare-metal program. Both carry
interleaved little-endian frames; the hex image is one 16-bit word per
line because that is what $readmemh reads.

With no input file it synthesizes a decaying arpeggio, so everything
builds without anyone having to find source material. Decaying notes
are deliberate test material: the tail runs down through the codes a
converter idles around, which is where a resistor ladder's error is the
largest fraction of the signal.
"""

import argparse
import math
import struct
import sys
import wave


def decode(raw, width):
    """Unpack PCM bytes to floats in [-1, 1), still interleaved."""
    if width == 1:      # unsigned
        return [(b - 128) / 128.0 for b in raw]
    if width == 2:
        return [v / 32768.0 for v in struct.unpack(f"<{len(raw)//2}h", raw)]
    if width == 3:
        vals = []
        for i in range(0, len(raw), 3):
            v = raw[i] | (raw[i+1] << 8) | (raw[i+2] << 16)
            if v & 0x800000:
                v -= 1 << 24
            vals.append(v / 8388608.0)
        return vals
    if width == 4:
        return [v / 2147483648.0 for v in struct.unpack(f"<{len(raw)//4}i", raw)]
    sys.exit(f"unsupported sample width {width}")


def resample(vals, srate, rate):
    """Linear interpolation is enough for listening material; the probe's
    measurement stimulus is synthesized in hardware, not resampled here."""
    if srate == rate:
        return vals
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


def read_wav(path, rate):
    """Read a WAV as one float sample list per channel."""
    with wave.open(path, 'rb') as w:
        nch, width, srate, nframes = (w.getnchannels(), w.getsampwidth(),
                                      w.getframerate(), w.getnframes())
        raw = w.readframes(nframes)

    vals = decode(raw, width)
    return [resample(vals[c::nch], srate, rate) for c in range(nch)]


def fold(chans, want):
    """Fold or spread source channels to the requested count."""
    if want == len(chans):
        return chans
    if want == 1:
        n = min(len(c) for c in chans)
        return [[sum(c[i] for c in chans) / len(chans) for i in range(n)]]
    if len(chans) == 1:
        return [chans[0]] * want
    return chans[:want]


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
    return [[v / peak for v in out]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input', nargs='?', help='WAV file; synthesized if omitted')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('-n', '--count', type=int, default=1 << 17,
                    help='image length in frames')
    ap.add_argument('-r', '--rate', type=int, default=48000)
    ap.add_argument('-c', '--channels', type=int, default=1, choices=(1, 2))
    ap.add_argument('-f', '--format', default='hex', choices=('hex', 'bin'))
    ap.add_argument('-l', '--level', type=float, default=-6.0,
                    help='peak level in dBFS')
    args = ap.parse_args()

    chans = read_wav(args.input, args.rate) if args.input \
        else synth(args.rate, args.count)
    chans = fold(chans, args.channels)

    # A ROM has a fixed depth and must be padded to it; a linked blob is
    # only as long as the material, so -n is a ceiling there and short
    # source does not become trailing silence.
    if args.format == 'hex':
        count = args.count
        chans = [c[:count] + [0.0] * max(0, count - len(c)) for c in chans]
    else:
        count = min(args.count, min(len(c) for c in chans))
        chans = [c[:count] for c in chans]

    if count == 0:
        sys.exit("no samples to write")

    peak = max(max(abs(v) for v in c) for c in chans) or 1.0
    gain = (10 ** (args.level / 20.0)) / peak

    # Fade the ends into each other; the image plays as a loop and a
    # step at the wrap is a click on every pass.
    fade = min(int(args.rate * 0.01), count // 4)

    frames = []
    for i in range(count):
        g = gain
        if i < fade:
            g *= i / fade
        elif i >= count - fade:
            g *= (count - i) / fade
        for c in chans:
            frames.append(max(-32768, min(32767, int(round(c[i] * g * 32767)))))

    if args.format == 'hex':
        with open(args.output, 'w') as f:
            f.writelines(f"{s & 0xFFFF:04X}\n" for s in frames)
    else:
        with open(args.output, 'wb') as f:
            f.write(struct.pack(f"<{len(frames)}h", *frames))

    src = args.input if args.input else 'synthesized arpeggio'
    print(f"wav2sample: {src} -> {args.output} "
          f"({count} frames, {args.channels} ch, "
          f"{count/args.rate:.2f} s at {args.rate} Hz)")


if __name__ == '__main__':
    main()
