; RUN: llc -mtriple=penumbra-unknown-none -O0 < %s | FileCheck %s

; Signed overflow intrinsics: G_SADDO, G_SSUBO lowered to add/sub + icmp.

declare {i32, i1} @llvm.sadd.with.overflow.i32(i32, i32)
declare {i32, i1} @llvm.ssub.with.overflow.i32(i32, i32)

define i32 @sadd_overflow(i32 %a, i32 %b) {
; CHECK-LABEL: sadd_overflow:
; CHECK:       add
  %r = call {i32, i1} @llvm.sadd.with.overflow.i32(i32 %a, i32 %b)
  %val = extractvalue {i32, i1} %r, 0
  ret i32 %val
}

define i32 @ssub_overflow(i32 %a, i32 %b) {
; CHECK-LABEL: ssub_overflow:
; CHECK:       sub
  %r = call {i32, i1} @llvm.ssub.with.overflow.i32(i32 %a, i32 %b)
  %val = extractvalue {i32, i1} %r, 0
  ret i32 %val
}
