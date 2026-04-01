; RUN: llc -march=penumbra -global-isel -verify-machineinstrs < %s | FileCheck %s

; Test sign/zero extension and immediate shift constant folding.

; SEXT i8 → i32: SHL 24 + SAR 24
define i32 @sext_i8(i8 %x) {
; CHECK-LABEL: sext_i8:
; CHECK:       shl r1, 24
; CHECK-NEXT:  sar r1, 24
; CHECK-NEXT:  jmp r13
  %r = sext i8 %x to i32
  ret i32 %r
}

; SEXT i16 → i32: SHL 16 + SAR 16
define i32 @sext_i16(i16 %x) {
; CHECK-LABEL: sext_i16:
; CHECK:       shl r1, 16
; CHECK-NEXT:  sar r1, 16
; CHECK-NEXT:  jmp r13
  %r = sext i16 %x to i32
  ret i32 %r
}

; ZEXT i8 → i32: AND with 0xFF (via constant in register)
define i32 @zext_i8(i8 %x) {
; CHECK-LABEL: zext_i8:
; CHECK:       lli {{r[0-9]+}}, 255
; CHECK:       and r1,
; CHECK:       jmp r13
  %r = zext i8 %x to i32
  ret i32 %r
}

; ZEXT i1 → i32: AND with 1
define i32 @zext_i1(i1 %x) {
; CHECK-LABEL: zext_i1:
; CHECK:       lli {{r[0-9]+}}, 1
; CHECK:       and r1,
; CHECK:       jmp r13
  %r = zext i1 %x to i32
  ret i32 %r
}

; Shift by constant folds to immediate form.
define i32 @shift_const(i32 %x) {
; CHECK-LABEL: shift_const:
; CHECK:       shl r1, 4
; CHECK-NEXT:  jmp r13
  %r = shl i32 %x, 4
  ret i32 %r
}
