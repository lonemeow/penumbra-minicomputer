// RUN: llvm-mc -triple=penumbra -show-encoding < %s | FileCheck %s
// RUN: llvm-mc -triple=penumbra -filetype=obj < %s \
// RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=DISASM

// Multiply / divide (Format R, op 16-19).  The third register Rdh is always
// write-only (product high half, or remainder).  Two assembly forms share each
// opcode: a 2-operand form with Rdh wired to R0 (high half / remainder
// discarded), and a 3-operand form naming Rdh explicitly.  See
// doc/internals/divmul.md and doc/system/instruction-encoding.md.

// --- 2-operand forms: Rdh = R0, low half / quotient only ---

// CHECK: mul	r1, r2                  // encoding: [0x00,0x00,0x24,0x20]
mul r1, r2
// CHECK: mulu	r3, r4                  // encoding: [0x00,0x00,0x68,0x22]
mulu r3, r4
// CHECK: div	r5, r6                  // encoding: [0x00,0x00,0xac,0x24]
div r5, r6
// CHECK: divu	r7, r8                  // encoding: [0x00,0x00,0xf0,0x26]
divu r7, r8

// --- 3-operand forms: Rdh names the high-half / remainder destination ---

// CHECK: mul	r1, r2, r3              // encoding: [0x00,0x30,0x24,0x20]
mul r1, r2, r3
// CHECK: mulu	r4, r5, r6              // encoding: [0x00,0x60,0x8a,0x22]
mulu r4, r5, r6
// CHECK: div	r7, r8, r9              // encoding: [0x00,0x90,0xf0,0x24]
div r7, r8, r9
// CHECK: divu	r10, r11, r1            // encoding: [0x00,0x10,0x56,0x27]
divu r10, r11, r1

// --- Disassembly round-trip: Rdh==R0 decodes back to the 2-operand form,
//     a named Rdh decodes to the 3-operand form. ---

// DISASM: mul	r1, r2
// DISASM: mulu	r3, r4
// DISASM: div	r5, r6
// DISASM: divu	r7, r8
// DISASM: mul	r1, r2, r3
// DISASM: mulu	r4, r5, r6
// DISASM: div	r7, r8, r9
// DISASM: divu	r10, r11, r1
