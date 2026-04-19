// PC-relative %pcrel() fixups on ADDi accept any |offset| ≤ 65535 via the
// assembler's ADDi↔SUBi flip.  This matches what sqlite3's VDBE dispatch
// produces: a legitimate positive offset up to ~62 KB that used to be
// silently truncated by the encoder before the range check was added.
//
// Positive large offset (62780 bytes) — within range, must succeed and
// produce ADDi with the correct uimm16.
// Negative large offset (-62780 bytes) — within range, must succeed and
// produce SUBi with the negated value.

// Link the object so fixups are fully resolved before checking encoding.
// Same-file local labels aren't enough — MC emits a relocation for them
// too; we need the linker to apply the final value.
// RUN: llvm-mc -triple=penumbra -filetype=obj %s -o %t.o
// RUN: ld.lld --defsym=_start=pos_in_range %t.o -o %t
// RUN: llvm-objdump -d %t | FileCheck %s

  .text
  .globl pos_in_range
pos_in_range:
// CHECK-LABEL: <pos_in_range>:
// CHECK-NEXT: add r1, 62780
  add r1, %pcrel(far_pos)

  .space 62776
  .globl far_pos
far_pos:
  add r2, r2

// The negative case sits just after far_pos, so %pcrel back to
// pos_in_range is -62784 bytes.  The applyFixup logic flips ADDi to SUBi
// and encodes the negated value, so we should see `sub r3, 62784`.
  .globl neg_in_range
neg_in_range:
// CHECK-LABEL: <neg_in_range>:
// CHECK-NEXT: sub r3, 62784
  add r3, %pcrel(pos_in_range)
