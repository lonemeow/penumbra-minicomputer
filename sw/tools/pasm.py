#!/usr/bin/env python3
"""
Penumbra Assembler (pasm)

Two-pass assembler for the Penumbra ISA. Produces $readmemh-compatible hex.

Usage:
    python3 pasm.py input.s -o output.hex [--org 0xFFFF0000]

Source format:
    ; Comments start with semicolon
    label:                  ; Labels end with colon
        LLI  R1, #42       ; Format L: immediate
        ADD  R1, R2         ; Format R: register-register
        ADD  R1, #5         ; Smart: assembler picks Format L
        CMP  R1, #10        ; Smart: assembler picks Format L
        LDW  R3, [R4 + #8] ; Format M: memory load
        BEQ  label          ; Format B: branch to label
        LA   R1, #label     ; Pseudo: load 32-bit label address (LLI+LUI)
        LI   R1, #0x12345678 ; Pseudo: load 32-bit immediate (LLI+LUI)
        ERET                ; Exception return via EPC/ESR
        RDSPR R1, USP       ; Read special-purpose register (ESR, EPC, or USP)
        WRSPR USP, R1       ; Write special-purpose register (ESR, EPC, or USP)
        .word 0xDEADBEEF   ; Raw 32-bit data
        .word 0x1234, 0x5678 ; Multiple words
        .byte 0x41, 0x42   ; Raw bytes (packed little-endian into words)
        .asciz "Hello\\n"   ; Null-terminated ASCII string
        .equ NAME, 0xFF    ; Named constant

Registers: R0-R15 (R0 is always zero, R13=LR, R14=SP, R15=PC)
Immediates: #decimal, #0xHEX, #0bBIN, #-decimal, or #NAME

Built-in constants for sysreg access:
    WRSYS R4, #MMU, #TLB_INDEX   ; device and register by name
    LLI   R1, #TLB_V             ; flag constants (0x01, 0x08, etc.)
"""

import sys
import argparse
import re

# ── Built-in constants (mirrors penumbra_pkg.sv) ─────────────
# These are always available; user .equ definitions can override them.

BUILTIN_CONSTANTS = {
    # Sysreg device IDs
    "MMU":          0,
    "SYS":          1,

    # MMU registers (device 0)
    "MMUCR":        0,
    "FAULT_ADDR":   1,
    "FAULT_STATUS": 2,
    "TLB_VPN":      3,
    "TLB_PTE":      4,
    "TLB_INDEX":    5,

    # SYS registers (device 1)
    "MACHINE_ID":   0,  # legacy alias for CPU_ISA
    "CPU_ISA":      0,
    "MACH_FEAT":    1,
    "CPU_NAME0":    2,
    "CPU_NAME1":    3,
    "CPU_NAME2":    4,
    "CPU_NAME3":    5,
    "MACH_NAME0":   6,
    "MACH_NAME1":   7,
    "MACH_NAME2":   8,
    "MACH_NAME3":   9,

    # Cache device IDs (devices 2 and 3)
    "DCACHE":       2,
    "ICACHE":       3,

    # Cache registers (shared layout for both D-cache and I-cache)
    "CACHE_INFO":   0,
    "CACHE_CTRL":   1,
    "CACHE_INVAL":  2,

    # TLB PTE flag bits
    "TLB_V":        0x01,
    "TLB_C":        0x04,
    "TLB_R":        0x08,
    "TLB_W":        0x10,
    "TLB_X":        0x20,
    "TLB_U":        0x40,
    "TLB_G":        0x80,

    # UART MMIO (16450-compatible, word-strided at 0xFF00_0000)
    "UART_BASE":    0xFF000000,
    "UART_DATA":    0x00,   # RBR (read) / THR (write), DLAB=0
    "UART_IER":     0x04,   # Interrupt enable, DLAB=0
    "UART_IIR":     0x08,   # Interrupt identification (read)
    "UART_LCR":     0x0C,   # Line control (DLAB = bit 7)
    "UART_MCR":     0x10,   # Modem control
    "UART_LSR":     0x14,   # Line status
    "UART_MSR":     0x18,   # Modem status
    "UART_SCR":     0x1C,   # Scratch register
    "UART_DLL":     0x00,   # Divisor latch low (DLAB=1)
    "UART_DLM":     0x04,   # Divisor latch high (DLAB=1)

    # UART LSR bit masks
    "LSR_DR":       0x01,   # Data ready (RX)
    "LSR_THRE":     0x20,   # TX holding register empty
    "LSR_TEMT":     0x40,   # Transmitter empty

    # MMU fault status masks (mirrors penumbra_pkg.sv FSTAT_*)
    "FAULT_TLB_MISS": 1,    # fault_type[3:0] = TLB miss
    "FAULT_PROT":     2,    # fault_type[3:0] = protection violation
    "FAULT_ALIGN":    3,    # fault_type[3:0] = alignment
    "FAULT_BUS":      4,    # fault_type[3:0] = bus fault (no device)
    "FSTAT_R":    0x100,    # bit 8: faulting access was read
    "FSTAT_W":    0x200,    # bit 9: faulting access was write
    "FSTAT_X":    0x400,    # bit 10: faulting access was execute
    "FSTAT_USR":  0x800,    # bit 11: faulting access was user mode
}

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

def parse_imm(s, constants=None):
    """Parse immediate value (with or without # prefix).

    Accepts numeric literals (#42, #0xFF, #0b101, #-3) or named
    constants (#TLB_V, #MMU). The constants dict is checked when
    numeric parsing fails.
    """
    s = s.strip()
    if s.startswith("#"):
        s = s[1:]
    s = s.strip()
    neg = False
    if s.startswith("-"):
        neg = True
        s = s[1:]
    # Try numeric literal
    try:
        if s.startswith("0x") or s.startswith("0X"):
            val = int(s, 16)
        elif s.startswith("0b") or s.startswith("0B"):
            val = int(s, 2)
        else:
            val = int(s)
        return -val if neg else val
    except ValueError:
        pass
    # Try named constant
    name = s.upper()
    if constants and name in constants:
        val = constants[name]
        return -val if neg else val
    raise ValueError(f"unknown immediate or constant '{s}'")

# ── Data directive helpers ───────────────────────────────────

def parse_escape_string(s):
    """Parse C-style escape sequences, return list of byte values."""
    result = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            c = s[i + 1]
            if   c == 'n':  result.append(0x0A); i += 2
            elif c == 'r':  result.append(0x0D); i += 2
            elif c == 't':  result.append(0x09); i += 2
            elif c == '\\': result.append(0x5C); i += 2
            elif c == '"':  result.append(0x22); i += 2
            elif c == '0':  result.append(0x00); i += 2
            elif c == 'x' and i + 3 < len(s):
                result.append(int(s[i+2:i+4], 16)); i += 4
            else:
                result.append(ord(s[i])); i += 1
        else:
            result.append(ord(s[i])); i += 1
    return result

def pack_bytes_to_words(byte_list):
    """Pack byte values into 32-bit words, little-endian, zero-padded."""
    padded = list(byte_list)
    while len(padded) % 4 != 0:
        padded.append(0)
    words = []
    for i in range(0, len(padded), 4):
        word = padded[i] | (padded[i+1] << 8) | (padded[i+2] << 16) | (padded[i+3] << 24)
        words.append(word)
    return words

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
    # F=1 aliases (CMP Rd, Rs — register form; CMP Rd, #imm routed to Format L)
    "CMP":  (1,  True,  1), "TEST": (2,  True,  1),
    # System ops (no Rs for most)
    "WRSYS":     (16, True,  0),  # WRSYS Rd, #dev, #reg (special 3-operand)
    "RDSYS":     (17, True,  0),  # RDSYS Rd, #dev, #reg (special 3-operand)
    "GETSR":     (18, False, 0),
    "SETSR":     (19, False, 0),
    "SYSCALL":   (20, False, 0),
    "BREAK":     (21, False, 0),
    "RTI":       (22, False, 0),  # Legacy alias for ERET (no args)
    "ICACHE_INV":(23, False, 0),
    "JMP":       (24, True,  0),  # Rs field is the target register
    "EI":        (25, False, 0),
    "DI":        (26, False, 0),
    "WRSPR":     (27, False, 0),   # WRSPR {ESR|EPC|USP}, Rd — SPR in spare[15:12]
    "RDSPR":     (28, False, 0),   # RDSPR Rd, {ESR|EPC|USP} — SPR in spare[15:12]
}

# ── Format L — Immediate ops ─────────────────────────────────
# Encoding: [01][op:4][Rd:4][spare:6][imm16:16]

FORMAT_L_OPS = {
    "LLI":  0, "LLIS": 1, "LUI":  2,
    "INC":  3, "DEC":  4, "CMPI": 5, "ANDI": 6, "TESTI": 7,
    "SHLI": 8, "SHRI": 9, "SARI": 10,
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

# SPR name → number mapping (encoded in IR[15:12], same position as sys_dev)
SPR_NAMES = {"ESR": 0, "EPC": 1, "USP": 2}

# Pseudo-instructions
PSEUDO_OPS = {"NOP", "RET", "LA", "LI"}


def encode_format_r(op, rd, rs, f_bit, spare=0):
    """Encode Format R instruction."""
    return (0b00 << 30) | (op << 25) | (rd << 21) | (rs << 17) | (f_bit << 16) | spare

def encode_format_l(op, rd, imm16):
    """Encode Format L instruction."""
    imm16 &= 0xFFFF
    return (0b01 << 30) | (op << 26) | (rd << 22) | imm16

def encode_format_m(l_bit, sz, se, rd, rb, offset16):
    """Encode Format M instruction."""
    offset16 &= 0xFFFF
    return ((0b10 << 30) | (l_bit << 29) | (sz << 27) | (se << 26)
            | (rd << 22) | (rb << 18) | (offset16 << 2))

def encode_format_b(cond, offset22):
    """Encode Format B instruction."""
    offset22 &= 0x3FFFFF
    return (0b11 << 30) | (cond << 26) | (offset22 << 4)


def assemble_line(mnemonic, operands, addr, labels, line_num, constants=None):
    """Assemble one instruction, return 32-bit word or raise ValueError."""
    mn = mnemonic.upper()

    # ── Pseudo-instructions ──
    if mn == "NOP":
        return encode_format_r(0, 0, 0, 0)  # ADD R0, R0
    if mn == "RET":
        return encode_format_r(24, 0, 13, 0)  # JMP R13

    # ── ERET — exception return via EPC/ESR ─────────────────────
    if mn == "ERET":
        if len(operands) == 0:
            op = FORMAT_R_OPS["RTI"][0]
            return encode_format_r(op, 0, 0, 0)
        else:
            raise ValueError("ERET takes no operands (use WRSPR EPC/ESR to modify return state before ERET)")

    # ── Smart mnemonic routing ───────────────────────────────────
    # TODO(human): implement smart_route_to_format_l()
    # ADD/SUB/CMP with an immediate operand → Format L (INC/DEC/CMPI)
    # This function is called for mnemonics that exist in both Format R
    # (register-register) and have a Format L counterpart (register-immediate).
    SMART_MNEMONICS = {"ADD": "INC", "SUB": "DEC", "CMP": "CMPI", "AND": "ANDI", "TEST": "TESTI",
                       "SHL": "SHLI", "SHR": "SHRI", "SAR": "SARI"}

    def smart_route_to_format_l(mn, operands, constants, labels):
        """Check if a smart mnemonic should be routed to Format L.

        Args:
            mn: uppercase mnemonic (ADD, SUB, or CMP)
            operands: list of operand strings (e.g. ["R1", "#5"])
            constants: dict of named constants
            labels: dict of labels

        Returns:
            Encoded 32-bit instruction word if this is a reg-imm form,
            or None if it's a reg-reg form (caller should fall through
            to normal Format R handling).
        """
        if len(operands) != 2:
            return None  # Fall through — Format R handler will give proper error
        rd = parse_reg(operands[0])
        if rd is None:
            raise ValueError(f"bad register '{operands[0]}'")
        if parse_reg(operands[1]) is not None:
            return None  # reg-reg form → Format R
        label_name = operands[1].strip().lstrip('#')
        if label_name in labels:
            imm = labels[label_name]
        else:
            imm = parse_imm(operands[1], constants)
        op = FORMAT_L_OPS[SMART_MNEMONICS[mn]]
        return encode_format_l(op, rd, imm)

    if mn in SMART_MNEMONICS:
        result = smart_route_to_format_l(mn, operands, constants, labels)
        if result is not None:
            return result
        # Fall through to Format R handling for reg-reg form

    # ── RDSPR Rd, {ESR|EPC|USP} — read special-purpose register ──
    # Unified: single opcode, SPR number in spare[15:12]
    if mn == "RDSPR":
        if len(operands) != 2:
            raise ValueError("RDSPR expects Rd, {ESR|EPC|USP}")
        rd = parse_reg(operands[0])
        if rd is None:
            raise ValueError(f"bad register '{operands[0]}'")
        spec = operands[1].upper()
        if spec not in SPR_NAMES:
            raise ValueError(f"RDSPR: unknown SPR '{operands[1]}', expected ESR, EPC, or USP")
        spr_num = SPR_NAMES[spec]
        op = FORMAT_R_OPS["RDSPR"][0]
        spare = spr_num << 12
        return encode_format_r(op, rd, 0, 0, spare)

    # ── WRSPR {ESR|EPC|USP}, Rd — write special-purpose register ──
    # Unified: single opcode, SPR number in spare[15:12]
    if mn == "WRSPR":
        if len(operands) != 2:
            raise ValueError("WRSPR expects {ESR|EPC|USP}, Rd")
        spec = operands[0].upper()
        rd = parse_reg(operands[1])
        if rd is None:
            raise ValueError(f"bad register '{operands[1]}'")
        if spec not in SPR_NAMES:
            raise ValueError(f"WRSPR: unknown SPR '{operands[0]}', expected ESR, EPC, or USP")
        spr_num = SPR_NAMES[spec]
        op = FORMAT_R_OPS["WRSPR"][0]
        spare = spr_num << 12
        return encode_format_r(op, rd, 0, 0, spare)

    # ── Format R ──
    if mn in FORMAT_R_OPS:
        op, needs_rs, f_bit = FORMAT_R_OPS[mn]

        if mn in ("SYSCALL", "BREAK", "RTI", "ICACHE_INV", "EI", "DI"):
            return encode_format_r(op, 0, 0, 0)

        # WRSYS Rd, #dev, #reg  /  RDSYS Rd, #dev, #reg
        # Encoding: Format R with dev in spare[15:12], reg in spare[11:8]
        if mn in ("WRSYS", "RDSYS"):
            if len(operands) != 3:
                raise ValueError(f"{mn} expects Rd, #dev, #reg")
            rd = parse_reg(operands[0])
            if rd is None:
                raise ValueError(f"bad register '{operands[0]}'")
            dev = parse_imm(operands[1], constants)
            reg = parse_imm(operands[2], constants)
            if not (0 <= dev <= 15):
                raise ValueError(f"device ID must be 0-15, got {dev}")
            if not (0 <= reg <= 15):
                raise ValueError(f"register index must be 0-15, got {reg}")
            spare = (dev << 12) | (reg << 8)
            return encode_format_r(op, rd, 0, 0, spare)

        if mn == "JMP":
            if len(operands) != 1:
                raise ValueError(f"JMP expects 1 operand (Rs)")
            rs = parse_reg(operands[0])
            if rs is None:
                raise ValueError(f"bad register '{operands[0]}'")
            return encode_format_r(op, 0, rs, 0)

        if mn == "GETSR":
            if len(operands) != 1:
                raise ValueError(f"{mn} expects Rd")
            rd = parse_reg(operands[0])
            if rd is None:
                raise ValueError(f"bad register '{operands[0]}'")
            return encode_format_r(op, rd, 0, 0)

        if mn == "SETSR":
            if len(operands) != 1:
                raise ValueError("SETSR expects Rs")
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
            imm = parse_imm(operands[1], constants)
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
            offset16 = parse_imm(imm_str, constants)
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
        offset = parse_imm(target_str, constants)
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


def assemble(source_lines, org=0):
    """Two-pass assembler. Returns list of (addr, word) pairs."""
    # Pass 1: collect labels, constants, and compute addresses
    labels = {}
    constants = dict(BUILTIN_CONSTANTS)  # user .equ can override builtins
    addr = org
    instructions = []  # (line_num, addr, mnemonic, operands_str)

    for line_num, raw_line in enumerate(source_lines, 1):
        line = raw_line.split(";")[0].strip()
        if not line:
            continue

        # Directive: .equ NAME, VALUE
        m = re.match(r"\.equ\s+(\w+)\s*,\s*(.+)", line, re.IGNORECASE)
        if m:
            name = m.group(1).upper()
            try:
                val = parse_imm(m.group(2).strip(), constants)
            except ValueError as e:
                print(f"  Error line {line_num}: .equ: {e}", file=sys.stderr)
                sys.exit(1)
            constants[name] = val
            continue

        # Directive: .org
        m = re.match(r"\.org\s+(0x[0-9a-fA-F]+|\d+)", line)
        if m:
            addr = int(m.group(1), 0)
            continue

        # Directive: .word (one or more comma-separated values)
        m = re.match(r"\.word\s+(.+)", line, re.IGNORECASE)
        if m:
            vals = [v.strip() for v in m.group(1).split(",")]
            instructions.append((line_num, addr, ".word", m.group(1).strip()))
            addr += len(vals) * 4
            continue

        # Directive: .byte (one or more comma-separated byte values)
        m = re.match(r"\.byte\s+(.+)", line, re.IGNORECASE)
        if m:
            vals = [v.strip() for v in m.group(1).split(",")]
            nwords = (len(vals) + 3) // 4
            instructions.append((line_num, addr, ".byte", m.group(1).strip()))
            addr += nwords * 4
            continue

        # Directive: .asciz "string" (null-terminated ASCII)
        m = re.match(r'\.asciz\s+"((?:[^"\\]|\\.)*)"', line, re.IGNORECASE)
        if m:
            byte_list = parse_escape_string(m.group(1))
            nbytes = len(byte_list) + 1  # +1 for null terminator
            nwords = (nbytes + 3) // 4
            instructions.append((line_num, addr, ".asciz", m.group(1)))
            addr += nwords * 4
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
        # LA/LI expand to 2 words (LLI + LUI)
        if mnemonic.upper() in ("LA", "LI"):
            addr += 8
        else:
            addr += 4

    # Pass 2: encode instructions
    output = []
    errors = 0
    for line_num, addr, mnemonic, operand_str in instructions:
        try:
            if mnemonic == ".word":
                vals = [v.strip() for v in operand_str.split(",")]
                for i, v in enumerate(vals):
                    word = parse_imm(v, constants)
                    output.append((addr + i * 4, word & 0xFFFFFFFF))
            elif mnemonic == ".byte":
                vals = [parse_imm(v.strip(), constants) & 0xFF for v in operand_str.split(",")]
                for i, word in enumerate(pack_bytes_to_words(vals)):
                    output.append((addr + i * 4, word))
            elif mnemonic == ".asciz":
                byte_list = parse_escape_string(operand_str) + [0]
                for i, word in enumerate(pack_bytes_to_words(byte_list)):
                    output.append((addr + i * 4, word))
            elif mnemonic.upper() in ("LA", "LI"):
                operands = tokenize_operands(operand_str)
                if len(operands) != 2:
                    raise ValueError(f"{mnemonic} expects Rd, #value_or_label")
                rd = parse_reg(operands[0])
                if rd is None:
                    raise ValueError(f"bad register '{operands[0]}'")
                label_name = operands[1].strip().lstrip('#')
                if label_name in labels:
                    val = labels[label_name]
                else:
                    val = parse_imm(operands[1], constants)
                val &= 0xFFFFFFFF
                lo = val & 0xFFFF
                hi = (val >> 16) & 0xFFFF
                output.append((addr,     encode_format_l(FORMAT_L_OPS["LLI"], rd, lo)))
                output.append((addr + 4, encode_format_l(FORMAT_L_OPS["LUI"], rd, hi)))
            else:
                operands = tokenize_operands(operand_str)
                word = assemble_line(mnemonic, operands, addr, labels, line_num, constants)
                output.append((addr, word))
        except (ValueError, KeyError) as e:
            print(f"  Error line {line_num}: {e}", file=sys.stderr)
            errors += 1

    if errors:
        print(f"\n{errors} error(s) found.", file=sys.stderr)
        sys.exit(1)

    return output, labels


def emit_hex(entries, outfile, labels=None, base=0):
    """Write in $readmemh format, filling gaps with zeros.

    The base parameter specifies the physical address of the first word.
    Addresses in the hex file are relative to base (index 0 = base).
    """
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
        for addr in range(base, max_addr + 4, 4):
            if addr in label_at:
                names = ", ".join(label_at[addr])
                f.write(f"\n// [0x{addr:08X}] {names}\n")
            word = entry_map.get(addr, 0)
            f.write(f"{word:08X}\n")


def main():
    parser = argparse.ArgumentParser(description="Penumbra Assembler")
    parser.add_argument("input", help="Source file (.s)")
    parser.add_argument("-o", "--output", default="program.hex",
                        help="Output hex file (default: program.hex)")
    parser.add_argument("--org", default="0", metavar="ADDR",
                        help="Base address for code (default: 0)")
    args = parser.parse_args()

    org = int(args.org, 0)

    with open(args.input) as f:
        lines = f.readlines()

    entries, labels = assemble(lines, org=org)
    emit_hex(entries, args.output, labels, base=org)

    print(f"pasm: {len(entries)} words assembled, {len(labels)} labels -> {args.output}")


if __name__ == "__main__":
    main()
