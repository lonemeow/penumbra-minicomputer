; test_subword_load.s — Verify LDB/LDBS/LDH/LDHS byte/halfword loads
;
; Stores a known word pattern, then loads individual bytes and
; halfwords at various offsets, checking zero/sign extension.
;
; Memory layout (little-endian, byte address 0x100):
;   addr+0: 0x81  (byte 0, negative if sign-extended)
;   addr+1: 0x02  (byte 1, positive)
;   addr+2: 0xFF  (byte 2, negative if sign-extended)
;   addr+3: 0x7F  (byte 3, positive)
;   Full word at 0x100: 0x7FFF0281
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0

    ; ── Store a known word pattern ────────────────────────
    LLI  R8, #0x0281        ; low half: 0x0281
    LUI  R8, #0x7FFF        ; high half: 0x7FFF → R8 = 0x7FFF0281
    LLI  R9, #0x200         ; base address (well past program code)
    STW  R8, [R9]

    ; ══════════════════════════════════════════════════════
    ; Byte loads — zero-extend (LDB)
    ; ══════════════════════════════════════════════════════

    ; Byte 0 at offset 0: 0x81 → zero-extend → 0x00000081
    LDB  R2, [R9]
    CMP  R2, #0x81
    BNE  fail

    ; Byte 1 at offset 1: 0x02 → zero-extend → 0x00000002
    LDB  R2, [R9 + #1]
    CMP  R2, #2
    BNE  fail

    ; Byte 2 at offset 2: 0xFF → zero-extend → 0x000000FF
    LDB  R2, [R9 + #2]
    CMP  R2, #0xFF
    BNE  fail

    ; Byte 3 at offset 3: 0x7F → zero-extend → 0x0000007F
    LDB  R2, [R9 + #3]
    CMP  R2, #0x7F
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Byte loads — sign-extend (LDBS)
    ; ══════════════════════════════════════════════════════

    ; Byte 0: 0x81 → sign-extend → 0xFFFFFF81
    LDBS R2, [R9]
    ; Can't CMP with negative imm16, so check via known value
    LLI  R3, #0xFF81
    LUI  R3, #0xFFFF        ; R3 = 0xFFFFFF81
    CMP  R2, R3
    BNE  fail

    ; Byte 1: 0x02 → sign-extend → 0x00000002 (positive, no change)
    LDBS R2, [R9 + #1]
    CMP  R2, #2
    BNE  fail

    ; Byte 3: 0x7F → sign-extend → 0x0000007F (positive, no change)
    LDBS R2, [R9 + #3]
    CMP  R2, #0x7F
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Halfword loads — zero-extend (LDH)
    ; ══════════════════════════════════════════════════════

    ; Half at offset 0: 0x0281 → zero-extend → 0x00000281
    LDH  R2, [R9]
    CMP  R2, #0x0281
    BNE  fail

    ; Half at offset 2: 0x7FFF → zero-extend → 0x00007FFF
    LDH  R2, [R9 + #2]
    CMP  R2, #0x7FFF
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Halfword loads — sign-extend (LDHS)
    ; ══════════════════════════════════════════════════════

    ; Half at offset 0: 0x0281 → sign-extend → 0x00000281 (positive)
    LDHS R2, [R9]
    CMP  R2, #0x0281
    BNE  fail

    ; Store a word with negative halfwords for sign-extend test
    LLI  R8, #0x8000        ; low half: 0x8000 (negative)
    LUI  R8, #0xFFFE        ; high half: 0xFFFE (negative) → R8 = 0xFFFE8000
    LLI  R9, #0x204         ; different word address
    STW  R8, [R9]

    ; Half at offset 0: 0x8000 → sign-extend → 0xFFFF8000
    LDHS R2, [R9]
    LLI  R3, #0x8000
    LUI  R3, #0xFFFF        ; R3 = 0xFFFF8000
    CMP  R2, R3
    BNE  fail

    ; Half at offset 2: 0xFFFE → sign-extend → 0xFFFFFFFE
    LDHS R2, [R9 + #2]
    LLI  R3, #0xFFFE
    LUI  R3, #0xFFFF        ; R3 = 0xFFFFFFFE
    CMP  R2, R3
    BNE  fail

    ; All passed
    LLI  R1, #1
fail:
    BREAK
