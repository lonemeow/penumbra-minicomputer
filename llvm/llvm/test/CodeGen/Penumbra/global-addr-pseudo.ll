; RUN: llc -mtriple=penumbra -global-isel -stop-after=instruction-select < %s \
; RUN:   | FileCheck %s

; A static/absolute G_GLOBAL_VALUE is selected to a single rematerializable
; PseudoMOVADDR carrying the GlobalAddress, not an eager LLI+LUI pair.  Keeping
; it as one SSA def is what lets MachineCSE share identical bases and the
; register allocator rematerialize the address across calls (see
; global-addr-remat.ll).  PenumbraInstrInfo::expandPostRAPseudo lowers it back
; to LLI :lo16: / LUI :hi16: after register allocation; global-addr.ll checks
; the final asm.

@gvar = global i32 0

define ptr @addr_global() {
; CHECK-LABEL: name: addr_global
; CHECK: PseudoMOVADDR @gvar
; CHECK-NOT: LLI
; CHECK-NOT: LUI
  ret ptr @gvar
}
