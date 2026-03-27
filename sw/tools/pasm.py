#!/usr/bin/env python3
"""
Penumbra Assembler (pasm)

Two-pass assembler for the Penumbra ISA. Produces $readmemh-compatible hex.

Usage:
    python3 pasm.py input.s -o output.hex

Source format:
    ; Comments start with semicolon
    label:                  ; Labels end with colon
        LLI  R1, #42       ; Format L: immediate
        ADD  R1, R2         ; Format R: register-register
        LDW  R3, [R4 + #8] ; Format M: memory load
        BEQ  label          ; Format B: branch to label
        .word 0xDEADBEEF   ; Raw 32-bit data

Registers: R0-R15 (R0 is always zero, R13=LR, R14=SP, R15=PC)
Immediates: #decimal, #0xHEX, #0bBIN, or #-decimal
"""

import sys
import argparse
import re

# ── Register parsing ─────────────────────────────────────────
REG_ALIASES = {
    "SP": 14, "LR": 13, "PC": 15,
}

def parse_reg(s):
    """Parse register name, return 0-15 or None."""
    s = s.strip().upper()
    if s in REG_ALIASES:
        return REG_ALIASES[s]
    m = re.match(r"R(\d+)$", s)
    if m:
        n = int(m.group(1))
        if 0 <= n <= 15:
            return n
    return None

def parse_imm(s):
    """Parse immediate value (with or without # prefix)."""
    s = s.strip()
    if s.startswith("#"):
        s = s[1:]
    s = s.strip()
    neg = False
    if s.startswith("-"):
        neg = True
        s = s[1:]
    if s.startswith("0x") or s.startswith("0X"):
        val = int(s, 16)
    elif s.startswith("0b") or s.startswith("0B"):
        val = int(s, 2)
    else:
        val = int(s)
    return -val if neg else val

# ── Format R — Register-register ALU & system ops ────────────
# Encoding: [00][op:5][Rd:4][Rs:4][F:1][spare:16]

FORMAT_R_OPS = {
    # ALU ops (op, needs_rs, f_bit)
    "ADD":  (0,  True,  0), "SUB":  (1,  True,  0),
    "AND":  (2,  True,  0), "OR":   (3,  True,  0),
    "XOR":  (4,  True,  0), "SHL":  (5,  True,  0),
    "SHR":  (6,  True,  0), "SAR":  (7,  True,  0),
    "MOV":  (8,  True,  0), "NOT":  (9,  True,  0),
    "MUL":  (10, True,  0), "MULU": (11, True,  0),
    "DIV":  (12, True,  0), "DIVU": (13, True,  0),
    "MOD":  (14, True,  0), "MODU": (15, True,  0),
    # F=1 aliases
    "CMP":  (1,  True,  1), "TEST": (2,  True,  1),
    # System ops (no Rs for most)
    "GETSR":     (18, False, 0),
    "SETSR":     (19, False, 0),
    "SYSCALL":   (20, False, 0),
    "BREAK":     (21, False, 0),
    "RTI":       (22, False, 0),
    "ICACHE_INV":(23, False, 0),
    "JMP":       (24, True,  0),  # Rs field is the target register
    "EI":        (25, False, 0),
    "DI":        (26, False, 0),
    "GETUSP":    (27, False, 0),
    "SETUSP":    (28, False, 0),
}

# ── Format L — Immediate ops ─────────────────────────────────
# Encoding: [01][op:3][Rd:4][spare:7][imm16:16]

FORMAT_L_OPS = {
    "LLI":  0, "LLIS": 1, "LUI":  2,
    "INC":  3, "DEC":  4, "CMPI": 5,
}

# ── Format M — Memory load/store ─────────────────────────────
# Encoding: [10][L:1][sz:2][SE:1][Rd:4][Rb:4][offset16:16][spare:2]

FORMAT_M_OPS = {
    # (L, sz, SE)
    "LDW":  (1, 0b10, 0), "LDH":  (1, 0b01, 0), "LDHS": (1, 0b01, 1),
    "LDB":  (1, 0b00, 0), "LDBS": (1, 0b00, 1),
    "STW":  (0, 0b10, 0), "STH":  (0, 0b01, 0), "STB":  (0, 0b00, 0),
}

# ── Format B — Branch ────────────────────────────────────────
# Encoding: [11][cond:4][offset22:22][spare:4]

BRANCH_OPS = {
    "B":   0,  "BEQ": 1,  "BNE": 2,  "BCS": 3,  "BHS": 3,
    "BCC": 4,  "BLO": 4,  "BMI": 5,  "BPL": 6,  "BVS": 7,
    "BVC": 8,  "BHI": 9,  "BLS": 10, "BGE": 11, "BLT": 12,
    "BGT": 13, "BLE": 14, "BL":  15,
    # Semantic aliases (same condition, clearer intent in context)
    "BZ":  1,  "BNZ": 2,   # Zero/not-zero (after DEC, INC, etc.)
}

# Pseudo-instructions
PSEUDO_OPS = {"NOP", "RET"}


def encode_format_r(op, rd, rs, f_bit, spare=0):
    """Encode Format R instruction."""
    return (0b00 << 30) | (op << 25) | (rd << 21) | (rs << 17) | (f_bit << 16) | spare

def encode_format_l(op, rd, imm16):
    """Encode Format L instruction."""
    imm16 &= 0xFFFF
    return (0b01 << 30) | (op << 27) | (rd << 23) | imm16

def encode_format_m(l_bit, sz, se, rd, rb, offset16):
    """Encode Format M instruction."""
    offset16 &= 0xFFFF
    return ((0b10 << 30) | (l_bit << 29) | (sz << 27) | (se << 26)
            | (rd << 22) | (rb << 18) | (offset16 << 2))

def encode_format_b(cond, offset22):
    """Encode Format B instruction."""
    offset22 &= 0x3FFFFF
    return (0b11 << 30) | (cond << 26) | (offset22 << 4)


def assemble_line(mnemonic, operands, addr, labels, line_num):
    """Assemble one instruction, return 32-bit word or raise ValueError."""
    mn = mnemonic.upper()

    # ── Pseudo-instructions ──
    if mn == "NOP":
        return encode_format_r(0, 0, 0, 0)  # ADD R0, R0
    if mn == "RET":
        return encode_format_r(24, 0, 13, 0)  # JMP R13

    # ── Format R ──
    if mn in FORMAT_R_OPS:
        op, needs_rs, f_bit = FORMAT_R_OPS[mn]

        if mn in ("SYSCALL", "BREAK", "RTI", "ICACHE_INV", "EI", "DI"):
            return encode_format_r(op, 0, 0, 0)

        if mn == "JMP":
            if len(operands) != 1:
                raise ValueError(f"JMP expects 1 operand (Rs)")
            rs = parse_reg(operands[0])
            if rs is None:
                raise ValueError(f"bad register '{operands[0]}'")
            return encode_format_r(op, 0, rs, 0)

        if mn in ("GETSR", "GETUSP"):
            if len(operands) != 1:
                raise ValueError(f"{mn} expects Rd")
            rd = parse_reg(operands[0])
            if rd is None:
                raise ValueError(f"bad register '{operands[0]}'")
            return encode_format_r(op, rd, 0, 0)

        if mn in ("SETSR", "SETUSP"):
            if len(operands) != 1:
                raise ValueError(f"{mn} expects Rs")
            rs = parse_reg(operands[0])
            if rs is None:
                raise ValueError(f"bad register '{operands[0]}'")
            return encode_format_r(op, 0, rs, 0)

        # Standard ALU: mnemonic Rd, Rs
        if len(operands) != 2:
            raise ValueError(f"{mn} expects Rd, Rs")
        rd = parse_reg(operands[0])
        rs = parse_reg(operands[1])
        if rd is None:
            raise ValueError(f"bad register '{operands[0]}'")
        if rs is None:
            raise ValueError(f"bad register '{operands[1]}'")
        return encode_format_r(op, rd, rs, f_bit)

    # ── Format L ──
    if mn in FORMAT_L_OPS:
        op = FORMAT_L_OPS[mn]
        if len(operands) != 2:
            raise ValueError(f"{mn} expects Rd, #imm16")
        rd = parse_reg(operands[0])
        if rd is None:
            raise ValueError(f"bad register '{operands[0]}'")
        # Try label first (strip # prefix if present), then numeric immediate
        label_name = operands[1].strip().lstrip('#')
        if label_name in labels:
            imm = labels[label_name]
        else:
            imm = parse_imm(operands[1])
        return encode_format_l(op, rd, imm)

    # ── Format M ──
    if mn in FORMAT_M_OPS:
        l_bit, sz, se = FORMAT_M_OPS[mn]
        if len(operands) != 2:
            raise ValueError(f"{mn} expects [Rd, addr]")
        rd = parse_reg(operands[0])
        if rd is None:
            raise ValueError(f"bad register '{operands[0]}'")
        m = re.match(r"\[\s*(R\d+)\s*(?:([+-])\s*#\s*(-?\S+))?\s*\]", operands[1], re.IGNORECASE)
        if not m:
            raise ValueError(f"invalid address specification '{operands[1]}'")
        rb = parse_reg(m.group(1))
        if rb is None:
            raise ValueError(f"bad register '{m.group(1)}'")
        imm_str = m.group(3)
        if not imm_str:
            offset16 = 0
        else:
            offset16 = parse_imm(imm_str)
            if m.group(2) == '-':
                offset16 = -offset16
        return encode_format_m(l_bit, sz, se, rd, rb, offset16)

    # ── Format B ──
    if mn in BRANCH_OPS:
        cond = BRANCH_OPS[mn]
        if len(operands) != 1:
            raise ValueError(f"{mn} expects label or #offset")
        target_str = operands[0]

        # Try as label
        if target_str in labels:
            target_addr = labels[target_str]
            # offset = (target - PC) >> 2, signed 22-bit
            byte_offset = target_addr - addr
            if byte_offset % 4 != 0:
                raise ValueError(f"branch target not word-aligned")
            word_offset = byte_offset >> 2
            if word_offset < -(1 << 21) or word_offset >= (1 << 21):
                raise ValueError(f"branch target out of range ({word_offset})")
            return encode_format_b(cond, word_offset)

        # Try as numeric offset
        offset = parse_imm(target_str)
        return encode_format_b(cond, offset)

    raise ValueError(f"unknown mnemonic '{mnemonic}'")


def tokenize_operands(operand_str):
    """Split operand string into tokens, handling brackets."""
    # Join bracket expressions: "R3 , [ R4 + #8 ]" → ["R3", "[R4 + #8]"]
    operand_str = operand_str.strip()
    if not operand_str:
        return []

    result = []
    current = ""
    in_bracket = False

    for part in operand_str.split(","):
        part = part.strip()
        if not part:
            continue
        if "[" in part:
            in_bracket = True
            current = part
        elif in_bracket:
            current += "," + part
            if "]" in part:
                in_bracket = False
                result.append(current.strip())
                current = ""
        else:
            result.append(part.strip())

    if current:
        result.append(current.strip())

    return result


def assemble(source_lines):
    """Two-pass assembler. Returns list of (addr, word) pairs."""
    # Pass 1: collect labels and compute addresses
    labels = {}
    addr = 0
    instructions = []  # (line_num, addr, mnemonic, operands_str)

    for line_num, raw_line in enumerate(source_lines, 1):
        line = raw_line.split(";")[0].strip()
        if not line:
            continue

        # Directive: .org
        m = re.match(r"\.org\s+(0x[0-9a-fA-F]+|\d+)", line)
        if m:
            addr = int(m.group(1), 0)
            continue

        # Directive: .word
        m = re.match(r"\.word\s+(.+)", line, re.IGNORECASE)
        if m:
            instructions.append((line_num, addr, ".word", m.group(1).strip()))
            addr += 4
            continue

        # Label
        m = re.match(r"(\w+):\s*(.*)", line)
        if m:
            label = m.group(1)
            if label in labels:
                print(f"  Error line {line_num}: duplicate label '{label}'", file=sys.stderr)
                sys.exit(1)
            labels[label] = addr
            line = m.group(2).strip()
            if not line:
                continue

        # Instruction: split mnemonic from operands
        parts = line.split(None, 1)
        mnemonic = parts[0]
        operand_str = parts[1] if len(parts) > 1 else ""
        instructions.append((line_num, addr, mnemonic, operand_str))
        addr += 4

    # Pass 2: encode instructions
    output = []
    errors = 0
    for line_num, addr, mnemonic, operand_str in instructions:
        try:
            if mnemonic == ".word":
                word = parse_imm(operand_str)
                output.append((addr, word & 0xFFFFFFFF))
            else:
                operands = tokenize_operands(operand_str)
                word = assemble_line(mnemonic, operands, addr, labels, line_num)
                output.append((addr, word))
        except (ValueError, KeyError) as e:
            print(f"  Error line {line_num}: {e}", file=sys.stderr)
            errors += 1

    if errors:
        print(f"\n{errors} error(s) found.", file=sys.stderr)
        sys.exit(1)

    return output, labels


def emit_hex(entries, outfile, labels=None):
    """Write in $readmemh format, filling gaps with zeros."""
    if not entries:
        return

    # Build reverse label map
    label_at = {}
    if labels:
        for name, addr in labels.items():
            label_at.setdefault(addr, []).append(name)

    max_addr = max(a for a, _ in entries)
    entry_map = {a: w for a, w in entries}

    with open(outfile, "w") as f:
        f.write("// Penumbra program — generated by pasm.py\n\n")
        for addr in range(0, max_addr + 4, 4):
            if addr in label_at:
                names = ", ".join(label_at[addr])
                f.write(f"\n// [0x{addr:04X}] {names}\n")
            word = entry_map.get(addr, 0)
            f.write(f"{word:08X}\n")


def main():
    parser = argparse.ArgumentParser(description="Penumbra Assembler")
    parser.add_argument("input", help="Source file (.s)")
    parser.add_argument("-o", "--output", default="program.hex",
                        help="Output hex file (default: program.hex)")
    args = parser.parse_args()

    with open(args.input) as f:
        lines = f.readlines()

    entries, labels = assemble(lines)
    emit_hex(entries, args.output, labels)

    print(f"pasm: {len(entries)} words assembled, {len(labels)} labels -> {args.output}")


if __name__ == "__main__":
    main()
