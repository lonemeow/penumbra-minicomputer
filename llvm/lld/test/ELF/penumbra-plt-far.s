// RUN: llvm-mc -filetype=obj -triple=penumbra %s -o %t.o
// RUN: ld.lld -shared -o %t.so %t.o
// RUN: llvm-objdump -d --section=.plt %t.so | FileCheck %s

// The PLT stub must reach a .got.plt arbitrarily far away: the .data
// block below pushes it ~200 KB past .plt, beyond any pair of 16-bit
// immediates.  Every stub builds the full 32-bit displacement with
// LLI/LUI before the PC-relative add — a short-form stub would load
// its function pointer from inside .plt itself and jump to
// instruction bytes.

// CHECK:      <.plt>:
// CHECK-NEXT: lli	r11
// CHECK-NEXT: lui	r11
// CHECK-NEXT: add	r11, pc
// CHECK-NEXT: ldw	r11, [r11 + 0]
// CHECK-NEXT: jmp	r11
// CHECK-NEXT: lli	r11
// CHECK-NEXT: lui	r11
// CHECK-NEXT: add	r11, pc
// CHECK-NEXT: ldw	r11, [r11 + 0]
// CHECK-NEXT: jmp	r11

.text
.globl foo
.type foo,@function
foo:
  bl bar
  jmp r13
.size foo, . - foo

.data
.space 200000
