// MC code emitter must hard-abort on immediates that don't fit in their
// encoding field, instead of silently truncating and producing wrong code
// (that's what masked `MAXBSIZE` stack frames for months).

// RUN: not llvm-mc -triple=penumbra -filetype=obj %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s

// imm16 field: accepts [-32768, 65535] (union of LLIS signed and LLI/ADDi
// unsigned).  65536 is one past the unsigned max.
lli r1, 65536
// CHECK: error: immediate 65536 out of range [-32768, 65535] for imm16

// memory offset field: signed 16-bit, so 32768 overflows.
ldw r2, [r14 + 32768]
// CHECK: error: immediate 32768 out of range [-32768, 32767] for memory offset

// And at the other end: -32769 overflows the signed negative side.
stw r3, [r14 + -32769]
// CHECK: error: immediate -32769 out of range [-32768, 32767] for memory offset
