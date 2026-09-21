; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,FASTMUL
; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs -mcpu=generic \
; RUN:   < %s 2>&1 | FileCheck %s --check-prefixes=CHECK,FASTMUL \
; RUN:   --implicit-check-not="not a recognized"
; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs -mcpu=penumbra1 \
; RUN:   < %s 2>&1 | FileCheck %s --check-prefixes=CHECK,SLOWMUL \
; RUN:   --implicit-check-not="not a recognized"
; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs -mcpu=penumbra2 \
; RUN:   < %s 2>&1 | FileCheck %s --check-prefixes=CHECK,FASTMUL \
; RUN:   --implicit-check-not="not a recognized"
; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs \
; RUN:   -mcpu=penumbra2_5 < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefixes=CHECK,FASTMUL \
; RUN:   --implicit-check-not="not a recognized"

; Every generation implements the same ISA, so a processor name selects tuning
; and nothing else: each one must accept the module, and no name may change
; what is legal.  The CHECK lines pin code every name emits identically.
;
; The FASTMUL and SLOWMUL lines pin a divide by a constant, which the tuned
; core's scheduling model decides: a core whose multiply is sufficiently
; cheaper than its divide takes the magic-number rewrite, and penumbra1, which
; multiplies as slowly as it divides, keeps the divide.  "generic" is defined
; as the most-used variant's tuning (penumbra2_5), so generic -- including a
; run that names no -mcpu -- takes FASTMUL.
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

define i32 @tuned_for_penumbra2_5(i32 %a, i32 %b) #3 {
; CHECK-LABEL: tuned_for_penumbra2_5:
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

define i32 @udiv10_untuned(i32 %a) {
; CHECK-LABEL: udiv10_untuned:
; FASTMUL:       mulu
; FASTMUL-NOT:   divu
; SLOWMUL:       divu r1, r2
; SLOWMUL-NOT:   mulu
; CHECK:         jmp lr
  %result = udiv i32 %a, 10
  ret i32 %result
}

; A "tune-cpu" attribute overrides the command line, so these three decide the
; same way under every RUN line -- and generic decides as penumbra2_5 does.
define i32 @udiv10_tuned_for_penumbra1(i32 %a) #0 {
; CHECK-LABEL: udiv10_tuned_for_penumbra1:
; CHECK:         divu r1, r2
; CHECK-NEXT:    jmp lr
  %result = udiv i32 %a, 10
  ret i32 %result
}

define i32 @udiv10_tuned_for_penumbra2_5(i32 %a) #3 {
; CHECK-LABEL: udiv10_tuned_for_penumbra2_5:
; CHECK:         mulu r2, r1, r1
; CHECK-NOT:     divu
; CHECK:         jmp lr
  %result = udiv i32 %a, 10
  ret i32 %result
}

define i32 @udiv10_tuned_for_generic(i32 %a) #2 {
; CHECK-LABEL: udiv10_tuned_for_generic:
; CHECK:         mulu r2, r1, r1
; CHECK-NOT:     divu
; CHECK:         jmp lr
  %result = udiv i32 %a, 10
  ret i32 %result
}

attributes #0 = { "tune-cpu"="penumbra1" }
attributes #1 = { "tune-cpu"="penumbra2" }
attributes #2 = { "tune-cpu"="generic" }
attributes #3 = { "tune-cpu"="penumbra2_5" }
