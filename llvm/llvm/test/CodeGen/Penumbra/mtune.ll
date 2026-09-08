; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs < %s \
; RUN:   | FileCheck %s
; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs -mcpu=generic \
; RUN:   < %s 2>&1 | FileCheck %s --implicit-check-not="not a recognized"
; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs -mcpu=penumbra1 \
; RUN:   < %s 2>&1 | FileCheck %s --implicit-check-not="not a recognized"
; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs -mcpu=penumbra2 \
; RUN:   < %s 2>&1 | FileCheck %s --implicit-check-not="not a recognized"

; Every generation implements the same ISA, so a processor name selects tuning
; and nothing else: each one must accept the module, and no name may change
; what is legal.  While no tuning feature is declared they all emit identical
; code, which is what these shared CHECK lines pin -- the first feature to
; declare a real difference is expected to split this file.
;
; llc has no -mtune, so the per-function route that -mtune actually travels
; (clang writes it as a "tune-cpu" attribute) is exercised by the attributed
; functions below rather than by a command line.

define i32 @untuned(i32 %a, i32 %b) {
; CHECK-LABEL: untuned:
; CHECK:         add r1, r2
; CHECK-NEXT:    jmp lr
  %result = add i32 %a, %b
  ret i32 %result
}

define i32 @tuned_for_penumbra1(i32 %a, i32 %b) #0 {
; CHECK-LABEL: tuned_for_penumbra1:
; CHECK:         add r1, r2
; CHECK-NEXT:    jmp lr
  %result = add i32 %a, %b
  ret i32 %result
}

define i32 @tuned_for_penumbra2(i32 %a, i32 %b) #1 {
; CHECK-LABEL: tuned_for_penumbra2:
; CHECK:         add r1, r2
; CHECK-NEXT:    jmp lr
  %result = add i32 %a, %b
  ret i32 %result
}

; A function tuned for one generation next to a function tuned for another has
; to reach a different subtarget for each, so the cache may not collide them.
define i32 @tuned_for_generic(i32 %a, i32 %b) #2 {
; CHECK-LABEL: tuned_for_generic:
; CHECK:         add r1, r2
; CHECK-NEXT:    jmp lr
  %result = add i32 %a, %b
  ret i32 %result
}

attributes #0 = { "tune-cpu"="penumbra1" }
attributes #1 = { "tune-cpu"="penumbra2" }
attributes #2 = { "tune-cpu"="generic" }
