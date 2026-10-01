; audio_status.s -- the audio device's readiness under +audio_status.
;
; Autoconfigures the chain to reach the audio device, reads CAP, queues
; two frames on channel 0, then reads STATUS three times with LEVEL on
; either side.  A device that is not up must hold the frames, so LEVEL
; shows whether they were consumed.  The expected values live in
; run_iss_tests.py.
;
; R2 = CAP, R3..R5 = STATUS on the first to third read,
; R6 = channel 0 LEVEL after queueing, R7 = LEVEL after the reads,
; R8 = the class of the fourth device in the chain (audio's).

.equ CFG_WIN,     0xFE000000
.equ ACFG_CLASS,  0x00
.equ ACFG_BASE,   0x1C
.equ SPI_BASE,    0xFD000000
.equ USB_BASE,    0xFD001000
.equ AUDIO_BASE,  0xFD002000
.equ AUDIO_CHAN0, 0xFD002100
.equ DISP_BASE,   0xFD040000

.equ CAP,         0x00
.equ STATUS,      0x20
.equ C_LEVEL,     0x10
.equ C_FIFO,      0x18

_start:
    ; ── Autoconfig: SPI, USB and the display head the chain ──
    LLI  R4, #2               ; BUSCTL.CFG_EN
    WRSYS R4, #4, #0
    LI   R12, #CFG_WIN

    LI   R2, #SPI_BASE
    STW  R2, [R12 + #ACFG_BASE]
    LLI  R4, #0               ; the CFG_EN toggle advances the chain
    WRSYS R4, #4, #0
    LLI  R4, #2
    WRSYS R4, #4, #0

    LI   R2, #USB_BASE
    STW  R2, [R12 + #ACFG_BASE]
    LLI  R4, #0
    WRSYS R4, #4, #0
    LLI  R4, #2
    WRSYS R4, #4, #0

    LI   R2, #DISP_BASE
    STW  R2, [R12 + #ACFG_BASE]
    LLI  R4, #0
    WRSYS R4, #4, #0
    LLI  R4, #2
    WRSYS R4, #4, #0

    LDW  R8, [R12 + #ACFG_CLASS]
    LI   R2, #AUDIO_BASE
    STW  R2, [R12 + #ACFG_BASE]
    LLI  R4, #0               ; chain done; leave the window closed
    WRSYS R4, #4, #0

    ; ── The device ───────────────────────────────────────────
    LI   R12, #AUDIO_BASE
    LI   R13, #AUDIO_CHAN0
    LDW  R2, [R12 + #CAP]
    LI   R9, #0x00010001
    STW  R9, [R13 + #C_FIFO]
    STW  R9, [R13 + #C_FIFO]
    LDW  R6, [R13 + #C_LEVEL]
    LDW  R3, [R12 + #STATUS]
    LDW  R4, [R12 + #STATUS]
    LDW  R5, [R12 + #STATUS]
    LDW  R7, [R13 + #C_LEVEL]
    BREAK
