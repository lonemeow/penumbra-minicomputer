// RUN: llvm-mc -filetype=obj -triple=penumbra %s -o %t.o
// RUN: ld.lld -shared -o %t.so %t.o
// RUN: llvm-readobj -r %t.so | FileCheck %s

// Test TLS General-Dynamic in shared library: GOT tls_index entry
// with DTPMOD32 + DTPOFF32 dynamic relocations.

.text
.globl get_tls
.type get_tls,@function
get_tls:
  mov   r1, r15
  add   r1, %tlsgd_pcrel(tvar+4)
  bl    __tls_get_addr
  jmp   r13
.size get_tls, . - get_tls

.section .tbss,"awT",@nobits
.globl tvar
tvar:
  .zero 4

// CHECK: R_PENUMBRA_TLS_DTPMOD32 tvar
// CHECK: R_PENUMBRA_TLS_DTPOFF32 tvar
// CHECK: R_PENUMBRA_JUMP_SLOT __tls_get_addr
