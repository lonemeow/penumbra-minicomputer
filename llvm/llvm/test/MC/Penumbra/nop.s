// RUN: llvm-mc -triple=penumbra -show-encoding < %s | FileCheck %s
// RUN: llvm-mc -triple=penumbra -filetype=obj < %s \
// RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=PAD

// NOP is MOV R0, R0: MOV writes no flags and R0 discards the result, so
// a NOP, or alignment padding inside a function, leaves NZCV intact
// between a compare and the branch that tests it.

// CHECK: mov	zero, zero                      // encoding: [0x00,0x00,0x00,0x10]
nop

// PAD:      0: 00 00 00 10   mov zero, zero
// PAD-NEXT: 4: 00 00 24 00   add r1, r2
// PAD-NEXT: 8: 00 00 00 10   mov zero, zero
// PAD-NEXT: c: 00 00 00 10   mov zero, zero
// PAD-NEXT: 10: 00 00 24 00  add r1, r2
add r1, r2
.p2align 4
add r1, r2
