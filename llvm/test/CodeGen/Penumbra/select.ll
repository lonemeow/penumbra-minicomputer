; RUN: llc -march=penumbra -global-isel -verify-machineinstrs < %s | FileCheck %s

; Test G_SELECT lowering (conditional select via branch diamond).

; Basic ternary: return (a > b) ? a : b  (i.e., max)
define i32 @select_max(i32 %a, i32 %b) {
; CHECK-LABEL: select_max:
; CHECK:       cmp r1, r2
; CHECK-NEXT:  bgt .LBB0_2
; CHECK:       mov r1, r2
; CHECK:       .LBB0_2:
; CHECK-NEXT:  jmp r13
  %cmp = icmp sgt i32 %a, %b
  %sel = select i1 %cmp, i32 %a, i32 %b
  ret i32 %sel
}

; Select with constant operands: return (x == 0) ? 1 : 0
define i32 @select_const(i32 %x) {
; CHECK-LABEL: select_const:
; CHECK:       cmp
; CHECK:       beq .LBB1_2
; CHECK:       .LBB1_2:
; CHECK:       jmp r13
  %cmp = icmp eq i32 %x, 0
  %sel = select i1 %cmp, i32 1, i32 0
  ret i32 %sel
}

; Standalone icmp materialized as select: (a < b) ? 1 : 0
define i32 @icmp_to_int(i32 %a, i32 %b) {
; CHECK-LABEL: icmp_to_int:
; CHECK:       cmp
; CHECK:       bcc
; CHECK:       jmp r13
  %cmp = icmp ult i32 %a, %b
  %sel = select i1 %cmp, i32 1, i32 0
  ret i32 %sel
}

; Unsigned select: return (a >= b) ? a : b
define i32 @select_unsigned(i32 %a, i32 %b) {
; CHECK-LABEL: select_unsigned:
; CHECK:       cmp r1, r2
; CHECK-NEXT:  bcs .LBB3_2
; CHECK:       .LBB3_2:
; CHECK-NEXT:  jmp r13
  %cmp = icmp uge i32 %a, %b
  %sel = select i1 %cmp, i32 %a, i32 %b
  ret i32 %sel
}
