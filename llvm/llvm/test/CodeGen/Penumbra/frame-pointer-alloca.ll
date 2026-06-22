; RUN: llc -mtriple=penumbra-unknown-none -O0 < %s | FileCheck %s

; When a function has both local variables and dynamic alloca,
; the frame pointer (R10) must be used to access locals.
; Without FP, alloca moves SP and invalidates [SP+offset] references.

declare void @use_ptr(ptr)
declare void @use_int(i32)

define void @locals_with_alloca(i32 %n) {
; CHECK-LABEL: locals_with_alloca:
;
; Frame pointer setup: sub SP, then CSR spills (SP-relative), then mov R10=SP.
; The R10 CSR spill MUST come before `mov r10, sp` and use [sp+...] —
; otherwise the saved R10 captures the new FP value (= SP) instead of the
; caller's value.  See frame-fp-csr-spill-order.ll for the dedicated regression.
; CHECK:       sub sp,
; CHECK:       stw r10, [sp +
; CHECK:       mov r10, sp
;
; Store to local via FP (not SP):
; CHECK:       stw {{.*}}, [r10 +
;
; After alloca, locals still accessed via R10:
; CHECK:       sub {{.*}}
; CHECK:       mov sp,
; CHECK:       ldw {{.*}}, [r10 +
;
; Epilogue: restore SP from FP, then SP-relative CSR restore.
; CHECK:       mov sp, r10
; CHECK:       ldw r10, [sp +
entry:
  %local = alloca i32
  store i32 42, ptr %local
  %vla = alloca i8, i32 %n
  call void @use_ptr(ptr %vla)
  %val = load i32, ptr %local
  call void @use_int(i32 %val)
  ret void
}
