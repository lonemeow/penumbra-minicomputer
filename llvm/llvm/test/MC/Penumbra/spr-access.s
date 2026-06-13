// RUN: llvm-mc -triple=penumbra -show-encoding %s | FileCheck %s
//
// RDSPR reads any SPR including SR (read-only); WRSPR writes the value SPRs
// (ESR/EPC/USP/SCR0-3).  WRSPR SR is rejected — see wrspr-sr-reserved.s.  SPR
// names map to their index; the instruction printer shows the number.

// CHECK: rdspr r1, 3 // encoding: [0x00,0x30,0x20,0x3e]
rdspr r1, sr

// CHECK: rdspr r1, 0 // encoding: [0x00,0x00,0x20,0x3e]
rdspr r1, esr

// CHECK: wrspr 0, r1 // encoding: [0x00,0x00,0x20,0x3c]
wrspr esr, r1

// CHECK: wrspr 2, r1 // encoding: [0x00,0x20,0x20,0x3c]
wrspr usp, r1

// CHECK: wrspr 4, r1 // encoding: [0x00,0x40,0x20,0x3c]
wrspr scr0, r1
