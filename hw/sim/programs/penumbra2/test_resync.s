; test_resync.s — gen2 WRSYS context-synchronization guard.
;
; WRSYS is context-synchronizing: after it commits, the instructions behind it
; (frozen in ID/IF, fetched under the old state) are discarded and re-fetched
; from the WRSYS's successor, so they observe the new state. The re-fetch must
; preserve exactly-once execution — the discarded copy must not also commit
; (a duplicate) and the successor must not be lost (a skip).
;
; Each WRSYS here is immediately followed by an increment. With a correct
; re-fetch each increment runs exactly once (R5 = 3). A re-sync that failed to
; flush the held copy would run an increment twice (R5 > 3); one that redirected
; wrong or over-flushed would skip or corrupt it (R5 < 3 or a bad branch). The
; WRSYS targets the writable scratch device (6); only its synchronizing
; behaviour is under test, not the value written.
;
; (Without an MMU the re-fetch re-derives the same instruction, so this guards
; the mechanism's exactly-once property; re-fetch under *changed* translation is
; verified at MMU integration.) Self-checks into R1; run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                ; assume FAIL
    LLI  R5, #0                ; increment accumulator

    WRSYS R0, #6, #0           ; context-sync: the next instruction re-fetches
    ADD  R5, #1                ; must run exactly once → R5 = 1

    WRSYS R0, #6, #1
    ADD  R5, #1                ; → R5 = 2

    WRSYS R0, #6, #2
    ADD  R5, #1                ; → R5 = 3

    CMP  R5, #3                ; exactly three increments, each run once
    BNE  fail
    LLI  R1, #1               ; PASS
fail:
    BREAK
