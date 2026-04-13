; RUN: llc -mtriple=penumbra-unknown-none -O0 < %s | FileCheck %s

; When a function has both local variables and dynamic alloca,
; the frame pointer (R10) must be used to access locals.
; Without FP, alloca moves SP and invalidates [SP+offset] references.

declare void @use_ptr(ptr)
declare void @use_int(i32)

define void @locals_with_alloca(i32 %n) {
; CHECK-LABEL: locals_with_alloca:
;
; Frame pointer setup: sub SP, mov R10(FP)=SP
; CHECK:       sub r14,
; CHECK-NEXT:  mov r10, r14
;
; Store to local via FP (not SP):
; CHECK:       stw {{.*}}, [r10 +
;
; After alloca, locals still accessed via R10:
; CHECK:       sub {{.*}}
; CHECK:       mov r14,
; CHECK:       ldw {{.*}}, [r10 +
;
; Epilogue: restore SP from FP
; CHECK:       mov r14, r10
entry:
  %local = alloca i32
  store i32 42, ptr %local
  %vla = alloca i8, i32 %n
  call void @use_ptr(ptr %vla)
  %val = load i32, ptr %local
  call void @use_int(i32 %val)
  ret void
}
