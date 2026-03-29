#!/usr/bin/env python3
"""
Penumbra Microcode Assembler (uasm)

Reads a symbolic microcode source file and produces a $readmemh-compatible
hex file for the 256-entry × 49-bit microcode ROM.

Usage:
    python3 uasm.py input.uasm -o microcode.hex

Source format:
    # Comments start with #
    .org 0x00           # Set ROM address (hex or decimal)

    add:                # Label (for documentation, not referenced)
      reg_a=IR_RD  reg_b=IR_RS  reg_w=IR_RD  w_en=1
      alu=ADD  bmux=REG  wmux=RBUS
      flags=1  pc=PLUS4  branch=FETCH

    # Each non-blank, non-comment, non-directive line after a .org or label
    # is one micro-op (one ROM entry). Fields are space-separated key=value
    # pairs. Unspecified fields default to 0. Multiple lines before the next
    # label or .org are consecutive micro-ops at successive ROM addresses.

Field names and their symbolic values are defined below.
"""

import sys
import argparse
import re

# ── Micro-word field definitions ──────────────────────────────
# Each entry: (field_name, high_bit, low_bit, symbolic_values)
# Symbolic values is a dict mapping name → integer value.
# Fields are listed MSB-first matching the spec.

FIELDS = [
    ("priv", 50, 50, {
        # 1-bit: privileged instruction (sequencer checks on first micro-op)
    }),
    ("cross_bank", 49, 49, {
        # 1-bit: R14 accesses opposite bank (GETUSP/SETUSP)
    }),
    ("a_src", 48, 47, {
        "REG": 0, "ESR": 1, "EPC": 2, "VECTOR": 3,
    }),
    ("reg_a", 46, 43, {
        "IR_RD": 0, "IR_RS": 1,
        # Literal registers: R2-R15 (value = register number)
        **{f"R{i}": i for i in range(2, 16)},
    }),
    ("reg_b", 42, 39, {
        "IR_RD": 0, "IR_RS": 1,
        **{f"R{i}": i for i in range(2, 16)},
    }),
    ("reg_w", 38, 35, {
        "IR_RD": 0, "IR_RS": 1,
        **{f"R{i}": i for i in range(2, 16)},
    }),
    ("w_en", 34, 34, {
        # 1-bit field: 0 or 1
    }),
    ("alu", 33, 29, {
        "ADD": 0, "SUB": 1, "AND": 2, "OR": 3, "XOR": 4,
        "SHL": 5, "SHR": 6, "SAR": 7,
        "PASS_A": 8, "PASS_B": 9, "NOT": 10,
        "MUL": 11, "MULU": 12, "DIV": 13, "DIVU": 14,
        "MOD": 15, "MODU": 16,
    }),
    ("bmux", 28, 27, {
        "REG": 0, "IMM": 1, "CONST4": 2, "CONST8": 3,
    }),
    ("wmux", 26, 26, {
        "RBUS": 0, "MDR": 1,
    }),
    ("imm_mode", 25, 24, {
        "ZERO_EXT": 0, "SIGN_EXT": 1, "SHIFT_L16": 2,
    }),
    ("w_flags", 23, 23, {
        # 1-bit field: 0 or 1
    }),
    ("sr_load", 22, 22, {}),
    ("mar_load", 21, 21, {}),
    ("mdr_load_mem", 20, 20, {}),
    ("mdr_load_a", 19, 19, {}),
    ("mem_read", 18, 18, {}),
    ("mem_write", 17, 17, {}),
    ("mem_size", 16, 15, {
        "BYTE": 0, "HALF": 1, "WORD": 2,
    }),
    ("sign_ext", 14, 14, {}),
    ("pc", 13, 11, {
        "HOLD": 0, "NEXT": 1, "PLUS4": 1, "OFFSET": 2, "ABUS": 3, "MDR": 4,
    }),
    ("sys_cycle", 10, 10, {}),
    ("sys_we", 9, 9, {}),
    ("alu_start", 8, 8, {}),
    ("branch", 7, 5, {
        "SEQ": 0, "FETCH": 1, "STALL": 2, "BRT": 3, "BRF": 4,
        "SKIP": 6,
    }),
    ("fwd_offset", 4, 2, {}),
    ("ei_set", 1, 1, {}),
    ("di_set", 0, 0, {}),
]

# Build lookup tables
FIELD_MAP = {}  # name → (high, low, symbols)
for name, hi, lo, syms in FIELDS:
    FIELD_MAP[name] = (hi, lo, syms)

ROM_SIZE = 256
WORD_BITS = 51  # bits 50:0

# ── Dispatch slot layout ─────────────────────────────────────
# Each zone defines a contiguous range of ROM addresses with a fixed
# slot size. Multi-step micro-routines must not cross slot boundaries.
# Entries: (start, end_exclusive, slot_size, zone_name)
SLOT_ZONES = [
    (0x00, 0x20, 2, "R-ALU"),         # 16 ×2 slots (ALU ops, op[4]=0)
    (0x20, 0x40, 4, "Format L"),      # 8 ×4 slots
    (0x40, 0x60, 2, "R-SYS"),         # 16 ×2 slots (system ops, op[4]=1)
    (0x60, 0x64, 2, "Format B"),      # 2 ×2 slots: Bcc (0x60), BL (0x62)
    (0x70, 0x71, 1, "Exception"),     # 1 single-entry slot (int_entry)
    (0x80, 0xC0, 4, "Format M"),      # 16 ×4 slots
]


def slot_boundary(addr):
    """Return (slot_start, slot_end_exclusive, zone_name) for a ROM address,
    or None if the address is in an unassigned region.

    For zones with slot_size == 1 (Format R, B, Exception), the "slot" is
    the entire zone — multi-step routines intentionally consume consecutive
    entries. The real constraint is not crossing into the next zone.

    For zones with slot_size > 1 (Format L, M with ×4 spacing), the slot
    boundary is enforced per-slot since the dispatch hardware uses fixed
    spacing."""
    for zone_start, zone_end, slot_size, zone_name in SLOT_ZONES:
        if zone_start <= addr < zone_end:
            if slot_size == 1:
                # Single-entry zones: only enforce zone boundary
                return (addr, zone_end, zone_name)
            else:
                # Multi-entry zones: enforce per-slot boundary
                slot_index = (addr - zone_start) // slot_size
                slot_start = zone_start + slot_index * slot_size
                return (slot_start, slot_start + slot_size, zone_name)
    return None

# Default field values — applied when not explicitly specified.
# Most micro-ops end with "advance PC, return to fetch," so we default
# to that. Multi-step routines override with pc=HOLD branch=SEQ or
# branch=STALL as needed.
DEFAULTS = {
    "pc": "NEXT",
    "branch": "FETCH",
}


def parse_value(field_name, value_str, symbols, line_num):
    """Parse a field value: symbolic name or integer literal."""
    # Try symbolic lookup first
    upper = value_str.upper()
    if upper in symbols:
        return symbols[upper]

    # Try integer literal (decimal, hex, binary)
    try:
        if value_str.startswith("0x") or value_str.startswith("0X"):
            return int(value_str, 16)
        elif value_str.startswith("0b") or value_str.startswith("0B"):
            return int(value_str, 2)
        else:
            return int(value_str)
    except ValueError:
        print(f"  Error line {line_num}: unknown value '{value_str}' for field '{field_name}'",
              file=sys.stderr)
        print(f"    Valid symbols: {', '.join(symbols.keys()) if symbols else '(integer only)'}",
              file=sys.stderr)
        sys.exit(1)


def pack_word(field_values, line_num):
    """Pack field name→value pairs into a 49-bit integer."""
    # Apply defaults for fields not explicitly set
    merged = dict(DEFAULTS)
    merged.update(field_values)

    word = 0
    for name, value in merged.items():
        if name not in FIELD_MAP:
            print(f"  Error line {line_num}: unknown field '{name}'", file=sys.stderr)
            print(f"    Valid fields: {', '.join(FIELD_MAP.keys())}", file=sys.stderr)
            sys.exit(1)

        hi, lo, symbols = FIELD_MAP[name]
        width = hi - lo + 1
        max_val = (1 << width) - 1

        val = parse_value(name, str(value), symbols, line_num)

        if val < 0 or val > max_val:
            print(f"  Error line {line_num}: value {val} out of range for "
                  f"field '{name}' (0..{max_val}, {width} bits)", file=sys.stderr)
            sys.exit(1)

        word |= (val & max_val) << lo

    return word


# ── Illegal instruction sentinel ────────────────────────────
# Unused ROM entries are filled with this value. The sequencer
# detects branch=7 (BR_ILLEGAL) on the first micro-op and traps
# to VEC_ILLEGAL. All other fields are zero (pc=HOLD, no side effects).
ILLEGAL_SENTINEL = 7 << 5  # branch[7:5] = 111, everything else 0


def assemble(source_lines):
    """Assemble source lines into ROM contents."""
    rom = [ILLEGAL_SENTINEL] * ROM_SIZE
    addr = 0
    labels = {}
    errors = 0

    # Slot overflow tracking: when a .org or label starts a routine,
    # record the slot boundary. Subsequent micro-ops must stay within it.
    current_slot = None   # (slot_start, slot_end, zone_name) or None
    current_label = None  # label that started this routine

    for line_num, raw_line in enumerate(source_lines, 1):
        # Strip comments and whitespace
        line = raw_line.split("#")[0].strip()
        if not line:
            continue

        # Directive: .org
        m = re.match(r"\.org\s+(0x[0-9a-fA-F]+|\d+)", line)
        if m:
            addr = int(m.group(1), 0)
            if addr >= ROM_SIZE:
                print(f"  Error line {line_num}: .org address 0x{addr:02X} "
                      f"exceeds ROM size ({ROM_SIZE})", file=sys.stderr)
                errors += 1
            current_slot = slot_boundary(addr)
            current_label = None
            continue

        # Label (ends with colon)
        m = re.match(r"(\w+):\s*(.*)", line)
        if m:
            label = m.group(1)
            labels[label] = addr
            current_slot = slot_boundary(addr)
            current_label = label
            # Rest of line may contain field assignments
            line = m.group(2).strip()
            if not line:
                continue

        # Field assignments: key=value pairs
        pairs = line.split()
        field_values = {}
        for pair in pairs:
            if "=" not in pair:
                print(f"  Error line {line_num}: expected field=value, got '{pair}'",
                      file=sys.stderr)
                errors += 1
                continue

            key, val = pair.split("=", 1)
            if key in field_values:
                print(f"  Warning line {line_num}: field '{key}' set twice", file=sys.stderr)
            field_values[key] = val

        if addr >= ROM_SIZE:
            print(f"  Error line {line_num}: ROM address 0x{addr:02X} "
                  f"out of range", file=sys.stderr)
            errors += 1
        elif field_values:
            # Slot boundary check
            if current_slot is not None:
                slot_start, slot_end, zone_name = current_slot
                if addr >= slot_end:
                    routine = current_label or f"0x{slot_start:02X}"
                    print(f"  Error line {line_num}: micro-op at 0x{addr:02X} overflows "
                          f"{zone_name} slot 0x{slot_start:02X}–0x{slot_end-1:02X} "
                          f"(routine '{routine}')", file=sys.stderr)
                    errors += 1

            rom[addr] = pack_word(field_values, line_num)
            addr += 1

    if errors:
        print(f"\n{errors} error(s) found.", file=sys.stderr)
        sys.exit(1)

    return rom, labels


def emit_hex(rom, outfile, labels=None):
    """Write ROM contents in $readmemh format."""
    # Build reverse label map (addr → label)
    label_at = {}
    if labels:
        for name, addr in labels.items():
            label_at.setdefault(addr, []).append(name)

    # 49 bits = 13 hex digits (ceil(49/4))
    hex_width = (WORD_BITS + 3) // 4

    with open(outfile, "w") as f:
        f.write(f"// Penumbra microcode ROM — {ROM_SIZE} entries × {WORD_BITS} bits\n")
        f.write(f"// Generated by uasm.py\n\n")

        for addr in range(ROM_SIZE):
            # Label comments
            if addr in label_at:
                names = ", ".join(label_at[addr])
                f.write(f"\n// [{addr:3d} / 0x{addr:02X}] {names}\n")

            word = rom[addr]
            f.write(f"{word:0{hex_width}X}\n")


def main():
    parser = argparse.ArgumentParser(description="Penumbra Microcode Assembler")
    parser.add_argument("input", nargs="?", help="Source file (.uasm)")
    parser.add_argument("-o", "--output", default="microcode.hex",
                        help="Output hex file (default: microcode.hex)")
    parser.add_argument("--dump", action="store_true",
                        help="Print field table and exit")
    args = parser.parse_args()

    if args.dump:
        print(f"{'Field':<15} {'Bits':>7}  {'Width':>5}  Symbols")
        print("-" * 70)
        for name, hi, lo, syms in FIELDS:
            bits = f"[{hi}:{lo}]" if hi != lo else f"   [{hi}]"
            width = hi - lo + 1
            sym_str = ", ".join(f"{k}={v}" for k, v in syms.items()) if syms else "(integer)"
            print(f"{name:<15} {bits:>7}  {width:>5}  {sym_str}")
        return

    with open(args.input) as f:
        lines = f.readlines()

    rom, labels = assemble(lines)

    emit_hex(rom, args.output, labels)

    # Summary
    nonzero = sum(1 for w in rom if w != 0)
    print(f"uasm: {nonzero} micro-ops assembled, {len(labels)} labels → {args.output}")


if __name__ == "__main__":
    main()
