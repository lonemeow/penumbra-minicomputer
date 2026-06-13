// RUN: not llvm-mc -triple=penumbra %s 2>&1 | FileCheck %s
//
// WRSPR SR (SPR number 3) is a reserved encoding that traps to VEC_ILLEGAL:
// SR is read-only via RDSPR, and software changes it via EI/DI/ERET/exception
// entry.  The assembler must reject it so it never emits the illegal opcode —
// both the named form and the raw number.  RDSPR SR stays valid (spr-access.s).

wrspr sr, r1
// CHECK: [[@LINE-1]]:1: error: WRSPR SR is reserved

wrspr 3, r1
// CHECK: [[@LINE-1]]:1: error: WRSPR SR is reserved
