// RUN: llvm-mc -triple=penumbra -filetype=obj %s -o %t.o

// Test 1: linked binary — branch targets and lli/lui address annotations.
// RUN: ld.lld %t.o -o %t
// RUN: llvm-objdump -d %t | FileCheck %s --check-prefix=LINKED

// Test 2: object file — relocation display with -r.
// RUN: llvm-objdump -d -r %t.o | FileCheck %s --check-prefix=RELOC

  .text
  .globl _start

// --- Branch and address-load annotations (linked binary) -----------------

_start:
  // LLI+LUI pair loading address of mydata.
  lli r1, %lo16(mydata)
  lui r1, %hi16(mydata)
  ldw r2, [r1 + 0]

  // Direct call and branch to known symbols.
  bl  helper
  b   _start

// LINKED-LABEL: <_start>:
// LINKED:       lli r1,
// LINKED-NEXT:  lui r1, {{.*}} <mydata>
// LINKED:       bl {{.*}} <helper>
// LINKED:       b {{.*}} <_start>

  .globl helper
helper:
  // Conditional branch back to _start.
  beq _start
  // Indirect return — no annotation.
  jmp r13

// LINKED-LABEL: <helper>:
// LINKED:       beq {{.*}} <_start>
// LINKED:       jmp r13
// LINKED-NOT:   <

// --- Register invalidation (no false annotations) ------------------------

  .globl no_annotate
no_annotate:
  // LLI followed by a clobbering SUB before LUI — tracking invalidated.
  // (ADDi is tracked, so use SUBi which isn't.)
  lli r1, %lo16(mydata)
  sub r1, 1
  lui r1, %hi16(mydata)
  jmp r13

// LINKED-LABEL: <no_annotate>:
// LINKED:       lli r1,
// LINKED-NEXT:  sub r1, 1
// LINKED-NEXT:  lui r1,
// LINKED-NOT:   mydata
// LINKED:       jmp r13

// --- Relocation display in object file -----------------------------------

// RELOC-LABEL: <_start>:
// RELOC:       lli r1, 0
// RELOC-NEXT:  R_PENUMBRA_LO16 mydata
// RELOC:       lui r1, 0
// RELOC-NEXT:  R_PENUMBRA_HI16 mydata
// RELOC:       bl
// RELOC-NEXT:  R_PENUMBRA_BRANCH22 helper
// RELOC:       b
// RELOC-NEXT:  R_PENUMBRA_BRANCH22 _start

// RELOC-LABEL: <helper>:
// RELOC:       beq
// RELOC-NEXT:  R_PENUMBRA_BRANCH22 _start

  .data
  .globl mydata
mydata:
  .long 42
