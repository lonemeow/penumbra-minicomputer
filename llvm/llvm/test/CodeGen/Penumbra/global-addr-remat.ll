; RUN: llc -mtriple=penumbra -global-isel -verify-machineinstrs < %s \
; RUN:   | FileCheck %s

; PseudoMOVADDR keeps a static global address as a single rematerializable def.
; Here &g is live across a call (loaded before, stored after) while ten args are
; also live across it, exhausting the callee-saved registers.  Under that
; pressure the allocator splits &g's live range and recomputes it after the call
; instead of spilling the address — so %lo16(g)/%hi16(g) is materialized both
; before and after "bl clobber".  The old eager LLI+LUI pair could not be
; rematerialized at all (remat needs a single def, not a two-instruction chain),
; so it had to spill the address or burn a callee-saved register.
;
; Pressure-sensitive by design: this guards the rematerialization *capability*.
; How aggressively it is exploited is a per-subtarget policy decision (gen1's
; 1 KB direct-mapped I$ vs gen2's 4 KB 4-way) — see doc/TODO.md.

@g = global i32 0
declare void @clobber()

define i32 @remat(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e,
                  i32 %f, i32 %h, i32 %i, i32 %j, i32 %k) {
; CHECK-LABEL: remat:
; CHECK:       lli r{{[0-9]+}}, %lo16(g)
; CHECK:       lui r{{[0-9]+}}, %hi16(g)
; CHECK:       bl clobber
; CHECK:       lli r{{[0-9]+}}, %lo16(g)
; CHECK:       lui r{{[0-9]+}}, %hi16(g)
  %v = load i32, ptr @g
  call void @clobber()
  %s1 = add i32 %a, %b
  %s2 = add i32 %s1, %c
  %s3 = add i32 %s2, %d
  %s4 = add i32 %s3, %e
  %s5 = add i32 %s4, %f
  %s6 = add i32 %s5, %h
  %s7 = add i32 %s6, %i
  %s8 = add i32 %s7, %j
  %s9 = add i32 %s8, %k
  %t  = add i32 %s9, %v
  store i32 %t, ptr @g
  ret i32 %t
}
