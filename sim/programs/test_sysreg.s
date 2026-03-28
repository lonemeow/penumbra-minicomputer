; test_sysreg.s — Test WRSYS/RDSYS with MMU control registers
;
; Tests:
;   1. Write a value to MMUCR, read it back → verify round-trip
;   2. Write TLB_INDEX, TLB_VPN, TLB_PTE → read back → verify
;   3. Read FAULT_ADDR (should be 0 after reset)
;
; Result in R1: 1 = all passed
;
; MMU sysreg addresses (device 0):
;   0 = MMUCR, 1 = FAULT_ADDR, 2 = FAULT_STATUS
;   3 = TLB_VPN, 4 = TLB_PTE, 5 = TLB_INDEX

_start:
    LLI  LR, #done           ; set return address
    LLI  R1, #0              ; R1 = result accumulator (0 = fail)

    ; ── Test 1: Write/read MMUCR ─────────────────────────────
    ; Write 0x0500 to MMUCR (ASID=5, M=0)
    LLI  R2, #0x0500
    WRSYS R2, #0, #0          ; MMUCR = 0x0500
    RDSYS R3, #0, #0          ; R3 = MMUCR
    CMP  R3, R2
    BNE  done                 ; fail: MMUCR round-trip mismatch

    ; ── Test 2: Write/read TLB entry ─────────────────────────
    ; Set TLB_INDEX = 0 (set 0, way 0)
    LLI  R4, #0
    WRSYS R4, #0, #5          ; TLB_INDEX = 0

    ; Write TLB_VPN = {4'b0, VPN=0xABCDE, ASID=0x42}
    ; VPN[19:0]=0xABCDE → bits [27:8], ASID=0x42 → bits [7:0]
    ; Full value = 0x0ABCDE42
    ; Build in two steps: LLI + LUI
    LLI  R5, #0xDE42          ; lower 16 bits
    LUI  R5, #0x0ABC          ; upper 16 bits → R5 = 0x0ABCDE42
    WRSYS R5, #0, #3          ; TLB_VPN = R5

    ; Write TLB_PTE = {PPN=0x12345, SW=0xA, flags=0x89}
    ; PPN[19:0]=0x12345 → bits [31:12], SW[3:0]=0xA → bits [11:8], flags=0x89
    ; Full value = 0x12345A89
    LLI  R6, #0x5A89          ; lower 16 bits
    LUI  R6, #0x1234          ; upper 16 bits → R6 = 0x12345A89
    WRSYS R6, #0, #4          ; TLB_PTE = R6 (commits entry)

    ; Read back TLB_VPN and TLB_PTE
    ; First re-select the index (in case it was affected)
    WRSYS R4, #0, #5          ; TLB_INDEX = 0
    RDSYS R7, #0, #3          ; R7 = TLB_VPN
    RDSYS R8, #0, #4          ; R8 = TLB_PTE

    ; Check VPN round-trip
    CMP  R7, R5
    BNE  done                 ; fail: TLB_VPN mismatch

    ; Check PTE round-trip
    CMP  R8, R6
    BNE  done                 ; fail: TLB_PTE mismatch

    ; ── Test 3: FAULT_ADDR reads as 0 after reset ────────────
    RDSYS R9, #0, #1          ; R9 = FAULT_ADDR
    LLI  R10, #0
    CMP  R9, R10
    BNE  done                 ; fail: FAULT_ADDR not zero

    ; ── All passed ────────────────────────────────────────────
    LLI  R1, #1               ; R1 = 1 (success)

done:
    B    done                  ; halt loop
