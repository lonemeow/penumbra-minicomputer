; core_identity.s -- what the ISS presents for its +core generation.
;
; Reads the CPU name's generation word and each cache's INFO, and checks
; that a cache's CTRL reads back the enable software wrote.  The expected
; values live in run_iss_tests.py, derived there from the RTL parameters.
;
; R2 = CPU_NAME2, R3 = L1-I INFO, R4 = L1-D INFO, R5 = L2 INFO,
; R6 = L2 CTRL out of reset, R7 = L2 CTRL after enabling it.

_start:
    RDSYS R2, #CPU, #CPU_NAME2
    RDSYS R3, #ICACHE, #CACHE_INFO
    RDSYS R4, #DCACHE, #CACHE_INFO
    RDSYS R5, #L2, #CACHE_INFO
    RDSYS R6, #L2, #CACHE_CTRL
    LLI  R8, #1
    WRSYS R8, #L2, #CACHE_CTRL
    RDSYS R7, #L2, #CACHE_CTRL
    BREAK
