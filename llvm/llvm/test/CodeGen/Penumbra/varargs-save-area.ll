; RUN: llc -mtriple=penumbra -global-isel -stop-after=irtranslator < %s \
; RUN:   | FileCheck %s

; A variadic function's prologue stores R1-R4 into the register save area,
; so no frame object over the area may be immutable: a load from an
; immutable object counts as invariant and could be scheduled ahead of the
; store that fills it.  The frame objects here are the four save slots and
; the start of the anonymous arguments, which lies inside the save area.

declare void @llvm.va_start(ptr)
declare void @llvm.va_end(ptr)

define i32 @first_vararg(i32 %named, ...) {
; CHECK-LABEL: name: first_vararg
; CHECK:       fixedStack:
; CHECK-COUNT-5: isImmutable: false
; CHECK-NOT:   isImmutable: true
; CHECK:       {{^}}stack:
  %vl = alloca ptr, align 4
  call void @llvm.va_start(ptr %vl)
  %arg = va_arg ptr %vl, i32
  call void @llvm.va_end(ptr %vl)
  ret i32 %arg
}
