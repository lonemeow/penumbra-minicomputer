; RUN: opt -passes=div-rem-pairs -S -mtriple=penumbra < %s | FileCheck %s

; The divmul unit computes quotient and remainder in one operation
; (DIV_P/DIVU_P), reported to the mid-end as TTI hasDivRemOp.  DivRemPairs
; must therefore hoist a cross-block div/rem pair adjacent — where the
; GISel pre-legalizer combiner fuses it into a single G_SDIVREM/G_UDIVREM
; (see test/CodeGen/Penumbra/divrem.ll) — and must not decompose the
; remainder into mul+sub, which would cost a second divmul-unit operation.

define i32 @srem_hoist(i32 %a, i32 %b, i1 %c) {
; CHECK-LABEL: @srem_hoist(
; CHECK:       entry:
; CHECK-NEXT:    %div = sdiv i32 %a, %b
; CHECK-NEXT:    %rem = srem i32 %a, %b
; CHECK-NOT:     mul
; CHECK-NOT:     sub
entry:
  %div = sdiv i32 %a, %b
  br i1 %c, label %if, label %end

if:
  %rem = srem i32 %a, %b
  br label %end

end:
  %ret = phi i32 [ %div, %entry ], [ %rem, %if ]
  ret i32 %ret
}

define i32 @urem_hoist(i32 %a, i32 %b, i1 %c) {
; CHECK-LABEL: @urem_hoist(
; CHECK:       entry:
; CHECK-NEXT:    %div = udiv i32 %a, %b
; CHECK-NEXT:    %rem = urem i32 %a, %b
; CHECK-NOT:     mul
; CHECK-NOT:     sub
entry:
  %div = udiv i32 %a, %b
  br i1 %c, label %if, label %end

if:
  %rem = urem i32 %a, %b
  br label %end

end:
  %ret = phi i32 [ %div, %entry ], [ %rem, %if ]
  ret i32 %ret
}
