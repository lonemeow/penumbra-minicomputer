; eth_mdio.s -- an Ethernet adapter whose PHY software manages.
;
; Under +eth=loop,mdio the device has a PHY at management address 1,
; reached through the MDIO register, and runs at whatever speed
; software writes to CTRL.SPEED: frames pass only while that is the
; speed the PHY's link settled on.  The device here carries 10 and
; 100 Mbit/s, and the PHY, a gigabit one, first settles on 1000, which
; the device cannot follow.  The program reads the PHY, shows that no
; frame passes, has the PHY negotiate again without its gigabit modes,
; shows that none passes while it negotiates, and then shows frames
; passing at 100 and not at 10.  The expected values live in
; run_iss_tests.py.
;
; R2 = CAP, R3 = LINK.  MDIO after reading register 2: R4 = at an
; address with no PHY, R5 = at the PHY's.  R6 = MDIO's BUSY bit just
; after a transaction starts.  RX_PROD after a frame sent to the
; station: R7 = with the link at 1000, R8 = after one sent while the
; PHY negotiated and one with the link at 100 and CTRL.SPEED saying so,
; R9 = with CTRL.SPEED at 10.  R10 = MDIO after reading the PHY's
; status with the link at 100, R11 = TX_CONS.

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


.equ MDIO,        0x14

; MDIO words: the PHY's address in bits 28:24, the register in 20:16,
; the value in 15:0, and bit 30 to write.
.equ RD_NOBODY_ID, 0x00020000  ; address 0, register 2
.equ RD_ID,        0x01020000  ; address 1, register 2
.equ RD_STATUS,    0x01010000  ; address 1, register 1
.equ WR_NO_GIGABIT, 0x41090000 ; register 9: advertise no 1000 Mbit/s mode
.equ WR_RENEGOTIATE, 0x41001200 ; register 0: negotiation on, start again
.equ STATUS_LINK, 4            ; the status register's link bit
.equ BUSY,        0x80000000

.equ AT_100,      0x103        ; CTRL: RX_EN | TX_EN, SPEED 1
.equ AT_10,       0x003        ; CTRL: RX_EN | TX_EN, SPEED 0

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
    LDW  R2, [R12 + #CAP]
    STW  R2, [R9 + #0]
    LDW  R2, [R12 + #LINK]
    STW  R2, [R9 + #4]

    ; ── The management register ──────────────────────────────
    LI   R4, #RD_NOBODY_ID
    BL   mdio
    STW  R2, [R9 + #8]
    LI   R4, #RD_ID
    BL   mdio
    STW  R2, [R9 + #12]
    LI   R4, #RD_STATUS
    STW  R4, [R12 + #MDIO]
    LDW  R2, [R12 + #MDIO]
    LI   R3, #BUSY
    AND  R2, R3
    STW  R2, [R9 + #16]
    BL   settle

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
    LLI  R2, #AT_100
    STW  R2, [R12 + #CTRL]

    ; ── The link at 1000: nothing passes ─────────────────────
    BL   link
    LLI  R3, #1
    BL   queue
    BL   drain
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R9 + #20]

    ; ── Negotiated again without gigabit: the link at 100 ────
    LI   R4, #WR_NO_GIGABIT
    BL   mdio
    LI   R4, #WR_RENEGOTIATE
    BL   mdio
    LLI  R3, #1               ; sent with the link down: lost
    BL   queue
    BL   drain
    BL   link
    STW  R2, [R9 + #32]
    LLI  R3, #1
    BL   queue
    BL   drain
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R9 + #24]

    ; ── The device at 10 on a link at 100: nothing passes ────
    LLI  R2, #AT_10
    STW  R2, [R12 + #CTRL]
    LLI  R3, #1
    BL   queue
    BL   drain
    LDW  R2, [R12 + #RX_PROD]
    STW  R2, [R9 + #28]
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

; One management transaction: write R4 to MDIO, wait for it to finish,
; and leave the register's value in R2.  BUSY is the sign bit.
mdio:
    STW  R4, [R12 + #MDIO]
settle:
    LDW  R2, [R12 + #MDIO]
    CMP  R2, #0
    BLT  settle
    JMP  R13

; Wait for the PHY to report a link, and leave MDIO, holding its status
; register, in R2.  Calls mdio, so the return address waits in R8.
link:
    LLI  R8, #0
    OR   R8, R13
link_poll:
    LI   R4, #RD_STATUS
    BL   mdio
    TEST R2, #STATUS_LINK
    BEQ  link_poll
    JMP  R8

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
