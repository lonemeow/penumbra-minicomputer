; RUN: llc -march=penumbra -global-isel -verify-machineinstrs < %s | FileCheck %s

; Test G_ICMP + G_BRCOND folding into CMP + Bcc, G_BR into B,
; and G_PHI selection at control-flow joins.

; Signed comparison: sgt -> BGT
define i32 @max(i32 %a, i32 %b) {
; CHECK-LABEL: max:
; CHECK:       cmp r1, r2
; CHECK-NEXT:  bgt .LBB0_2
; CHECK:       mov r1, r2
; CHECK:       .LBB0_2:
; CHECK-NEXT:  jmp r13
entry:
  %cmp = icmp sgt i32 %a, %b
  br i1 %cmp, label %if.then, label %if.else

if.then:
  br label %return

if.else:
  br label %return

return:
  %retval = phi i32 [ %a, %if.then ], [ %b, %if.else ]
  ret i32 %retval
}

; Unconditional branch.
define i32 @always_return(i32 %a) {
; CHECK-LABEL: always_return:
; CHECK:       jmp r13
entry:
  br label %done

done:
  ret i32 %a
}

; Unsigned comparison: ugt -> BHI
define i32 @unsigned_max(i32 %a, i32 %b) {
; CHECK-LABEL: unsigned_max:
; CHECK:       cmp r1, r2
; CHECK-NEXT:  bhi .LBB2_2
; CHECK:       mov r1, r2
; CHECK:       .LBB2_2:
; CHECK-NEXT:  jmp r13
entry:
  %cmp = icmp ugt i32 %a, %b
  br i1 %cmp, label %if.then, label %if.else

if.then:
  br label %return

if.else:
  br label %return

return:
  %retval = phi i32 [ %a, %if.then ], [ %b, %if.else ]
  ret i32 %retval
}
