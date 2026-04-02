//===-- PenumbraGISel.td - GlobalISel patterns for Penumbra -*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// TableGen patterns imported by the GlobalISel instruction selector.
// Simple 1:1 mappings live here; complex selection (constants, loads/stores
// with frame-index folding, ICMP-folded branches) stays in the manual
// selector in PenumbraInstructionSelector.cpp.
//
//===----------------------------------------------------------------------===//

include "Penumbra.td"

//===----------------------------------------------------------------------===//
// ALU register-register patterns
//===----------------------------------------------------------------------===//
//
// Penumbra ALU ops are 2-operand destructive (Rd = Rd op Rs).
// The GlobalISel importer reads the tied-operand constraint from FormatR
// and the register allocator inserts COPYs to satisfy it.
// Defs = [SR] (flag clobbering) is propagated automatically.
//
// G_PTR_ADD and G_PTRMASK are matched by the add/and patterns via
// automatic pointer-type handling in the importer.

def : Pat<(i32 (add GPR:$src1, GPR:$src2)), (ADD GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (sub GPR:$src1, GPR:$src2)), (SUB GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (and GPR:$src1, GPR:$src2)), (AND GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (or  GPR:$src1, GPR:$src2)), (OR  GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (xor GPR:$src1, GPR:$src2)), (XOR GPR:$src1, GPR:$src2)>;
