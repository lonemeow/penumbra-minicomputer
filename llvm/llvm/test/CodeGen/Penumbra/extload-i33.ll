; RUN: llc -mtriple=penumbra-unknown-netbsd -O0 -global-isel < %s | FileCheck %s

; Mixed-sign __builtin_add_overflow on equal-width inputs (size_t a,
; size_t b, ptrdiff_t *sum) lowers in clang to i33 SADD-with-overflow:
; the extra bit gives SADD enough room to detect overflow on inputs
; that were originally unsigned.  IRTranslator emits a pair of
; `load i32 + zext i32 -> i33`, and pre-legalize `combines_for_extload`
; folds those into `G_ZEXTLOAD :: load (s32) -> s33`.  The legalizer
; must clamp the s33 destination back to s32 (and let the subsequent
; SADDO chain handle the sign-fixup), otherwise it bails with
; "unable to legalize instruction".  At -O1+ InstCombine collapses
; the i33 form before MIR exists, so this only manifests at -O0.

define i32 @sumsize(i32 %a, i32 %b) {
; CHECK-LABEL: sumsize:
; CHECK:       ldw {{r[0-9]+}}, [r14
; CHECK:       ldw {{r[0-9]+}}, [r14
  %a.addr = alloca i32, align 4
  %b.addr = alloca i32, align 4
  store i32 %a, ptr %a.addr, align 4
  store i32 %b, ptr %b.addr, align 4
  %la = load i32, ptr %a.addr, align 4
  %lb = load i32, ptr %b.addr, align 4
  %ax = zext i32 %la to i33
  %bx = zext i32 %lb to i33
  %s = call { i33, i1 } @llvm.sadd.with.overflow.i33(i33 %ax, i33 %bx)
  %ov = extractvalue { i33, i1 } %s, 1
  %lo = extractvalue { i33, i1 } %s, 0
  %t = trunc i33 %lo to i32
  %r = select i1 %ov, i32 -1, i32 %t
  ret i32 %r
}

declare { i33, i1 } @llvm.sadd.with.overflow.i33(i33, i33)
