        .text

; Bus fault ignore handler: advance EPC past the faulting instruction
; and resume. Uses R12 as scratch (saved/restored via PC-relative).
        .globl  _trap_bus_ignore
_trap_bus_ignore:
        stw     r12, [pc + trap_scratch - .]
        rdspr   r12, epc
        add     r12, 4
        wrspr   epc, r12
        ldw     r12, [pc + trap_scratch - .]
        eret
trap_scratch:
        .long   0

; Probe the last word of a page to test if it is RAM.
; R1 = page number.  Returns 1 if accessible, 0 if bus fault.
        .globl  _detect_page
_detect_page:
        ; r1 = (pagenum + 1) * 4096 - 4  => last word of page
        add     r1, 1
        shl     r1, 12
        sub     r1, 4

        ; Store, read back and compare
        li      r2, 0x12345678
        stw     r2, [r1]
        mov     r3, r0
        ldw     r3, [r1]
        cmp     r2, r3
        bne     .Lfail
        li      r1, 1
        b       .Lexit
.Lfail:
        li      r1, 0
.Lexit:
        ret
