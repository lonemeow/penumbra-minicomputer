; test_alu_ops.s — comprehensive ALU opcode verification
;
; For every single-cycle ALU opcode, verifies:
;   - Correct result computed
;   - Correct source register(s) read (Rd != Rs to catch routing bugs)
;   - Source register not clobbered
;   - Key flags set correctly (N, Z, C, V where applicable)
;
; Convention: R1=1 PASS, R1=0 FAIL
; Pattern: "test; Bcc ok; BREAK" — on failure the PC of the
;          BREAK immediately identifies which check failed.

_start:
    LLI  R1, #0                ; assume fail

    ; ================================================================
    ; ADD — Rd = Rd + Rs, flags: NZCV
    ; ================================================================

    ; ADD: basic result and Rs preserved
    LLI  R2, #100
    LLI  R3, #50
    ADD  R2, R3                ; R2 = 150
    CMP  R2, #150
    BEQ  add_ok1
    BREAK
add_ok1:
    CMP  R3, #50              ; Rs unchanged
    BEQ  add_ok2
    BREAK
add_ok2:

    ; ADD: zero result sets Z
    LLI  R2, #0
    ADD  R2, R0               ; 0 + 0 = 0, Z=1
    BEQ  add_ok3
    BREAK
add_ok3:

    ; ADD: negative result sets N
    LLIS R2, #-1              ; 0xFFFFFFFF
    LLI  R3, #0
    ADD  R2, R3               ; result = 0xFFFFFFFF, N=1
    BMI  add_ok4
    BREAK
add_ok4:

    ; ADD: carry out sets C
    LLIS R2, #-1              ; 0xFFFFFFFF
    LLI  R3, #2
    ADD  R2, R3               ; 0xFFFFFFFF + 2 = 1 with C=1
    BCS  add_ok5
    BREAK
add_ok5:
    CMP  R2, #1               ; result = 1
    BEQ  add_ok6
    BREAK
add_ok6:

    ; ADD: no carry clears C
    LLI  R2, #1
    LLI  R3, #2
    ADD  R2, R3               ; 1 + 2 = 3, C=0
    BCC  add_ok7
    BREAK
add_ok7:

    ; ================================================================
    ; SUB — Rd = Rd - Rs, flags: NZCV (C = NOT borrow)
    ; ================================================================

    ; SUB: basic result and Rs preserved
    LLI  R2, #200
    LLI  R3, #75
    SUB  R2, R3               ; R2 = 125
    CMP  R2, #125
    BEQ  sub_ok1
    BREAK
sub_ok1:
    CMP  R3, #75              ; Rs unchanged
    BEQ  sub_ok2
    BREAK
sub_ok2:

    ; SUB: equal operands → Z=1, C=1 (no borrow)
    LLI  R2, #42
    LLI  R3, #42
    SUB  R2, R3               ; 0, Z=1, C=1
    BEQ  sub_ok3
    BREAK
sub_ok3:
    ; Re-do to check C (CMP above overwrote flags)
    LLI  R2, #42
    LLI  R3, #42
    SUB  R2, R3               ; C=1
    BCS  sub_ok4              ; C set = no borrow
    BREAK
sub_ok4:

    ; SUB: underflow → N=1, C=0 (borrow)
    LLI  R2, #10
    LLI  R3, #20
    SUB  R2, R3               ; -10, N=1
    BMI  sub_ok5
    BREAK
sub_ok5:
    ; Re-do for C
    LLI  R2, #10
    LLI  R3, #20
    SUB  R2, R3               ; C=0 (borrow)
    BCC  sub_ok6
    BREAK
sub_ok6:

    ; SUB: no borrow → C=1
    LLI  R2, #20
    LLI  R3, #10
    SUB  R2, R3               ; 10, C=1
    BCS  sub_ok7
    BREAK
sub_ok7:

    ; ================================================================
    ; AND — Rd = Rd & Rs, flags: NZ
    ; ================================================================

    ; AND: basic masking and Rs preserved
    LI   R2, #0xFF00FF00
    LI   R3, #0x0F0F0F0F
    AND  R2, R3               ; R2 = 0x0F000F00
    LI   R4, #0x0F000F00
    CMP  R2, R4
    BEQ  and_ok1
    BREAK
and_ok1:
    LI   R4, #0x0F0F0F0F
    CMP  R3, R4               ; Rs unchanged
    BEQ  and_ok2
    BREAK
and_ok2:

    ; AND: zero result → Z=1
    LLI  R2, #0xFF00
    LLI  R3, #0x00FF
    AND  R2, R3               ; 0, Z=1
    BEQ  and_ok3
    BREAK
and_ok3:

    ; AND: negative result → N=1
    LI   R2, #0x80000001
    LLIS R3, #-1              ; 0xFFFFFFFF
    AND  R2, R3               ; 0x80000001, N=1
    BMI  and_ok4
    BREAK
and_ok4:

    ; ================================================================
    ; OR — Rd = Rd | Rs, flags: NZ
    ; ================================================================

    ; OR: basic combine and Rs preserved
    LLI  R2, #0xF000
    LLI  R3, #0x000F
    OR   R2, R3               ; R2 = 0xF00F
    LLI  R4, #0xF00F
    CMP  R2, R4
    BEQ  or_ok1
    BREAK
or_ok1:
    CMP  R3, #0x000F          ; Rs unchanged
    BEQ  or_ok2
    BREAK
or_ok2:

    ; OR: with zero → identity
    LLI  R2, #0x1234
    OR   R2, R0               ; 0x1234 | 0 = 0x1234
    CMP  R2, #0x1234
    BEQ  or_ok3
    BREAK
or_ok3:

    ; OR: MSB set → N=1
    LI   R2, #0x80000000
    LLI  R3, #1
    OR   R2, R3               ; 0x80000001, N=1
    BMI  or_ok4
    BREAK
or_ok4:

    ; OR: zero | zero → Z=1
    LLI  R2, #0
    OR   R2, R0               ; 0, Z=1
    BEQ  or_ok5
    BREAK
or_ok5:

    ; ================================================================
    ; XOR — Rd = Rd ^ Rs, flags: NZ
    ; ================================================================

    ; XOR: basic and Rs preserved
    LLI  R2, #0xFF0F
    LLI  R3, #0x0FF0
    XOR  R2, R3               ; R2 = 0xF0FF
    LLI  R4, #0xF0FF
    CMP  R2, R4
    BEQ  xor_ok1
    BREAK
xor_ok1:
    CMP  R3, #0x0FF0          ; Rs unchanged
    BEQ  xor_ok2
    BREAK
xor_ok2:

    ; XOR: self → zero, Z=1
    LLI  R2, #0x5678
    XOR  R2, R2               ; 0, Z=1
    BEQ  xor_ok3
    BREAK
xor_ok3:

    ; XOR: with all-ones flips MSB → N
    LI   R2, #0x7FFFFFFF
    LLIS R3, #-1              ; 0xFFFFFFFF
    XOR  R2, R3               ; 0x80000000, N=1
    BMI  xor_ok4
    BREAK
xor_ok4:

    ; ================================================================
    ; SHL — Rd = Rd << Rs[4:0], flags: NZC
    ; ================================================================

    ; SHL: basic and Rs preserved
    LLI  R2, #1
    LLI  R3, #8
    SHL  R2, R3               ; 0x100
    CMP  R2, #0x100
    BEQ  shl_ok1
    BREAK
shl_ok1:
    CMP  R3, #8               ; Rs unchanged
    BEQ  shl_ok2
    BREAK
shl_ok2:

    ; SHL: carry = last bit shifted out (MSB into C)
    LI   R2, #0x80000000      ; MSB set
    LLI  R3, #1
    SHL  R2, R3               ; C=1, result=0
    BCS  shl_ok3
    BREAK
shl_ok3:
    CMP  R2, #0
    BEQ  shl_ok4
    BREAK
shl_ok4:

    ; SHL: shift by 0 → identity
    LLI  R2, #0x1234
    LLI  R3, #0
    SHL  R2, R3
    CMP  R2, #0x1234
    BEQ  shl_ok5
    BREAK
shl_ok5:

    ; ================================================================
    ; SHR — Rd = Rd >> Rs[4:0] (logical), flags: NZC
    ; ================================================================

    ; SHR: basic and Rs preserved
    LLI  R2, #0x8000
    LLI  R3, #4
    SHR  R2, R3               ; 0x0800
    CMP  R2, #0x0800
    BEQ  shr_ok1
    BREAK
shr_ok1:
    CMP  R3, #4               ; Rs unchanged
    BEQ  shr_ok2
    BREAK
shr_ok2:

    ; SHR: zero-fills from top (N must be clear)
    LLIS R2, #-1              ; 0xFFFFFFFF
    LLI  R3, #16
    SHR  R2, R3               ; 0x0000FFFF
    BPL  shr_ok3              ; N clear — check before CMP!
    BREAK
shr_ok3:
    LLI  R4, #0xFFFF
    CMP  R2, R4
    BEQ  shr_ok4
    BREAK
shr_ok4:

    ; SHR: carry = last bit shifted out (LSB into C)
    LLI  R2, #1
    LLI  R3, #1
    SHR  R2, R3               ; C=1, result=0
    BCS  shr_ok5
    BREAK
shr_ok5:

    ; ================================================================
    ; SAR — Rd = Rd >> Rs[4:0] (arithmetic), flags: NZC
    ; ================================================================

    ; SAR: sign-extends from top, Rs preserved
    LLIS R2, #-128             ; 0xFFFFFF80
    LLI  R3, #4
    SAR  R2, R3               ; 0xFFFFFFF8
    LLIS R4, #-8
    CMP  R2, R4
    BEQ  sar_ok1
    BREAK
sar_ok1:
    CMP  R3, #4               ; Rs unchanged
    BEQ  sar_ok2
    BREAK
sar_ok2:

    ; SAR: positive stays positive (check N before CMP!)
    LLI  R2, #0x7F00
    LLI  R3, #8
    SAR  R2, R3               ; 0x007F
    BPL  sar_ok3              ; N clear
    BREAK
sar_ok3:
    CMP  R2, #0x007F
    BEQ  sar_ok4
    BREAK
sar_ok4:

    ; SAR: negative stays negative (check N before CMP!)
    LLIS R2, #-1              ; 0xFFFFFFFF
    LLI  R3, #31
    SAR  R2, R3               ; still 0xFFFFFFFF
    BMI  sar_ok5              ; N set
    BREAK
sar_ok5:
    LLIS R4, #-1
    CMP  R2, R4
    BEQ  sar_ok6
    BREAK
sar_ok6:

    ; ================================================================
    ; MOV — Rd = Rs (PASS_A), no flag update
    ; ================================================================

    ; MOV: copies value, Rd != Rs to verify routing
    LLI  R2, #0
    LLI  R3, #0x5678
    MOV  R2, R3               ; R2 = 0x5678
    CMP  R2, #0x5678
    BEQ  mov_ok1
    BREAK
mov_ok1:
    CMP  R3, #0x5678          ; Rs unchanged
    BEQ  mov_ok2
    BREAK
mov_ok2:

    ; MOV: full 32-bit value preserved
    LI   R4, #0xDEADBEEF
    LLI  R5, #0
    MOV  R5, R4
    CMP  R5, R4
    BEQ  mov_ok3
    BREAK
mov_ok3:

    ; MOV: copy R0 → gives zero
    LLI  R2, #0xFFFF
    MOV  R2, R0               ; R2 = 0
    CMP  R2, #0
    BEQ  mov_ok4
    BREAK
mov_ok4:

    ; ================================================================
    ; NOT — Rd = ~Rs, flags: NZ
    ; CRITICAL: Rd != Rs in all tests (catches the reg_a/reg_b bug)
    ; ================================================================

    ; NOT: basic inversion, different Rd and Rs
    LLI  R2, #0xFFFF          ; garbage in Rd
    LLI  R3, #0x00FF
    NOT  R2, R3               ; R2 = ~0x000000FF = 0xFFFFFF00
    LI   R4, #0xFFFFFF00
    CMP  R2, R4
    BEQ  not_ok1
    BREAK
not_ok1:
    CMP  R3, #0x00FF          ; Rs unchanged — THE regression test
    BEQ  not_ok2
    BREAK
not_ok2:

    ; NOT: inverting zero → all ones, N=1
    LLI  R2, #0x1234          ; garbage in Rd
    NOT  R2, R0               ; R2 = ~0 = 0xFFFFFFFF
    BMI  not_ok3              ; N should be set
    BREAK
not_ok3:
    LLIS R4, #-1
    CMP  R2, R4
    BEQ  not_ok4
    BREAK
not_ok4:

    ; NOT: inverting all ones → zero, Z=1
    LLIS R3, #-1              ; 0xFFFFFFFF
    NOT  R2, R3               ; R2 = 0, Z=1
    BEQ  not_ok5
    BREAK
not_ok5:

    ; NOT: another Rd != Rs case with 32-bit values
    LLI  R6, #0
    LI   R7, #0xAAAA5555
    NOT  R6, R7               ; R6 = 0x5555AAAA
    LI   R4, #0x5555AAAA
    CMP  R6, R4
    BEQ  not_ok6
    BREAK
not_ok6:
    LI   R4, #0xAAAA5555
    CMP  R7, R4               ; R7 untouched
    BEQ  not_ok7
    BREAK
not_ok7:

    ; ================================================================
    ; CMP — flags-only SUB, Rd unchanged
    ; ================================================================

    ; CMP: equal → Z=1
    LLI  R2, #100
    LLI  R3, #100
    CMP  R2, R3
    BEQ  cmp_ok1
    BREAK
cmp_ok1:
    CMP  R2, #100             ; Rd not modified
    BEQ  cmp_ok2
    BREAK
cmp_ok2:

    ; CMP: greater → C=1 (no borrow)
    LLI  R2, #200
    LLI  R3, #100
    CMP  R2, R3
    BCS  cmp_ok3
    BREAK
cmp_ok3:

    ; CMP: less → N=1, C=0 (borrow)
    LLI  R2, #50
    LLI  R3, #200
    CMP  R2, R3               ; N=1
    BMI  cmp_ok4
    BREAK
cmp_ok4:
    LLI  R2, #50
    LLI  R3, #200
    CMP  R2, R3               ; C=0
    BCC  cmp_ok5
    BREAK
cmp_ok5:

    ; ================================================================
    ; TEST — flags-only AND, Rd unchanged
    ; ================================================================

    ; TEST: bits in common → Z=0
    LLI  R2, #0x00F0
    LLI  R3, #0x0FF0
    TEST R2, R3               ; 0x00F0, Z=0
    BNE  test_ok1
    BREAK
test_ok1:
    CMP  R2, #0x00F0          ; Rd not modified
    BEQ  test_ok2
    BREAK
test_ok2:

    ; TEST: no bits in common → Z=1
    LLI  R2, #0xFF00
    LLI  R3, #0x00FF
    TEST R2, R3               ; 0, Z=1
    BEQ  test_ok3
    BREAK
test_ok3:

    ; ================================================================
    ; Overflow flag (V) — signed overflow detection
    ; ================================================================

    ; Positive + positive -> overflow
    ; Reg+Reg form
    LI  R2, 0x7FFFFFFF
    LI  R3, 0x00000001
    ADD R2, R3
    BVS ovf_ok1
    BREAK
ovf_ok1:
    ; Negative + negative -> overflow
    ; Reg+Reg form
    LI  R2, -0x80000000
    LI  R3, -0x00000001
    ADD R2, R3
    BVS ovf_ok2
    BREAK
ovf_ok2:
    ; Positive + positive -> no overflow
    ; Reg+Reg form
    LI  R2, 0x0FFFFFFF
    LI  R3, 0x00000001
    ADD R2, R3
    BVC ovf_ok3
    BREAK
ovf_ok3:
    ; Negative + negative -> no overflow
    ; Reg+Reg form
    LI  R2, -0x08000000
    LI  R3, -0x00000001
    ADD R2, R3
    BVC ovf_ok4
    BREAK
ovf_ok4:


    ; ════════════════════════════════════════════════════════════════
    ; All checks passed
    ; ════════════════════════════════════════════════════════════════
    LLI  R1, #1               ; PASS
    BREAK
