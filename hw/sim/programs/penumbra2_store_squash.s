; penumbra2_store_squash.s — gen2 precise-exception test: a younger store in
; the shadow of an older fault must NOT commit its memory write.
;
; In-order completion makes the WB slot the oldest in flight, so a fault is
; taken precisely there. A store that is *younger* than the faulting
; instruction is still in MEM (its launch cycle) when the fault flushes, and
; its write must be cancelled — the data memory must be untouched. This is the
; "store commit vs. fault flush" requirement (doc/TODO.md): o_dmem_we is gated
; on ~i_bubble, and the fault flush (i_bubble = wb_fault_commit) reaches MEM in
; the same cycle the older instruction faults at WB.
;
; The older instruction is a misaligned load (VEC_ALIGN); the younger store
; writes a poison value over a pre-seeded sentinel. The handler reloads the
; target: sentinel intact ⇒ the store was squashed (PASS); poison present ⇒ the
; store leaked through the flush (FAIL). All store operands are set up *before*
; the faulting load so the store sits immediately behind it (the tightest case).
;
; Vector table lives in RAM (physical low), written here at run time — reachable
; only by the vector fetch because fetch and data share one unified memory.
; Self-checks into R1 (1 = PASS, 0 = FAIL); run via tb_penumbra2_branch.

_start:
    LLI  R1, #0                 ; assume FAIL until the handler proves the squash

    ; ── Install the alignment-fault handler into the vector table ──
    LA   R2, align_handler      ; R2 = &align_handler (in ROM)
    LLI  R3, #0x20              ; VEC_ALIGN (8) << 2 = 0x20  (table slot, RAM)
    STW  R2, [R3]              ; vector_table[VEC_ALIGN] = handler address

    ; ── Seed the target word with a sentinel ────────────────────
    LLI  R7, #0x300            ; store target (word-aligned, clear of code/data)
    LLI  R6, #0xABCD           ; sentinel = 0x0000ABCD
    STW  R6, [R7]             ; mem[0x300] = sentinel (a normal, retiring store)

    ; ── Pre-stage the younger store's poison data + fault base ──
    LLI  R8, #0xDEAD           ; poison = 0x0000DEAD
    LLI  R4, #0x200            ; aligned base for the misaligned load

    ; ── The critical adjacency: older fault, younger store ──────
    LDW  R5, [R4 + #1]         ; OLDER: EA = 0x201, word-misaligned → VEC_ALIGN
    STW  R8, [R7]             ; YOUNGER: in MEM's launch cycle when the fault
                              ;          flushes — its write must be cancelled
    BREAK                      ; poison: must be flushed, never retires

align_handler:
    LDW  R9, [R7]             ; reload the target word
    CMP  R9, R6               ; sentinel intact ⇒ poison store was squashed
    BNE  done                  ; mismatch ⇒ store leaked through the flush (FAIL)
    LLI  R1, #1               ; PASS — the precise exception cancelled the store
done:
    BREAK
