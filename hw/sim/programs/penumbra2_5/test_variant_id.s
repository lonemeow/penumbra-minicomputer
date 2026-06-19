; test_variant_id.s — gen2.5 self-identifies through the cpuid name.
;
; Lives only under programs/penumbra2_5/, so it runs solely on the
; gen2.5 build (penumbra2's inherited suite never reaches it). The
; baseline gen2 reports CPU_NAME2 = "/2\0\0"; gen2.5 reports "/2.5".
; Passing proves the CPU_VARIANT parameter routes from the build
; (-D PENUMBRA_CPU_VARIANT) through machine_penumbra2 into cpuid.
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0                ; assume fail

    ; ── cpuid name word 2 = "/2.5" (LE: '/' '2' '.' '5' = 0x352E322F) ──
    RDSYS R2, #CPU, #CPU_NAME2
    LI    R3, #0x352E322F
    CMP   R2, R3
    BNE   fail

    ; ── ISA register is identical across the family (v1, no features) ──
    RDSYS R2, #CPU, #CPU_ISA
    CMPI  R2, #1
    BNE   fail

    LLI  R1, #1                ; PASS
fail:
    BREAK
