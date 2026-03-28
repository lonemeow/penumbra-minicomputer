; Simplified sysreg test — MMUCR round-trip
; R1 starts at 0xBEEF, RDSYS should overwrite with 0x0500
_start:
    LLI  R1, #0xBEEF         ; known marker value
    LLI  R2, #0x0500         ; value to write to MMUCR
    WRSYS R2, #0, #0          ; MMUCR = 0x0500
    RDSYS R1, #0, #0          ; R1 = MMUCR readback (expect 0x0500)
done:
    B    done
