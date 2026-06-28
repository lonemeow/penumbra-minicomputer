#!/usr/bin/env python3
"""USB CRC reference model — the readable spec for the hardware LFSRs.

Two layers are kept deliberately separate, because the hardware splits them
across two modules:

  * the bare LFSR *remainder* — what ``hw/rtl/io/usb/usb_crc5.sv`` outputs, and
  * the full on-wire USB CRC — the remainder, then 1's-complement and a
    bit-reverse, which the MAC forms above the bare module.

``crc5_remainder`` mirrors the RTL step for step, so the testbench can check
the module against it. ``token_crc5`` adds the framing for when the MAC's own
test needs a trusted on-wire value. Run this file directly to print a vector
table.
"""

CRC5_POLY = 0x05          # x^5 + x^2 + 1 tap mask (x^5 is the shifted-out bit)
CRC5_SEED = 0x1F          # USB seeds the remainder to all-ones
CRC5_WIDTH = 5


def crc5_remainder(bits):
    """Left-shift LFSR remainder over ``bits``, fed in order (bits[0] first).

    Identical to usb_crc5.sv: the feedback bit is the top register bit XORed
    with the incoming bit; the register shifts up one each step; on feedback
    the polynomial taps are XORed back in.
    """
    crc = CRC5_SEED
    top = 1 << (CRC5_WIDTH - 1)
    mask = (1 << CRC5_WIDTH) - 1
    for b in bits:
        feedback = ((crc & top) != 0) ^ (b & 1)
        crc = (crc << 1) & mask
        if feedback:
            crc ^= CRC5_POLY
    return crc


def reflect(value, width):
    """Reverse the low ``width`` bits of ``value`` (LSB <-> MSB)."""
    result = 0
    for i in range(width):
        if value & (1 << i):
            result |= 1 << (width - 1 - i)
    return result


def token_bits(addr, endp):
    """The 11-bit token field as transmitted: ADDR[7] then ENDP[4], LSB first.

    USB sends each field least-significant bit first, and the CRC covers the
    bits in that transmission order.
    """
    return [(addr >> i) & 1 for i in range(7)] + [(endp >> i) & 1 for i in range(4)]


def token_crc5(addr, endp):
    """Full on-wire CRC5 for a token: remainder, then invert and reverse.

    This is what the MAC appends to a token on the wire; the bare module stops
    at the remainder.
    """
    rem = crc5_remainder(token_bits(addr, endp))
    return reflect(rem ^ ((1 << CRC5_WIDTH) - 1), CRC5_WIDTH)


def _main():
    # Directed patterns the testbench cross-checks against the RTL: seed
    # propagation (all zeros), single-bit feedback at each end, and a few real
    # token fields.
    patterns = [
        [0] * 11,
        [1] + [0] * 10,
        [0] * 10 + [1],
        token_bits(0x00, 0x0),
        token_bits(0x15, 0xE),
        token_bits(0x3A, 0xA),
        token_bits(0x7F, 0xF),
    ]
    print("# bits (fed in order)            remainder")
    for p in patterns:
        print(f"{''.join(str(b) for b in p):<32} 0x{crc5_remainder(p):02x}")
    print()
    print("# token (addr, endp) -> on-wire CRC5")
    for addr, endp in [(0x00, 0x0), (0x15, 0xE), (0x3A, 0xA), (0x7F, 0xF)]:
        print(f"({addr:#04x}, {endp:#04x}) -> 0x{token_crc5(addr, endp):02x}")


if __name__ == "__main__":
    _main()
