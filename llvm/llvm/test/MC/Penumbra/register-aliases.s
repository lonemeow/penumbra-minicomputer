// RUN: llvm-mc -triple=penumbra -show-encoding < %s | FileCheck %s

// The ABI register aliases (declared as AltNames in PenumbraRegisterInfo.td)
// are accepted as register operands and resolve to the same registers as the
// canonical rN spellings.  The instruction printer always emits the canonical
// name, so each alias below prints as its rN form.

// CHECK: mov r0, r1
mov zero, r1

// CHECK: mov r12, r1
mov tp, r1

// CHECK: mov r13, r14
mov lr, sp

// CHECK: add r0, r15
add zero, pc
