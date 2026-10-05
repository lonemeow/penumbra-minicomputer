; eth_loop.s -- the Ethernet model's rings under +eth=loop,rx=4,tx=2.
;
; Autoconfigures the chain to reach the Ethernet adapter, reads what it
; reports, then sends frames to its own station address.  The loop
; backend hands each back to the receive side, so the receive ring
; fills: four slots hold four frames, further ones are dropped and
; counted, and releasing slots lets frames in again.  The expected
; values live in run_iss_tests.py.
;
; R2 = the class of the fifth device in the chain, R3 = CAP,
; R4 = RX_GEOM, R5 = TX_GEOM, R6 = LINK,
; R7 = the first word of receive slot 0 after one 20-byte frame,
; R8 = RX_PROD and R9 = RX_DROP after six frames with none released,
; R10 = RX_PROD after releasing two and sending a seventh,
; R11 = TX_CONS at the end.

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

.equ SHORT_0,     0x50020014   ; LEN 20, 02:50

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
    STW  R3, [R9 + #0]
    LDW  R2, [R12 + #CAP]
    STW  R2, [R9 + #4]
    LDW  R2, [R12 + #RX_GEOM]
    STW  R2, [R9 + #8]
    LDW  R2, [R12 + #TX_GEOM]
    STW  R2, [R9 + #12]
    LDW  R2, [R12 + #LINK]
    STW  R2, [R9 + #16]

    ; The station takes the factory address, and both directions run.
    LDW  R2, [R12 + #FACTORY_LO]
    STW  R2, [R12 + #ADDR_LO]
    LDW  R2, [R12 + #FACTORY_HI]
    STW  R2, [R12 + #ADDR_HI]
    LLI  R2, #3               ; RX_EN | TX_EN
    STW  R2, [R12 + #CTRL]

    ; ── One short frame: the device pads it to 60 bytes ──────
    LI   R4, #SHORT_0
    LI   R5, #TO_STATION_1
    BL   fill
    LLI  R3, #1
    BL   queue
    BL   drain
    LDW  R2, [R10 + #0]
    STW  R2, [R9 + #20]

    ; ── Five more, none released: the ring holds four ────────
    LLI  R8, #5
more:
    BL   queue
    BL   drain
    SUB  R8, #1
    BNE  more
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R9 + #24]
    LDW  R2, [R12 + #RX_DROP]
    STW  R2, [R9 + #28]

    ; ── Releasing two slots lets the next frame in ───────────
    LLI  R2, #2
    STW  R2, [R12 + #RX_CONS]
    BL   queue
    BL   drain
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R9 + #32]
    LDW  R2, [R12 + #TX_CONS]
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
