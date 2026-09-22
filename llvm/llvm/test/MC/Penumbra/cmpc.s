// RUN: llvm-mc -triple=penumbra -show-encoding < %s | FileCheck %s
// RUN: llvm-mc -triple=penumbra -filetype=obj < %s \
// RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=DISASM

// CMPC is SBC with the flag-only bit (IR[16]) set: the two differ in that bit
// alone, and each decodes back to its own mnemonic.

// CHECK: cmpc	r2, r3                  // encoding: [0x00,0x00,0x47,0x16]
cmpc r2, r3
// CHECK: sbc	r2, r3                  // encoding: [0x00,0x00,0x46,0x16]
sbc r2, r3

// DISASM: cmpc	r2, r3
// DISASM-NEXT: sbc	r2, r3
