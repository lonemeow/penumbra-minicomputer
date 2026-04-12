// RUN: llvm-mc -triple=penumbra -show-encoding < %s \
// RUN:   | FileCheck %s

// Test that R0 (zero) and R15 (PC) are valid source operands in ALU
// instructions.  GPRz on $Rs allows any GPR including reserved ones.

// --- R0 (hardwired zero) as source ---

// CHECK: add	r1, r0                  // encoding: [0x00,0x00,0x20,0x00]
add r1, r0

// CHECK: sub	r2, r0                  // encoding: [0x00,0x00,0x40,0x02]
sub r2, r0

// CHECK: and	r3, r0                  // encoding: [0x00,0x00,0x60,0x04]
and r3, r0

// CHECK: mov	r1, r0                  // encoding: [0x00,0x00,0x20,0x10]
mov r1, r0

// CHECK: not	r1, r0                  // encoding: [0x00,0x00,0x20,0x12]
not r1, r0

// CHECK: cmp	r1, r0                  // encoding: [0x00,0x00,0x21,0x02]
cmp r1, r0

// CHECK: test	r1, r0                  // encoding: [0x00,0x00,0x21,0x04]
test r1, r0

// --- R15 (PC) as source ---

// CHECK: add	r1, r15                 // encoding: [0x00,0x00,0x3e,0x00]
add r1, pc

// CHECK: mov	r1, r15                 // encoding: [0x00,0x00,0x3e,0x10]
mov r1, pc

// CHECK: cmp	r1, r15                 // encoding: [0x00,0x00,0x3f,0x02]
cmp r1, pc

// --- R0 as store data (store zero) ---

// CHECK: stw	r0, [r1 + 0]            // encoding: [0x00,0x00,0x04,0x90]
stw r0, [r1 + 0]

// CHECK: stb	r0, [r14 + 4]           // encoding: [0x10,0x00,0x38,0x80]
stb r0, [sp + 4]

// --- R0 as base address (zero-base addressing for low memory) ---

// CHECK: ldw	r1, [r0 + 100]          // encoding: [0x90,0x01,0x40,0xb0]
ldw r1, [r0 + 100]

// CHECK: ldb	r3, [r0 + 255]          // encoding: [0xfc,0x03,0xc0,0xa0]
ldb r3, [r0 + 255]

// CHECK: stw	r0, [r0 + 0]            // encoding: [0x00,0x00,0x00,0x90]
stw r0, [r0 + 0]

// CHECK: sth	r2, [r0 + 8]            // encoding: [0x20,0x00,0x80,0x88]
sth r2, [r0 + 8]
