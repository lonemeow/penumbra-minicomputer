; RUN: llc -mtriple=penumbra-unknown-netbsd -O2 -stop-after=finalize-isel < %s \
; RUN:   | FileCheck %s
; RUN: llc -mtriple=penumbra-unknown-netbsd -O2 -relocation-model=pic \
; RUN:   -stop-after=finalize-isel < %s | FileCheck %s --check-prefix=PIC

; An SR definition is dead unless an instruction reads the flags before the
; next definition.  The instructions here are the ones the selector builds by
; hand; the machine passes that move or merge instructions leave one alone
; unless its SR definition is marked dead.

@g = external global i32

declare void @use(ptr)

; Nothing reads a multiply's flags.
define i32 @mul(i32 %a, i32 %b) {
; CHECK-LABEL: name: mul
; CHECK: MUL %{{[0-9]+}}, %{{[0-9]+}}, implicit-def dead $sr
  %r = mul i32 %a, %b
  ret i32 %r
}

; The low word's carry feeds the ADC, whose own flags nothing reads.
define i64 @add64(i64 %a, i64 %b) {
; CHECK-LABEL: name: add64
; CHECK: ADD %{{[0-9]+}}, %{{[0-9]+}}, implicit-def $sr
; CHECK-NEXT: ADC %{{[0-9]+}}, %{{[0-9]+}}, implicit-def dead $sr, implicit $sr
  %r = add i64 %a, %b
  ret i64 %r
}

; A compare's value is read out of the carry; the instructions that read it
; out write flags of their own, which are dead.
define i32 @ult_value(i32 %a, i32 %b) {
; CHECK-LABEL: name: ult_value
; CHECK: CMP %{{[0-9]+}}, %{{[0-9]+}}, implicit-def $sr
; CHECK: SBC %{{[0-9]+}}, $r0, implicit-def dead $sr, implicit $sr
; CHECK-NEXT: ANDi %{{[0-9]+}}, 1, implicit-def dead $sr
  %c = icmp ult i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; The difference an equality test is built from is compared against zero, so
; the subtract's own flags are dead and the compare's are read.
define i32 @eq_value(i32 %a, i32 %b) {
; CHECK-LABEL: name: eq_value
; CHECK: SUB %{{[0-9]+}}, %{{[0-9]+}}, implicit-def dead $sr
; CHECK-NEXT: CMP $r0, %{{[0-9]+}}, implicit-def $sr
; CHECK: ADC %{{[0-9]+}}, $r0, implicit-def dead $sr, implicit $sr
  %c = icmp eq i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; A compare beside its branch stays live.
define i32 @branch(i32 %a, i32 %b) {
; CHECK-LABEL: name: branch
; CHECK: CMP %{{[0-9]+}}, %{{[0-9]+}}, implicit-def $sr
; CHECK-NEXT: BC{{[CS]}} %bb.{{[0-9]+}}, implicit $sr
entry:
  %c = icmp ult i32 %a, %b
  br i1 %c, label %then, label %else
then:
  ret i32 1
else:
  ret i32 2
}

; A two-word compare: CMPC continues the flags CMP left, and the branch of
; the select expansion reads the result.
define i32 @select64(i64 %a, i64 %b, i32 %x, i32 %y) {
; CHECK-LABEL: name: select64
; CHECK: CMP %{{[0-9]+}}, %{{[0-9]+}}, implicit-def $sr
; CHECK-NEXT: CMPC %{{[0-9]+}}, %{{[0-9]+}}, implicit-def $sr, implicit $sr
; CHECK-NEXT: BLT %bb.{{[0-9]+}}, implicit $sr
  %c = icmp slt i64 %a, %b
  %r = select i1 %c, i32 %x, i32 %y
  ret i32 %r
}

; A frame address used as a value is an add, and the callee leaves whatever
; flags it likes.
define void @frame_address() {
; CHECK-LABEL: name: frame_address
; CHECK: LEAfi %stack.0.a, 0, implicit-def dead $sr
; CHECK: BL @use, {{.*}}implicit-def dead $sr
  %a = alloca [4 x i32]
  call void @use(ptr %a)
  ret void
}

; The PC anchor of a GOT address.
define i32 @got_load() {
; PIC-LABEL: name: got_load
; PIC: PICADDPC %{{[0-9]+}}, {{[0-9]+}}, implicit-def dead $sr
  %v = load i32, ptr @g
  ret i32 %v
}
