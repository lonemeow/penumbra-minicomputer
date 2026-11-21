; test_usbhc_enum.s — Enumerate the CLASS_USBHC device end to end
; REQUIRES: usbhc bus
;
; The path the boot ROM's keyboard bring-up will take: autoconfig the
; chain to find the USB host controller, power the port, watch a device
; connect at full speed, drive a bus reset, then read the 18-byte device
; descriptor with a control transfer — SETUP, three IN data chunks
; (8+8+2 through the 8-byte endpoint zero), and the closing zero-length
; OUT status. Every completion is reaped through the W1C IRQ_STATUS, and
; every result field (RESULT, RXLEN, RXTOGGLE) is checked.
;
; Result: R1=1 PASS, R1=0 FAIL. On FAIL the runner's register dump
; identifies the failure:
;   R10   — stage marker: 1 CAP, 2 connect/speed, 3 reset/enable,
;           4 SETUP, 5/6 IN1 status/data, 7/8 IN2, 9/10 IN3,
;           11 status stage, 12 SET_ADDRESS, 13 old address silent,
;           14 bounded re-read at the new address, 15 unknown request,
;           16 config-descriptor probe, 17 full config read,
;           18 SET_CONFIGURATION, 19 string read ending in a ZLP
;   R6    — the masked XFER_STATUS a transaction check compared
;   R2/R3 — the got/want pair of the failing compare
;   R8/R9 — trap vector marker / faulting PC, if a trap fired

.equ CFG_WIN,     0xFE000000
.equ ACFG_CLASS,  0x00
.equ ACFG_BASE,   0x1C
.equ SPI_BASE,    0xFD000000
.equ USB_BASE,    0xFD001000

.equ CAP,         0x00
.equ IRQ_STATUS,  0x04
.equ PORT_STATUS, 0x0C
.equ PORT_CTRL,   0x10
.equ TOKEN,       0x18
.equ XFER_CTRL,   0x1C
.equ XFER_STATUS, 0x20
.equ DATA_W0,     0x40
.equ DATA_W1,     0x44

_start:
    LLI  R1, #0               ; assume fail

    ; ── Any trap is a hard FAIL: record it and stop ──────────
    ; A stray fault must not wander off through RAM — the handlers
    ; leave a vector marker in R8 and the faulting PC in R9.
    LA   R2, trap_bus
    LLI  R3, #0x00            ; VEC_BUS_FAULT slot
    STW  R2, [R3]
    LA   R2, trap_illegal
    LLI  R3, #0x1C            ; VEC_ILLEGAL slot
    STW  R2, [R3]
    LA   R2, trap_align
    LLI  R3, #0x20            ; VEC_ALIGN slot
    STW  R2, [R3]

    ; ── Autoconfig: SPI heads the chain, the USBHC follows ───
    LLI  R4, #2               ; BUSCTL.CFG_EN
    WRSYS R4, #4, #0
    LI   R12, #CFG_WIN
    LI   R2, #SPI_BASE        ; park the SPI controller out of the way
    STW  R2, [R12 + #ACFG_BASE]
    LLI  R4, #0               ; the CFG_EN toggle advances the chain
    WRSYS R4, #4, #0
    LLI  R4, #2
    WRSYS R4, #4, #0

    LDW  R2, [R12 + #ACFG_CLASS]
    CMP  R2, #8               ; CLASS_USBHC
    BNE  fail
    LI   R2, #USB_BASE
    STW  R2, [R12 + #ACFG_BASE]
    LLI  R4, #0               ; chain done; leave the window closed
    WRSYS R4, #4, #0

    ; ── CAP: version 1, 64-byte buffer, LS + FS ──────────────
    LLI  R10, #1              ; stage: CAP
    LI   R12, #USB_BASE
    LDW  R2, [R12 + #CAP]
    LI   R3, #0x00034001
    CMP  R2, R3
    BNE  fail

    ; ── Power the port, wait for the connect, check the speed ─
    LLI  R10, #2              ; stage: connect/speed
    LLI  R2, #1               ; PORT_CTRL.POWER
    STW  R2, [R12 + #PORT_CTRL]
wait_connect:
    LDW  R2, [R12 + #PORT_STATUS]
    LLI  R3, #1               ; CONNECT
    TEST R2, R3
    BZ   wait_connect
    LLI  R3, #0x30            ; SPEED field
    AND  R2, R3
    CMP  R2, #0x20            ; 2 = full speed
    BNE  fail

    ; ── Bus reset: drive, see it active, release, port enables ─
    LLI  R10, #3              ; stage: reset/enable
    ; (Real firmware times the >=10 ms hold; the port logic itself
    ; has no minimum, so the test releases as soon as it's seen.)
    LLI  R2, #3               ; POWER | RESET
    STW  R2, [R12 + #PORT_CTRL]
wait_reset:
    LDW  R2, [R12 + #PORT_STATUS]
    LLI  R3, #4               ; RESET_ACTIVE
    TEST R2, R3
    BZ   wait_reset
    LLI  R2, #1               ; release the reset
    STW  R2, [R12 + #PORT_CTRL]
wait_enabled:
    LDW  R2, [R12 + #PORT_STATUS]
    LLI  R3, #2               ; ENABLED
    TEST R2, R3
    BZ   wait_enabled

    LLI  R2, #7               ; clear any pending sources
    STW  R2, [R12 + #IRQ_STATUS]

    ; ── SETUP: GET_DESCRIPTOR(device), wLength = 18 ──────────
    LLI  R10, #4              ; stage: SETUP
    LI   R2, #0x01000680      ; 80 06 00 01 (little-endian word)
    STW  R2, [R12 + #DATA_W0]
    LI   R2, #0x00120000      ; 00 00 12 00
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0               ; TOKEN: SETUP, addr 0, ep 0, toggle 0
    LI   R3, #0x00010008      ; XFER_CTRL: LENGTH 8 | START
    BL   do_xfer
    LLI  R3, #0x0E            ; RESULT field
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail

    ; ── IN chunk 1: descriptor bytes 0-7, toggle 1 ───────────
    LLI  R10, #5              ; stage: IN1 status
    LI   R2, #0x00010002      ; TOKEN: IN, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E          ; RESULT + RXTOGGLE + RXLEN
    AND  R6, R3
    LI   R3, #0x0810          ; ACK, toggle 1, 8 bytes
    CMP  R6, R3
    BNE  fail
    LLI  R10, #6              ; stage: IN1 data
    LDW  R2, [R12 + #DATA_W0]
    LI   R3, #0x01100112      ; 12 01 10 01
    CMP  R2, R3
    BNE  fail
    LDW  R2, [R12 + #DATA_W1]
    LI   R3, #0x08000000      ; 00 00 00 08
    CMP  R2, R3
    BNE  fail

    ; ── IN chunk 2: bytes 8-15, toggle 0 ─────────────────────
    LLI  R10, #7              ; stage: IN2 status
    LLI  R2, #2               ; TOKEN: IN, toggle 0
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0800          ; ACK, toggle 0, 8 bytes
    CMP  R6, R3
    BNE  fail
    LLI  R10, #8              ; stage: IN2 data
    LDW  R2, [R12 + #DATA_W0]
    LI   R3, #0x00011234      ; 34 12 01 00
    CMP  R2, R3
    BNE  fail
    LDW  R2, [R12 + #DATA_W1]
    LI   R3, #0x02010100      ; 00 01 01 02
    CMP  R2, R3
    BNE  fail

    ; ── IN chunk 3: bytes 16-17, toggle 1 ────────────────────
    LLI  R10, #9              ; stage: IN3 status
    LI   R2, #0x00010002
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0210          ; ACK, toggle 1, 2 bytes
    CMP  R6, R3
    BNE  fail
    LLI  R10, #10             ; stage: IN3 data
    LDW  R2, [R12 + #DATA_W0]
    LI   R3, #0xFFFF          ; RXLEN is 2: only two bytes meaningful
    AND  R2, R3
    LI   R3, #0x0100          ; 00 01
    CMP  R2, R3
    BNE  fail

    ; ── Status stage: zero-length OUT, toggle 1 ──────────────
    LLI  R10, #11             ; stage: status OUT
    LI   R2, #0x00010001      ; TOKEN: OUT, toggle 1
    LI   R3, #0x00010000      ; LENGTH 0 | START
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail

    ; ── SET_ADDRESS(5): no-data transfer, status stage is IN ─
    LLI  R10, #12             ; stage: SET_ADDRESS
    LI   R2, #0x00050500      ; 00 05 05 00 (little-endian word)
    STW  R2, [R12 + #DATA_W0]
    LLI  R2, #0
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0               ; TOKEN: SETUP, addr 0
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail
    LI   R2, #0x00010002      ; status: IN, addr 0, toggle 1
    LI   R3, #0x00010000      ; LENGTH 0 | START
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0010          ; ACK, ZLP with toggle 1
    CMP  R6, R3
    BNE  fail

    ; ── The old address must now be dead: silence, not data ──
    LLI  R10, #13             ; stage: old address silent
    LI   R2, #0x00010002      ; IN, addr 0
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0x06            ; TIMEOUT
    BNE  fail

    ; ── Bounded re-read at address 5: wLength = 8 exactly ────
    LLI  R10, #14             ; stage: bounded re-read
    LI   R2, #0x01000680      ; GET_DESCRIPTOR(device)...
    STW  R2, [R12 + #DATA_W0]
    LI   R2, #0x00080000      ; ...but wLength = 8 this time
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0x50            ; TOKEN: SETUP, addr 5
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail
    LI   R2, #0x00010052      ; IN, addr 5, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0810          ; ACK, toggle 1, exactly 8 bytes
    CMP  R6, R3
    BNE  fail
    LDW  R2, [R12 + #DATA_W0]
    LI   R3, #0x01100112
    CMP  R2, R3
    BNE  fail
    LI   R2, #0x00010051      ; status: OUT, addr 5, toggle 1
    LI   R3, #0x00010000      ; the transfer must already be complete
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail

    ; ── An unimplemented (vendor) request answers STALL ──────
    LLI  R10, #15             ; stage: unknown request
    LI   R2, #0x000042C0      ; C0 42 00 00 (vendor, device-to-host)
    STW  R2, [R12 + #DATA_W0]
    LI   R2, #0x00040000      ; wLength = 4
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0x50            ; TOKEN: SETUP, addr 5
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; the setup stage itself always ACKs
    BNE  fail
    LI   R2, #0x00010052      ; IN, addr 5, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0x04            ; STALL
    BNE  fail

    ; ── Config-descriptor probe: header only, wLength = 9 ────
    LLI  R10, #16             ; stage: config probe
    LI   R2, #0x02000680      ; 80 06 00 02 (GET_DESCRIPTOR config)
    STW  R2, [R12 + #DATA_W0]
    LI   R2, #0x00090000      ; wLength = 9
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0x50            ; TOKEN: SETUP, addr 5
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail
    LI   R2, #0x00010052      ; IN, addr 5, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0810          ; ACK, toggle 1, 8 bytes
    CMP  R6, R3
    BNE  fail
    LDW  R2, [R12 + #DATA_W0]
    LI   R3, #0x00120209      ; 09 02 12 00 — wTotalLength = 18
    CMP  R2, R3
    BNE  fail
    LI   R2, #0x00000052      ; IN, addr 5, toggle 0
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0100          ; ACK, toggle 0, 1 byte
    CMP  R6, R3
    BNE  fail
    LI   R2, #0x00010051      ; status: OUT, addr 5, toggle 1
    LI   R3, #0x00010000
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail

    ; ── Full config read: 8+8+2 through EP0, both descriptors ─
    LLI  R10, #17             ; stage: full config read
    LI   R2, #0x02000680      ; GET_DESCRIPTOR(config)...
    STW  R2, [R12 + #DATA_W0]
    LI   R2, #0x00120000      ; ...wLength = wTotalLength = 18
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0x50            ; TOKEN: SETUP, addr 5
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail
    LI   R2, #0x00010052      ; IN, addr 5, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0810          ; ACK, toggle 1, 8 bytes
    CMP  R6, R3
    BNE  fail
    LDW  R2, [R12 + #DATA_W1]
    LI   R3, #0x80000101      ; 01 01 00 80 — one interface, bus-powered
    CMP  R2, R3
    BNE  fail
    LI   R2, #0x00000052      ; IN, addr 5, toggle 0
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0800          ; ACK, toggle 0, 8 bytes
    CMP  R6, R3
    BNE  fail
    LDW  R2, [R12 + #DATA_W0]
    LI   R3, #0x00040932      ; 32 09 04 00 — interface descriptor head
    CMP  R2, R3
    BNE  fail
    LI   R2, #0x00010052      ; IN, addr 5, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0210          ; ACK, toggle 1, 2 bytes
    CMP  R6, R3
    BNE  fail
    LI   R2, #0x00010051      ; status: OUT, addr 5, toggle 1
    LI   R3, #0x00010000
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail

    ; ── SET_CONFIGURATION(1): no-data, status stage is IN ────
    LLI  R10, #18             ; stage: SET_CONFIGURATION
    LI   R2, #0x00010900      ; 00 09 01 00 (little-endian word)
    STW  R2, [R12 + #DATA_W0]
    LLI  R2, #0
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0x50            ; TOKEN: SETUP, addr 5
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail
    LI   R2, #0x00010052      ; status: IN, addr 5, toggle 1
    LI   R3, #0x00010000
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0010          ; ACK, ZLP with toggle 1
    CMP  R6, R3
    BNE  fail

    ; ── String read over-long: exact-multiple data ends in ZLP ─
    ; The 16-byte product string against wLength 255: chunks 8+8
    ; leave no short packet, so the transfer must terminate with a
    ; zero-length DATA1.
    LLI  R10, #19             ; stage: string ZLP read
    LI   R2, #0x03020680      ; 80 06 02 03 (GET_DESCRIPTOR string 2)
    STW  R2, [R12 + #DATA_W0]
    LI   R2, #0x00FF0000      ; wLength = 255
    STW  R2, [R12 + #DATA_W1]
    LLI  R2, #0x50            ; TOKEN: SETUP, addr 5
    LI   R3, #0x00010008
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail
    LI   R2, #0x00010052      ; IN, addr 5, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0810          ; ACK, toggle 1, 8 bytes
    CMP  R6, R3
    BNE  fail
    LDW  R2, [R12 + #DATA_W0]
    LI   R3, #0x00530310      ; 10 03 'S' 00 — string header + text
    CMP  R2, R3
    BNE  fail
    LI   R2, #0x00000052      ; IN, addr 5, toggle 0
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0800          ; ACK, toggle 0, 8 bytes
    CMP  R6, R3
    BNE  fail
    LI   R2, #0x00010052      ; IN, addr 5, toggle 1
    LI   R3, #0x00010008
    BL   do_xfer
    LI   R3, #0x7F1E
    AND  R6, R3
    LI   R3, #0x0010          ; ACK, toggle 1, zero bytes — the ZLP
    CMP  R6, R3
    BNE  fail
    LI   R2, #0x00010051      ; status: OUT, addr 5, toggle 1
    LI   R3, #0x00010000
    BL   do_xfer
    LLI  R3, #0x0E
    AND  R6, R3
    CMP  R6, #0               ; ACK
    BNE  fail

    LLI  R1, #1
fail:
    BREAK

trap_bus:
    LLI  R8, #0xB
    RDSPR R9, EPC
    LLI  R1, #0
    BREAK
trap_illegal:
    LLI  R8, #0x1
    RDSPR R9, EPC
    LLI  R1, #0
    BREAK
trap_align:
    LLI  R8, #0xA
    RDSPR R9, EPC
    LLI  R1, #0
    BREAK

; ═══════════════════════════════════════════════════════════════
; do_xfer — launch one transaction and reap its completion
; ═══════════════════════════════════════════════════════════════
; Input:  R2 = TOKEN value, R3 = XFER_CTRL value (LENGTH | START)
;         R12 = controller base
; Output: R6 = XFER_STATUS (XFER_DONE already cleared via W1C)
; Clobbers: R7
do_xfer:
    STW  R2, [R12 + #TOKEN]
    STW  R3, [R12 + #XFER_CTRL]
xfer_poll:
    LDW  R6, [R12 + #IRQ_STATUS]
    LLI  R7, #1               ; XFER_DONE
    TEST R6, R7
    BZ   xfer_poll
    STW  R7, [R12 + #IRQ_STATUS]
    LDW  R6, [R12 + #XFER_STATUS]
    RET
