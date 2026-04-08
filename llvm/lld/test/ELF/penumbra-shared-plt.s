// RUN: llvm-mc -filetype=obj -triple=penumbra %s -o %t.o
// RUN: ld.lld -shared -o %t.so %t.o
// RUN: llvm-readobj -r --dyn-relocations %t.so | FileCheck %s

// Test that shared library linking produces correct PLT/GOT relocations.

.text
.globl foo
.type foo,@function
foo:
  bl bar
  jmp r13
.size foo, . - foo

// CHECK: R_PENUMBRA_JUMP_SLOT bar
