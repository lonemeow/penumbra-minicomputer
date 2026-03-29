; test_exec_ram.s — Execute code from RAM
;
; Copies a small function from ROM to RAM, jumps to it, and verifies
; it executes correctly. This exercises the instruction fetch path
; through slow memory (simple_mem with realistic SDRAM latency).
;
; The RAM function:
;   - Sets R1 = 1 (pass)
;   - Returns to caller via RET (JMP R13)
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Copy ram_func from ROM to RAM at 0x1000 ──────────────
    LA   R2, #ram_func         ; source: ROM address of ram_func
    LLI  R3, #0x1000           ; dest: RAM address
    LA   R4, #ram_func_end     ; source end

copy_loop:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy_loop

    ; ── Call the RAM copy ────────────────────────────────────
    LLI  R3, #0x1000           ; RAM function address
    LA   R13, #return_here     ; set return address
    JMP  R3                    ; jump to RAM — fetches from slow memory

return_here:
    ; R1 should be 1 if ram_func executed correctly
    CMP  R1, #1
    BNE  fail

    ; ── All checks passed ───────────────────────────────────
    ; R1 is already 1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Function that will be copied to RAM and executed from there.
; Must be position-independent (no absolute labels or LA).
; ═══════════════════════════════════════════════════════════════
ram_func:
    LLI  R1, #1               ; PASS
    RET                        ; JMP R13 — back to return_here
ram_func_end:
