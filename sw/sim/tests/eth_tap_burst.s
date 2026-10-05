; eth_tap_burst.s -- a burst from the host through +eth=tap.
;
; Sends a broadcast frame carrying "BRST", which asks eth_tap_peer.py
; for eight frames back to back, twice what the receive ring holds.
; The host hands them over at once, but the backend lets them arrive
; only as fast as the link carries them: at the 10 Mbit/s this runs at,
; one every 1,680 instructions.  So the guest's first sight of a frame
; is of one frame, the ring then fills, and the other four are dropped
; and counted.  The expected values live in run_iss_tests.py.
;
; R2 = RX_PROD when a frame is first seen, R3 = RX_PROD and
; R4 = RX_DROP once four frames have been dropped.

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


; The frame sent: to everyone, from the station, type 0x88B5 (the
; local experimental one), carrying "BRST".
.equ TO_ALL_0,    0xFFFF003C   ; LEN 60, FF:FF
.equ TO_ALL_1,    0xFFFFFFFF
.equ FROM_0,      0x4E455002   ; 02:50:45:4E
.equ FROM_1_TYPE, 0xB5880100   ; 00:01, then 88 B5
.equ PAYLOAD,     0x54535242   ; "BRST"

; How long to wait for the answer, in turns of a four-instruction loop.
; The host answers within milliseconds; this is seconds.
.equ PATIENCE,    25000000

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

    ; ── One frame out ────────────────────────────────────────
    LI   R2, #TO_ALL_0
    STW  R2, [R11 + #0]
    LI   R2, #TO_ALL_1
    STW  R2, [R11 + #4]
    LI   R2, #FROM_0
    STW  R2, [R11 + #8]
    LI   R2, #FROM_1_TYPE
    STW  R2, [R11 + #12]
    LI   R2, #PAYLOAD
    STW  R2, [R11 + #16]
    LLI  R2, #1
    STW  R2, [R12 + #TX_PROD]

    ; ── The first frame of the burst ─────────────────────────
    LI   R5, #PATIENCE
first:
    LDW  R2, [R12 + #RX_PROD]
    CMP  R2, #0
    BNE  rest
    SUB  R5, #1
    BNE  first

    ; ── The rest: three fill the ring, four find it full ─────
rest:
    LI   R5, #PATIENCE
drops:
    LDW  R4, [R12 + #RX_DROP]
    CMP  R4, #4
    BEQ  done
    SUB  R5, #1
    BNE  drops
done:
    LDW  R3, [R12 + #RX_PROD]
    LDW  R4, [R12 + #RX_DROP]
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
