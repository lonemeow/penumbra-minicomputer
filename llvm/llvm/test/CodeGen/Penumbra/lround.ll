; RUN: llc -mtriple=penumbra-unknown-none -O0 < %s | FileCheck %s

; lround/llround/lrint/llrint must legalize to libm libcalls
; (no FPU on Penumbra).

define i32 @lround_f64(double %x) {
; CHECK-LABEL: lround_f64:
; CHECK:       bl lround
  %r = call i32 @llvm.lround.i32.f64(double %x)
  ret i32 %r
}

define i32 @lround_f32(float %x) {
; CHECK-LABEL: lround_f32:
; CHECK:       bl lroundf
  %r = call i32 @llvm.lround.i32.f32(float %x)
  ret i32 %r
}

define i64 @llround_f64(double %x) {
; CHECK-LABEL: llround_f64:
; CHECK:       bl llround
  %r = call i64 @llvm.llround.i64.f64(double %x)
  ret i64 %r
}

define i64 @llround_f32(float %x) {
; CHECK-LABEL: llround_f32:
; CHECK:       bl llroundf
  %r = call i64 @llvm.llround.i64.f32(float %x)
  ret i64 %r
}

define i32 @lrint_f64(double %x) {
; CHECK-LABEL: lrint_f64:
; CHECK:       bl lrint
  %r = call i32 @llvm.lrint.i32.f64(double %x)
  ret i32 %r
}

define i32 @lrint_f32(float %x) {
; CHECK-LABEL: lrint_f32:
; CHECK:       bl lrintf
  %r = call i32 @llvm.lrint.i32.f32(float %x)
  ret i32 %r
}

define i64 @llrint_f64(double %x) {
; CHECK-LABEL: llrint_f64:
; CHECK:       bl llrint
  %r = call i64 @llvm.llrint.i64.f64(double %x)
  ret i64 %r
}

define i64 @llrint_f32(float %x) {
; CHECK-LABEL: llrint_f32:
; CHECK:       bl llrintf
  %r = call i64 @llvm.llrint.i64.f32(float %x)
  ret i64 %r
}

declare i32 @llvm.lround.i32.f64(double)
declare i32 @llvm.lround.i32.f32(float)
declare i64 @llvm.llround.i64.f64(double)
declare i64 @llvm.llround.i64.f32(float)
declare i32 @llvm.lrint.i32.f64(double)
declare i32 @llvm.lrint.i32.f32(float)
declare i64 @llvm.llrint.i64.f64(double)
declare i64 @llvm.llrint.i64.f32(float)
