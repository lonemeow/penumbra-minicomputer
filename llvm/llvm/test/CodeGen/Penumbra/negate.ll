; RUN: llc -mtriple=penumbra-unknown-none -O0 < %s | FileCheck %s
; RUN: llc -mtriple=penumbra-unknown-none -O1 < %s | FileCheck %s
;
; Penumbra SHL masks shift count to 5 bits (Rs[4:0]), so SHL by 32
; is a no-op (32 & 31 = 0).  MUL by -1 must not use the
; (x << 32) - x pattern — use SUB from zero instead.

define i32 @negate_mul(i32 %x) {
; CHECK-LABEL: negate_mul:
; CHECK-NOT:   lli{{.*}}, 32
; CHECK:       jmp r13
  %neg = mul i32 %x, -1
  ret i32 %neg
}

define i32 @negate_sub(i32 %x) {
; CHECK-LABEL: negate_sub:
; CHECK-NOT:   lli{{.*}}, 32
; CHECK:       jmp r13
  %neg = sub i32 0, %x
  ret i32 %neg
}

define i32 @negate_expr(i32 %x) {
; CHECK-LABEL: negate_expr:
; CHECK-NOT:   lli{{.*}}, 32
; CHECK:       jmp r13
  %inc = add i32 %x, 1
  %neg = mul i32 %inc, -1
  ret i32 %neg
}

; MUL by 2^31 + 1: should still strength-reduce (SHL 31 is valid)
define i32 @mul_pow2_31_plus1(i32 %x) {
; CHECK-LABEL: mul_pow2_31_plus1:
; CHECK:       shl{{.*}}, 31
; CHECK:       jmp r13
  %r = mul i32 %x, -2147483647  ; 2^31 + 1 (unsigned 0x80000001)
  ret i32 %r
}

; MUL by 2^31 - 1: boundary case, SHL 31 is the max valid shift
define i32 @mul_pow2_31_minus1(i32 %x) {
; CHECK-LABEL: mul_pow2_31_minus1:
; CHECK:       shl{{.*}}, 31
; CHECK-NOT:   lli{{.*}}, 32
; CHECK:       jmp r13
  %r = mul i32 %x, 2147483647  ; 2^31 - 1 (0x7FFFFFFF)
  ret i32 %r
}
