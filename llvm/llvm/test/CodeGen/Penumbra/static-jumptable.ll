; RUN: llc -mtriple=penumbra -global-isel -O0 -relocation-model=static \
; RUN:   -verify-machineinstrs < %s | FileCheck %s

; Static-mode jump tables should also use label-difference entries
; (same as PIC), placed inline in .text.  This avoids dynamic
; relocations in PIE and enables future 16-bit entry optimization.

define i32 @switch_static(i32 %x) {
entry:
  switch i32 %x, label %default [
    i32 0, label %bb0
    i32 1, label %bb1
    i32 2, label %bb2
    i32 3, label %bb3
  ]
bb0: ret i32 10
bb1: ret i32 20
bb2: ret i32 30
bb3: ret i32 40
default: ret i32 -1
}

; Base materialized via LLI+LUI (static):
; CHECK: lli r{{[0-9]+}}, %lo16(.LJTI0_0)
; CHECK: lui r{{[0-9]+}}, %hi16(.LJTI0_0)

; BRJT always adds base back (even in static mode):
; CHECK: add r{{[0-9]+}}, r{{[0-9]+}}
; CHECK: ldw r{{[0-9]+}}, [r{{[0-9]+}} + 0]
; CHECK: add r{{[0-9]+}}, r{{[0-9]+}}
; CHECK: jmp r{{[0-9]+}}

; Entries are label differences (not absolute):
; CHECK: .LJTI0_0:
; CHECK-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0
; CHECK-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0
; CHECK-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0
; CHECK-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0

; Jump table must NOT be in .rodata:
; CHECK-NOT: .section{{.*}}.rodata
