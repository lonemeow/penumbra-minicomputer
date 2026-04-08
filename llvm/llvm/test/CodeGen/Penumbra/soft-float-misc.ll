; RUN: llc -mtriple=penumbra-unknown-none -O0 < %s | FileCheck %s

; Soft-float operations that lower to integer bit manipulation or constants.

define double @fneg_f64(double %x) {
; CHECK-LABEL: fneg_f64:
; CHECK:       xor
  %r = fneg double %x
  ret double %r
}

define float @fabs_f32(float %x) {
; CHECK-LABEL: fabs_f32:
; CHECK:       and
  %r = call float @llvm.fabs.f32(float %x)
  ret float %r
}

define double @fcopysign_f64(double %mag, double %sign) {
; CHECK-LABEL: fcopysign_f64:
; CHECK:       and
; CHECK:       or
  %r = call double @llvm.copysign.f64(double %mag, double %sign)
  ret double %r
}

define i32 @get_rounding() {
; CHECK-LABEL: get_rounding:
; CHECK:       lli r1, 1
  %r = call i32 @llvm.get.rounding()
  ret i32 %r
}

declare float @llvm.fabs.f32(float)
declare double @llvm.copysign.f64(double, double)
declare i32 @llvm.get.rounding()
