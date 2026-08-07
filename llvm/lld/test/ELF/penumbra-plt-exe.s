// RUN: llvm-mc -filetype=obj -triple=penumbra %s -o %t.o
// RUN: echo '.globl bar; .type bar,@function; bar: jmp r13' | \
// RUN:   llvm-mc -filetype=obj -triple=penumbra - -o %tlib.o
// RUN: ld.lld -shared -o %tlib.so %tlib.o
// RUN: ld.lld -o %t %t.o %tlib.so
// RUN: llvm-objdump -d --section=.plt %t | FileCheck %s

// A fixed-address executable's PLT stub addresses its .got.plt slot
// absolutely: four instructions, full 32-bit reach, no PC-relative
// add.  The position-independent five-instruction form is only for
// output whose load address is unknown at link time.

// CHECK:      <.plt>:
// CHECK-NEXT: lli	r11
// CHECK-NEXT: lui	r11
// CHECK-NEXT: ldw	r11, [r11 + 0]
// CHECK-NEXT: jmp	r11
// CHECK-NEXT: lli	r11
// CHECK-NEXT: lui	r11
// CHECK-NEXT: ldw	r11, [r11 + 0]
// CHECK-NEXT: jmp	r11
// CHECK-NOT:  add{{.*}}pc

.globl _start
_start:
  bl bar
  jmp r13
