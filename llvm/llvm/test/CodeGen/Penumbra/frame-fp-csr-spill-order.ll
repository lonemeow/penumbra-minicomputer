; RUN: llc -mtriple=penumbra-unknown-none -O2 < %s | FileCheck %s

; Regression: when a function has both alloca (so hasFP=true → R10 used as
; frame pointer) AND clobbers callee-saved R10, the prologue MUST spill the
; caller's R10 to its CSR slot BEFORE setting up the new FP.  Otherwise the
; spill captures the new FP value (= SP) and the function returns with R10
; pointing into its own deallocated frame instead of the caller's value.
;
; The original symptom was a SEGV loop in PID-1 init at PC=0 because
; ld.elf_so's `_rtld()` cached `&_rtld_objmain` in R10 across a call to
; `_rtld_objmain_sym` (which uses `alloca` via the `_rtld_donelist_init`
; macro), and the buggy callee returned with R10 pointing at its own
; saved-SP slot instead.
;
; Required prologue order (after fix):
;   1. sub sp, frame_size       — allocate frame
;   2. stw r10, [sp + N]        — save caller's R10 to CSR slot, SP-relative
;   3. (other CSR spills, all SP-relative)
;   4. mov r10, sp              — set up new FP
;
; And the symmetric required epilogue order:
;   1. mov sp, r10              — restore SP from FP
;   2. ldw r10, [sp + N]        — restore caller's R10 from CSR slot, SP-relative
;   3. (other CSR restores, SP-relative)
;   4. add sp, frame_size

declare void @callee(ptr, ptr)

; Force R5..R10 to all be live across a call so they're all CSR-spilled,
; and use alloca so hasFP becomes true.
define i32 @fp_with_csrs(i32 %n, i32 %a, i32 %b, i32 %c, i32 %d) {
; CHECK-LABEL: fp_with_csrs:
;
; Prologue: SUB SP first, then SP-relative R10 spill, then FP setup.
; The R10 spill MUST come before `mov r10, sp`.  The CSR slot MUST be
; addressed as [sp + ...] (SP), not [r10 + ...] (FP) — at the time of
; the spill R10 still holds the caller's value, not the new FP.
;
; CHECK:         sub sp, {{[0-9]+}}
; CHECK:         stw r10, [sp + {{[0-9]+}}]
; CHECK-NOT:     mov r10, sp
; CHECK:         mov r10, sp
;
; CHECK-NOT:     stw r10, [r10 +
;
; Epilogue: restore SP from FP, then SP-relative R10 restore.
; CHECK:         mov sp, r10
; CHECK:         ldw r10, [sp + {{[0-9]+}}]
; CHECK-NOT:     ldw r10, [r10 +
; CHECK:         add sp, {{[0-9]+}}
; CHECK:         jmp lr
entry:
  %vla = alloca i32, i32 %n
  %local = alloca i32
  store i32 %a, ptr %local
  call void @callee(ptr %vla, ptr %local)

  ; Hand-crafted use of all callee-saved registers via inline asm so the
  ; codegen MUST spill R5–R10 (and R13) at function entry, exercising the
  ; CSR-spill ordering relative to the FP setup.
  %sum = call i32 asm sideeffect
    "add $0, $1\0Aadd $0, $2\0Aadd $0, $3\0Aadd $0, $4",
    "=&r,r,r,r,r,~{r5},~{r6},~{r7},~{r8},~{r9},~{r10}"
    (i32 %a, i32 %b, i32 %c, i32 %d)
  %v = load i32, ptr %local
  %r = add i32 %sum, %v
  ret i32 %r
}
