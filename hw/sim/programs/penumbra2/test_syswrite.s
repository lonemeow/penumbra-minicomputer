; test_syswrite.s — gen2 WRSYS write-path test (sysreg round-trip).
;
; WRSYS writes a GPR value to a CPU-internal sysreg. It is a drain-commit: it
; holds in EX until older instructions drain, commits there, then holds one
; extra cycle (post_commit_wait) so the device latches before any younger
; instruction can observe it. The write is composed in the spine from the held
; ID/EX fields at the commit and driven to the device block's write port.
;
; The value register is the Rd field of the encoding (matching the assembler and
; gen1) — gen2 decode now reads it from there (it previously read Rs, i.e. R0).
; This test would have read back 0 under that bug.
;
; The core's writable scratch sysreg (device 6, bring-up stand-in) is written by
; two WRSYS to different registers, then read back by RDSYS:
;   - distinct values to distinct registers prove the write data and the write
;     register selector both land correctly;
;   - the read-back through the separate RDSYS path proves write (EX) and read
;     (MEM) reach the same device across the two pipeline stages.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL); run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                ; assume FAIL until both round-trips check out

    ; ── Build two distinct values ───────────────────────────────
    LLI  R2, #0x5678
    LUI  R2, #0x1234           ; R2 = 0x12345678
    LLI  R3, #0xBABE
    LUI  R3, #0xCAFE           ; R3 = 0xCAFEBABE

    ; ── Write them to scratch device 6, registers 0 and 1 ───────
    WRSYS R2, #6, #0           ; scratch[0] = 0x12345678
    WRSYS R3, #6, #1           ; scratch[1] = 0xCAFEBABE

    ; ── Read back and check each ────────────────────────────────
    RDSYS R4, #6, #0
    CMP  R4, R2
    BNE  fail
    RDSYS R5, #6, #1
    CMP  R5, R3
    BNE  fail

    LLI  R1, #1               ; both round-trips correct → PASS
fail:
    BREAK
