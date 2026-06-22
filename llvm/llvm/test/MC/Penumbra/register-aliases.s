// RUN: llvm-mc -triple=penumbra -show-encoding < %s | FileCheck %s
// RUN: llvm-mc -triple=penumbra -show-encoding -penumbra-numeric-reg-names < %s \
// RUN:   | FileCheck %s --check-prefix=NUM

// The semantic register aliases (zero/tp/lr/sp/pc, declared as AltNames in
// PenumbraRegisterInfo.td) are accepted as register operands and resolve to
// the same registers as the canonical rN spellings.  The printer emits the
// semantic name by default; -penumbra-numeric-reg-names forces the rN form.

// CHECK: mov zero, r1
// NUM: mov r0, r1
mov zero, r1

// CHECK: mov tp, r1
// NUM: mov r12, r1
mov tp, r1

// CHECK: mov lr, sp
// NUM: mov r13, r14
mov lr, sp

// CHECK: add zero, pc
// NUM: add r0, r15
add zero, pc
