; RUN: llc -mtriple=penumbra-unknown-none -O0 -relocation-model=pic < %s \
; RUN:   | FileCheck %s

; PIC TLS access: GOT-indirect PC-relative offset to tls_index pair,
; then call __tls_get_addr.

@tls_var = thread_local global i32 0

define ptr @get_tls_addr() {
; CHECK-LABEL: get_tls_addr:
; CHECK:       mov r1, r15
; CHECK-NEXT:  lli r2, %tlsgd_got_pcrel_lo16(tls_var+4)
; CHECK-NEXT:  lui r2, %tlsgd_got_pcrel_hi16(tls_var+8)
; CHECK-NEXT:  add r1, r2
; CHECK-NEXT:  bl __tls_get_addr
  %p = call ptr @llvm.threadlocal.address(ptr @tls_var)
  ret ptr %p
}

; TLS variable goes in .tbss:
; CHECK: .section .tbss
; CHECK: tls_var:
