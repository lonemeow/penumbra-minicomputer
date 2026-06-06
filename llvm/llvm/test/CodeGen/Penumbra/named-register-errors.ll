; A named global register must name a real register and must be reserved
; (one the allocator never touches); otherwise codegen would silently clobber
; the pinned value.  getRegisterByName rejects both cases with a fatal error.

; RUN: split-file %s %t
; RUN: not llc -mtriple=penumbra -global-isel %t/allocatable.ll -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ALLOC
; RUN: not llc -mtriple=penumbra -global-isel %t/invalid.ll -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=INVALID

; ALLOC: named global register "r5" is allocatable
; INVALID: invalid register name "r99"

;--- allocatable.ll
define i32 @use_allocatable() {
  %v = call i32 @llvm.read_register.i32(metadata !0)
  ret i32 %v
}
declare i32 @llvm.read_register.i32(metadata)
!0 = !{!"r5"}

;--- invalid.ll
define i32 @use_invalid() {
  %v = call i32 @llvm.read_register.i32(metadata !0)
  ret i32 %v
}
declare i32 @llvm.read_register.i32(metadata)
!0 = !{!"r99"}
