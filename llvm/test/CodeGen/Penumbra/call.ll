; RUN: llc -march=penumbra -global-isel -verify-machineinstrs < %s | FileCheck %s

; Test function call lowering (lowerCall).

declare i32 @bar(i32)
declare i32 @add3(i32, i32, i32)

; Simple passthrough call.
define i32 @passthrough(i32 %x) {
; CHECK-LABEL: passthrough:
; CHECK:       bl bar
; CHECK-NEXT:  jmp r13
  %y = call i32 @bar(i32 %x)
  ret i32 %y
}

; Multiple arguments stay in R1-R3.
define i32 @multi_arg(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: multi_arg:
; CHECK:       bl add3
; CHECK-NEXT:  jmp r13
  %r = call i32 @add3(i32 %a, i32 %b, i32 %c)
  ret i32 %r
}

; Value must survive across call — requires callee-saved spill.
define i32 @needs_save(i32 %a, i32 %b) {
; CHECK-LABEL: needs_save:
; CHECK:       sub r14, 4
; CHECK:       stw r5, [r14 + 0]
; CHECK:       bl bar
; CHECK:       add r5, r1
; CHECK:       ldw r5, [r14 + 0]
; CHECK:       add r14, 4
; CHECK:       jmp r13
  %y = call i32 @bar(i32 %b)
  %z = add i32 %a, %y
  ret i32 %z
}

; Void call — no return value collection.
define void @void_call(i32 %x) {
; CHECK-LABEL: void_call:
; CHECK:       bl bar
; CHECK-NEXT:  jmp r13
  call void @bar(i32 %x)
  ret void
}
