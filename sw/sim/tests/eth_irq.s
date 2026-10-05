; eth_irq.s -- the Ethernet model's interrupt sources under +eth=loop.
;
; IRQ_STATUS has two bits, each a condition: RX (bit 0) while the
; receive ring holds a frame not yet released, TX (bit 1) while the
; transmit ring holds fewer frames than TX_WM.  The program reads the
; register around each change that should move a bit, takes one
; interrupt through the external vector, and then runs the frame counts
; past 65535 to check the comparison where they wrap.  The transmit
; ring has two slots.  The expected values live in run_iss_tests.py.
;
; IRQ_STATUS: R2 = idle with TX_WM 0, R3 = idle with TX_WM 1,
; R4 = just after queueing two frames with TX_WM 2, R5 = once both have
; been sent and have looped back, R6 = after releasing them with TX_WM 0,
; R7 = just after queueing two frames with TX_WM 3.
; R8 = IRQ_STATUS as the interrupt handler saw it, R9 = after it.
; With TX_CONS at 65535 and two frames queued, so TX_PROD is 1:
; R10 = IRQ_STATUS with TX_WM 2, R11 = with TX_WM 3.

.equ CFG_WIN,     0xFE000000
.equ ACFG_CLASS,  0x00
.equ ACFG_BASE,   0x1C
.equ SPI_BASE,    0xFD000000
.equ USB_BASE,    0xFD001000
.equ AUDIO_BASE,  0xFD002000
.equ DISP_BASE,   0xFD040000
.equ ETH_BASE,    0xFD080000
.equ ETH_TX,      0xFD088000   ; the transmit slot aperture
.equ ETH_RX,      0xFD090000   ; the receive slot aperture
.equ SLOT,        2048         ; transmit slot 1, from ETH_TX
.equ SLOT_WORD1,  2052         ; and its second word
.equ RESULTS,     0x1000

.equ CAP,         0x00
.equ IRQ_STATUS,  0x04
.equ IRQ_ENABLE,  0x08
.equ CTRL,        0x0C
.equ LINK,        0x10
.equ ADDR_LO,     0x18
.equ ADDR_HI,     0x1C
.equ FACTORY_LO,  0x20
.equ FACTORY_HI,  0x24
.equ RX_GEOM,     0x40
.equ RX_PROD,     0x44
.equ RX_CONS,     0x48
.equ RX_DROP,     0x4C
.equ TX_GEOM,     0x50
.equ TX_PROD,     0x54
.equ TX_CONS,     0x58
.equ TX_WM,       0x5C

; The first two words of a slot: LEN in the low half of the first, then
; the destination address.  STATION is the model's factory address,
; 02:50:45:4E:00:01, and OTHER differs from it in the last byte.
.equ TO_STATION_0, 0x5002003C  ; LEN 60, 02:50
.equ TO_STATION_1, 0x01004E45  ; 45:4E:00:01
.equ TO_OTHER_1,   0x02004E45  ; 45:4E:00:02

.equ VEC_EXT_IRQ, 0x24         ; vector 9
.equ SEEN,        24           ; the handler's result, from RESULTS
.equ IRQ_RX,      1

_start:
    ; ── Autoconfig: four devices head the chain ──────────────
    LLI  R4, #2               ; BUSCTL.CFG_EN
    WRSYS R4, #4, #0
    LI   R12, #CFG_WIN
    LI   R2, #SPI_BASE
    BL   place
    LI   R2, #USB_BASE
    BL   place
    LI   R2, #DISP_BASE
    BL   place
    LI   R2, #AUDIO_BASE
    BL   place
    LDW  R3, [R12 + #ACFG_CLASS]
    LI   R2, #ETH_BASE
    BL   place
    LLI  R4, #0               ; chain done; leave the window closed
    WRSYS R4, #4, #0

    LI   R12, #ETH_BASE
    LI   R11, #ETH_TX
    LI   R10, #ETH_RX
    LLI  R9, #RESULTS

    ; The station takes the factory address, and both directions run.
    LDW  R2, [R12 + #FACTORY_LO]
    STW  R2, [R12 + #ADDR_LO]
    LDW  R2, [R12 + #FACTORY_HI]
    STW  R2, [R12 + #ADDR_HI]
    LLI  R2, #3               ; RX_EN | TX_EN
    STW  R2, [R12 + #CTRL]

    LI   R4, #TO_STATION_0
    LI   R5, #TO_STATION_1
    BL   fill

    ; ── Idle ─────────────────────────────────────────────────
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #0]
    LLI  R2, #1
    STW  R2, [R12 + #TX_WM]
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #4]

    ; ── A full transmit ring, then both frames back ──────────
    LLI  R2, #2
    STW  R2, [R12 + #TX_WM]
    LLI  R3, #2
    BL   queue
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #8]
    BL   drain
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #12]

    STW  R0, [R12 + #TX_WM]
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R12 + #RX_CONS]
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #16]

    ; ── A threshold above the slot count holds TX asserted ───
    LLI  R2, #3
    STW  R2, [R12 + #TX_WM]
    LLI  R3, #2
    BL   queue
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #20]
    BL   drain
    STW  R0, [R12 + #TX_WM]
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R12 + #RX_CONS]

    ; ── One interrupt, for a received frame ──────────────────
    STW  R0, [R9 + #SEEN]
    LA   R2, #handler
    STW  R2, [R0 + #VEC_EXT_IRQ]
    LLI  R2, #IRQ_RX
    STW  R2, [R12 + #IRQ_ENABLE]
    EI
    LLI  R3, #1
    BL   queue
    LI   R5, #100000          ; give up rather than hang if none comes
irq_wait:
    LDW  R2, [R9 + #SEEN]
    CMP  R2, #0
    BNE  irq_taken
    SUB  R5, #1
    BNE  irq_wait
irq_taken:
    DI
    STW  R0, [R12 + #IRQ_ENABLE]
    BL   drain
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #28]
    LDW  R2, [R12 + #RX_PROD] ; release, in case no interrupt did
    STW  R2, [R12 + #RX_CONS]

    ; ── The counts past 65535 ────────────────────────────────
    ; Frames to another station, so nothing arrives, one at a time
    ; until TX_PROD and TX_CONS both stand at 65535.
    LI   R4, #TO_STATION_0
    LI   R5, #TO_OTHER_1
    BL   fill
    LLI  R3, #1
wrap:
    BL   queue
    BL   drain
    CMP  R6, #0xFFFF
    BNE  wrap

    LLI  R2, #2
    STW  R2, [R12 + #TX_WM]
    LLI  R3, #2
    BL   queue                ; TX_PROD wraps to 1
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #32]
    LLI  R2, #3
    STW  R2, [R12 + #TX_WM]
    LDW  R2, [R12 + #IRQ_STATUS]
    STW  R2, [R9 + #36]

    ; ── Results ──────────────────────────────────────────────
    LDW  R2, [R9 + #0]
    LDW  R3, [R9 + #4]
    LDW  R4, [R9 + #8]
    LDW  R5, [R9 + #12]
    LDW  R6, [R9 + #16]
    LDW  R7, [R9 + #20]
    LDW  R8, [R9 + #24]
    LDW  R10, [R9 + #32]
    LDW  R11, [R9 + #36]
    LDW  R9, [R9 + #28]
    BREAK

; The external interrupt: record IRQ_STATUS, then release the receive
; ring, which ends the RX condition.  Uses R8 alone.
handler:
    LDW  R8, [R12 + #IRQ_STATUS]
    STW  R8, [R9 + #SEEN]
    LDW  R8, [R12 + #RX_PROD]
    STW  R8, [R12 + #RX_CONS]
    ERET

; Give the device in the config window the base in R2, and advance the
; chain to the next device.
place:
    STW  R2, [R12 + #ACFG_BASE]
    LLI  R4, #0
    WRSYS R4, #4, #0
    LLI  R4, #2
    WRSYS R4, #4, #0
    JMP  R13

; Write R4 and R5 as the first two words of both transmit slots, so the
; next frame is the same whichever slot TX_PROD names.
fill:
    STW  R4, [R11 + #0]
    STW  R5, [R11 + #4]
    STW  R4, [R11 + #SLOT]
    STW  R5, [R11 + #SLOT_WORD1]
    JMP  R13

; Queue R3 frames in one write to TX_PROD.  Leaves the new TX_PROD in R6.
queue:
    LDW  R6, [R12 + #TX_PROD]
    ADD  R6, R3
    AND  R6, #0xFFFF
    STW  R6, [R12 + #TX_PROD]
    JMP  R13

; Wait until the device has finished with every frame up to R6.
drain:
    LDW  R7, [R12 + #TX_CONS]
    CMP  R7, R6
    BNE  drain
    JMP  R13
