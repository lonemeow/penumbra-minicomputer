; eth_filter.s -- the Ethernet model's receive checks under +eth=loop.
;
; Sends one frame at a time to a chosen destination and records RX_PROD
; after each, releasing whatever arrived, so each result shows whether
; that frame was received.  The expected values live in
; run_iss_tests.py.
;
; RX_PROD after a frame to: R2 = the station, R3 = another station,
; R4 = the broadcast address, R5 = a group address,
; R6 = that group address with ALLMULTI, R7 = the other station with
; PROMISC, R8 = the station with RX_EN clear, R9 = the station with a
; frame one byte over the longest allowed.  R10 = RX_DROP at the end.

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

.equ TO_BCAST_0,  0xFFFF003C   ; LEN 60, FF:FF
.equ TO_BCAST_1,  0xFFFFFFFF
.equ TO_GROUP_0,  0x0001003C   ; LEN 60, 01:00
.equ TO_GROUP_1,  0x0100005E   ; 5E:00:00:01
.equ LONG_0,      0x500205EF   ; LEN 1519, 02:50

.equ RX_TX,       3            ; CTRL: RX_EN | TX_EN
.equ TX_ONLY,     2
.equ PROMISC,     7            ; RX_EN | TX_EN | PROMISC
.equ ALLMULTI,    11           ; RX_EN | TX_EN | ALLMULTI

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
    LLI  R8, #RESULTS         ; where the next result goes

    ; The station takes the factory address, and both directions run.
    LDW  R2, [R12 + #FACTORY_LO]
    STW  R2, [R12 + #ADDR_LO]
    LDW  R2, [R12 + #FACTORY_HI]
    STW  R2, [R12 + #ADDR_HI]
    LLI  R2, #3               ; RX_EN | TX_EN
    STW  R2, [R12 + #CTRL]

    ; To the station.
    LI   R4, #TO_STATION_0
    LI   R5, #TO_STATION_1
    BL   fill
    BL   probe

    ; To another station.
    LI   R4, #TO_STATION_0
    LI   R5, #TO_OTHER_1
    BL   fill
    BL   probe

    ; To the broadcast address.
    LI   R4, #TO_BCAST_0
    LI   R5, #TO_BCAST_1
    BL   fill
    BL   probe

    ; To a group address.
    LI   R4, #TO_GROUP_0
    LI   R5, #TO_GROUP_1
    BL   fill
    BL   probe

    ; To the group address again, with ALLMULTI.
    LLI  R2, #ALLMULTI
    STW  R2, [R12 + #CTRL]
    LI   R4, #TO_GROUP_0
    LI   R5, #TO_GROUP_1
    BL   fill
    BL   probe

    ; To the other station, with PROMISC.
    LLI  R2, #PROMISC
    STW  R2, [R12 + #CTRL]
    LI   R4, #TO_STATION_0
    LI   R5, #TO_OTHER_1
    BL   fill
    BL   probe

    ; To the station, not receiving.
    LLI  R2, #TX_ONLY
    STW  R2, [R12 + #CTRL]
    LI   R4, #TO_STATION_0
    LI   R5, #TO_STATION_1
    BL   fill
    BL   probe

    ; A frame of 1519 bytes, accepting every destination.
    LLI  R2, #PROMISC
    STW  R2, [R12 + #CTRL]
    LI   R4, #LONG_0
    LI   R5, #TO_STATION_1
    BL   fill
    BL   probe

    LDW  R2, [R12 + #RX_DROP]
    STW  R2, [R8]

    ; ── Results ──────────────────────────────────────────────
    LDW  R2, [R9 + #0]
    LDW  R3, [R9 + #4]
    LDW  R4, [R9 + #8]
    LDW  R5, [R9 + #12]
    LDW  R6, [R9 + #16]
    LDW  R7, [R9 + #20]
    LDW  R8, [R9 + #24]
    LDW  R10, [R9 + #32]
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

; Send the frame in the transmit slots, wait for the device to finish
; with it, record RX_PROD at R8 and release whatever arrived.
probe:
    LDW  R6, [R12 + #TX_PROD]
    ADD  R6, #1
    STW  R6, [R12 + #TX_PROD]
probe_wait:
    LDW  R7, [R12 + #TX_CONS]
    CMP  R7, R6
    BNE  probe_wait
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R8]
    ADD  R8, #4
    STW  R2, [R12 + #RX_CONS]
    JMP  R13
