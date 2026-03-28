; Minimal WRSYS test — write to MMUCR, check R1=1 on success
_start:
    LLI  R2, #0x0500         ; value to write
    WRSYS R2, #0, #0          ; write to MMUCR
    LLI  R1, #1              ; if we get here, WRSYS didn't crash
done:
    B    done
