; RUN: llc -mtriple=penumbra -global-isel -O0 -relocation-model=pic \
; RUN:   -verify-machineinstrs < %s | FileCheck %s -check-prefix=PIC

; PIC jump tables use label-difference entries and add the JT base back.

define i32 @switch_test(i32 %x) {
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

; PIC: mov r{{[0-9]+}}, r15
; PIC: add r{{[0-9]+}}, %pcrel(.LJTI0_0+4)

; PIC: shl r{{[0-9]+}}, 2
; PIC: add r{{[0-9]+}}, r{{[0-9]+}}
; PIC: ldw r{{[0-9]+}}, [r{{[0-9]+}} + 0]
; PIC: add r{{[0-9]+}}, r{{[0-9]+}}
; PIC: jmp r{{[0-9]+}}

; Entries are label differences (position-independent):
; PIC: .LJTI0_0:
; PIC-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0
; PIC-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0
; PIC-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0
; PIC-NEXT: .word .LBB0_{{[0-9]+}}-.LJTI0_0
