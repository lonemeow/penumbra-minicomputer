; RUN: llvm-mc -triple=penumbra -show-encoding %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ENC
; RUN: llvm-mc -triple=penumbra -filetype=obj %s -o %t.o
; RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC

; Test PC-relative relocations: %pcrel() specifier on memory offset and immediate.

  .text
  .globl test_pcrel_load
test_pcrel_load:
; ENC: ldw r1, [r15 + %pcrel(myvar)]
; ENC: fixup {{.*}} kind: fixup_penumbra_memoffset16_pcrel
  ldw r1, [pc + %pcrel(myvar)]

  .globl test_pcrel_imm
test_pcrel_imm:
; ENC: add r2, %pcrel(myvar)
; ENC: fixup {{.*}} kind: fixup_penumbra_imm16_pcrel
  add r2, %pcrel(myvar)

; Verify two relocations are emitted at the expected offsets.
; RELOC: Relocations [
; RELOC:   Section {{.*}} .rela.text {
; RELOC:     0x0
; RELOC:     0x4
; RELOC:   }
; RELOC: ]
