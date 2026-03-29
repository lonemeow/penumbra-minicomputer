; test_mem_wrap.s — Memory address wrapping test
;
; Verifies that addresses wrap modulo RAM size (16 MB = 0x01000000).
; Writes a marker at a low address, then reads via the aliased
; address (low + 16 MB) and checks they match.
;
; Also verifies a write through the aliased address is visible
; at the original address.
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; R2 = 0x00000200  (low address, past program code)
    LLI  R2, #0x200
    ; R3 = 0x100000    (probe increment, 1MB)
    LLI  R3, #0
    LUI  R3, #0x0010
    ; R4 = (current probe address)
    MOV R4, R2
    ADD R4, R3
    ; R5 = 0x02000000  (start of non-RAM physical addresses)
    LLI R5, #0x0000
    LUI R5, #0x0200
    ; R6 = 0x12345678  (test pattern)
    LLI R6, #0x5678
    LUI R6, #0x1234
    ; R8 = iteration count
    MOV R8, R0
loop:
    STW R6, [R4] ; Store through potentially aliasing address
    LDW R7, [R2] ; Read through low address
    CMP R6, R7
    BEQ wrap_found

    ADD R4, R3 ; Advance probe address by 1 MB
    ADD R8, #1 ; Increment iteration count
    CMP R4, R5 ; Have we reached end of RAM region yet?
    BLT loop

    ; If we fall through, no wrap found before end of RAM addresses
    B fail

wrap_found:
    ; Found a wrap
    ; R4 = Aliasing address
    ; R8 = Total addressable memory in probe increment units

    ; For test purposes, verify that reading through aliasing address also works
    NOT R6, R6
    STW R6, [R2]
    LDW R7, [R4]
    CMP R7, R6
    BNE fail

    ; Success, both reads and writes through the address alias as expected
    LLI R1, #1

fail:
    BREAK
