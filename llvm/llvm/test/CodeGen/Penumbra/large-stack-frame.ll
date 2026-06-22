; Large stack frames exceed the 16-bit immediate of SUBi/ADDi, and CSR
; spill offsets likewise exceed the 16-bit memory offset field.  Both
; sites must expand to LLI+LUI+reg-form, else the encoder truncates
; silently and the frame is broken (see the `wc -l /etc/passwd` bug).

; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs < %s \
; RUN:   | FileCheck %s

declare void @use(ptr)

; Small frame still uses the fast immediate-form path.
define void @small_frame() {
; CHECK-LABEL: small_frame:
; CHECK:         sub sp, {{[0-9]+}}
; CHECK-NOT:     lli r11, {{.*}}
; CHECK:         add sp, {{[0-9]+}}
; CHECK:         jmp lr
  %buf = alloca [64 x i8], align 1
  call void @use(ptr %buf)
  ret void
}

; 64 KiB array — frame does not fit in 16-bit SUBi immediate.
; Prologue materializes the size in R11 and does a register SUB; epilogue
; mirrors with ADD.  Exactly one `sub sp, r11` (prologue) and one
; `add sp, r11` (epilogue) for the stack adjustment.
define void @huge_frame() {
; CHECK-LABEL: huge_frame:
; CHECK:         lli r11, {{[0-9]+}}
; CHECK-NEXT:    lui r11, 1
; CHECK-NEXT:    sub sp, r11
; CHECK-NOT:     subi sp,
; CHECK-NOT:     sub sp, r11
; CHECK:         add sp, r11
; CHECK-NOT:     addi sp,
; CHECK-NOT:     add sp, r11
; CHECK:         jmp lr
  %buf = alloca [65536 x i8], align 1
  call void @use(ptr %buf)
  ret void
}
