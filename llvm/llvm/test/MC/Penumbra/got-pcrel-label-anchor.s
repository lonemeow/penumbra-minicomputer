// RUN: llvm-mc -triple=penumbra-unknown-netbsd -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r %t.o | FileCheck %s

// PC-anchored GOT relocation pairs with label-difference operands
// (doc/system/abi.md, "PC-Anchored Relocation Pairs").  The subtracted
// anchor label folds into the relocation addend as A = P - Q, so the
// linker formula S + A - P yields the GOT offset relative to the
// anchor's address regardless of where the LLI/LUI sit.

  .text
  .globl f
f:
// Anchor directly after the pair: addends fold to the canonical -8/-4.
// CHECK:      R_PENUMBRA_GOT_PCREL_LO16 gsym 0xFFFFFFF8
// CHECK-NEXT: R_PENUMBRA_GOT_PCREL_HI16 gsym 0xFFFFFFFC
  lli  r3, %got_pcrel_lo16(gsym - .LPC0_0)
  lui  r3, %got_pcrel_hi16(gsym - .LPC0_0)
.LPC0_0:
  add  r3, r15
  ldw  r3, [r3 + 0]
  nop

// Anchor in a separate (tail-merged) block: addends measure the real
// distance from each fixup to the anchor across the branch.
// LLI at 0x14, LUI at 0x18, anchor at 0x28: A = 0x14-0x28 / 0x18-0x28.
// CHECK-NEXT: R_PENUMBRA_GOT_PCREL_LO16 gsym2 0xFFFFFFEC
// CHECK-NEXT: R_PENUMBRA_GOT_PCREL_HI16 gsym2 0xFFFFFFF0
  lli  r2, %got_pcrel_lo16(gsym2 - .LPC0_1)
  lui  r2, %got_pcrel_hi16(gsym2 - .LPC0_1)
  b    .Ltail
  nop
  nop
.Ltail:
.LPC0_1:
  add  r2, r15
  ldw  r2, [r2 + 0]

// A same-section local symbol must still produce a GOT relocation —
// the GOT slot is a link-time entity, so the anchor difference must
// never be folded to an assembly-time constant.
// CHECK-NEXT: R_PENUMBRA_GOT_PCREL_LO16 local_fn 0xFFFFFFF8
// CHECK-NEXT: R_PENUMBRA_GOT_PCREL_HI16 local_fn 0xFFFFFFFC
  lli  r4, %got_pcrel_lo16(local_fn - .LPC0_2)
  lui  r4, %got_pcrel_hi16(local_fn - .LPC0_2)
.LPC0_2:
  add  r4, r15
  ldw  r4, [r4 + 0]
  ret

local_fn:
  ret
