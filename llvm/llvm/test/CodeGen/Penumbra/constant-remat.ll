; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs < %s \
; RUN:   | FileCheck %s
; RUN: llc -mtriple=penumbra -global-isel -stop-after=instruction-select < %s \
; RUN:   | FileCheck %s --check-prefix=ISEL

; A constant too wide for one LLI or LLIS is selected to a single PseudoLI,
; not an LLI + LUI chain, so the register allocator can recompute it.  Here
; 0x12345678 is needed before and after a call while ten arguments are live
; across it, exhausting the callee-saved registers: it is rebuilt after
; "bl clobber" instead of spilled.  Pressure-sensitive by design, like
; global-addr-remat.ll: this guards the capability, not the policy.

declare void @clobber(i32)

; ISEL-LABEL: name: remat
; ISEL:       PseudoLI 305419896
; ISEL-NOT:   LUI
define i32 @remat(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e,
                  i32 %f, i32 %h, i32 %i, i32 %j, i32 %k) {
; CHECK-LABEL: remat:
; CHECK:       lli r1, 22136
; CHECK-NEXT:  lui r1, 4660
; CHECK-NEXT:  bl clobber
; CHECK:       lli r{{[0-9]+}}, 22136
; CHECK-NEXT:  lui r{{[0-9]+}}, 4660
  call void @clobber(i32 305419896)
  %s1 = add i32 %a, %b
  %s2 = add i32 %s1, %c
  %s3 = add i32 %s2, %d
  %s4 = add i32 %s3, %e
  %s5 = add i32 %s4, %f
  %s6 = add i32 %s5, %h
  %s7 = add i32 %s6, %i
  %s8 = add i32 %s7, %j
  %s9 = add i32 %s8, %k
  %t  = add i32 %s9, 305419896
  ret i32 %t
}
