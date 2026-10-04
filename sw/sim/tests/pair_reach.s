; pair_reach.s -- LLI+LUI pairs placed for the fuser's reach.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  The
; program starts on a 16-byte line, so each instruction's word in its
; line is known:
;
;   words 0-1  LLI R5; LUI R5   an aligned pair in one line
;   word  2    DI
;   words 3-4  LLI R6; LUI R6   straddles two lines
;   words 5-6  LLI R7; LUI R7   in one line, the first at an odd word
;   word  7    DI
;   word  8    BREAK
;
; reach:any fuses all three, reach:line the first and third, reach:pair
; the first alone.

_start:
    LLI  R5, #1
    LUI  R5, #2
    DI
    LLI  R6, #1
    LUI  R6, #2
    LLI  R7, #1
    LUI  R7, #2
    DI
    BREAK
