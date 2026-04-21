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
include "PenumbraCombine.td"

// Pointer value type for address space 0 (the only address space).
def PenumbraPtrVT : PtrValueType<i32, 0>;

//===----------------------------------------------------------------------===//
// ALU register-register patterns
//===----------------------------------------------------------------------===//
//
// Penumbra ALU ops are 2-operand destructive (Rd = Rd op Rs).
// The GlobalISel importer reads the tied-operand constraint from FormatR
// and the register allocator inserts COPYs to satisfy it.
// Defs = [SR] (flag clobbering) is propagated automatically.

def : Pat<(i32 (add GPR:$src1, GPR:$src2)), (ADD GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (sub GPR:$src1, GPR:$src2)), (SUB GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (and GPR:$src1, GPR:$src2)), (AND GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (or  GPR:$src1, GPR:$src2)), (OR  GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (xor GPR:$src1, GPR:$src2)), (XOR GPR:$src1, GPR:$src2)>;

//===----------------------------------------------------------------------===//
// ALU register-immediate patterns
//===----------------------------------------------------------------------===//
//
// Immediate ALU ops fold a 16-bit constant into the instruction.
// Higher-priority than reg-reg: the importer tries more specific patterns first.

def : Pat<(i32 (add GPR:$src, uimm16:$imm)), (ADDi GPR:$src, uimm16:$imm)>;
def : Pat<(i32 (sub GPR:$src, uimm16:$imm)), (SUBi GPR:$src, uimm16:$imm)>;
def : Pat<(i32 (and GPR:$src, uimm16:$imm)), (ANDi GPR:$src, uimm16:$imm)>;

//===----------------------------------------------------------------------===//
// Shift patterns (register and immediate)
//===----------------------------------------------------------------------===//
//
// Immediate shifts use a 5-bit shift amount (0–31). The imm form is preferred
// when the shift amount is constant.

def : Pat<(i32 (shl GPR:$src1, GPR:$src2)), (SHL GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (srl GPR:$src1, GPR:$src2)), (SHR GPR:$src1, GPR:$src2)>;
def : Pat<(i32 (sra GPR:$src1, GPR:$src2)), (SAR GPR:$src1, GPR:$src2)>;

def : Pat<(i32 (shl GPR:$src, uimm5:$amt)), (SHLi GPR:$src, uimm5:$amt)>;
def : Pat<(i32 (srl GPR:$src, uimm5:$amt)), (SHRi GPR:$src, uimm5:$amt)>;
def : Pat<(i32 (sra GPR:$src, uimm5:$amt)), (SARi GPR:$src, uimm5:$amt)>;

//===----------------------------------------------------------------------===//
// Move and bitwise NOT
//===----------------------------------------------------------------------===//

def : Pat<(i32 (not GPR:$src)), (NOT GPR:$src)>;

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//
//
// LLI:  unsigned 16-bit (0–65535)
// LLIS: negative signed 16-bit (-32768–-1)
// Values outside both ranges need LLI+LUI (handled manually in selectConstant).

def : Pat<(i32 uimm16:$val), (LLI uimm16:$val)>;
def : Pat<(i32 simm16neg:$val), (LLIS simm16neg:$val)>;

//===----------------------------------------------------------------------===//
// Loads
//===----------------------------------------------------------------------===//
//
// Frame-index-folded and pointer-typed loads are handled manually in
// selectLoad (routed there before selectImpl).  These patterns handle
// the common register-base case.

def : Pat<(i32 (load GPR:$base)),             (LDW GPR:$base, 0)>;
def : Pat<(PenumbraPtrVT (load GPR:$base)),   (LDW GPR:$base, 0)>;
def : Pat<(i32 (zextloadi16 GPR:$base)), (LDH GPR:$base, 0)>;
def : Pat<(i32 (sextloadi16 GPR:$base)), (LDHS GPR:$base, 0)>;
def : Pat<(i32 (extloadi16 GPR:$base)),  (LDH GPR:$base, 0)>;
def : Pat<(i32 (zextloadi8 GPR:$base)),  (LDB GPR:$base, 0)>;
def : Pat<(i32 (sextloadi8 GPR:$base)),  (LDBS GPR:$base, 0)>;
def : Pat<(i32 (extloadi8 GPR:$base)),   (LDB GPR:$base, 0)>;

//===----------------------------------------------------------------------===//
// Stores
//===----------------------------------------------------------------------===//
//
// Frame-index-folded and store-zero cases are handled manually in
// selectStore (routed there before selectImpl).

def : Pat<(store (i32 GPRz:$val), GPR:$base),               (STW GPRz:$val, GPR:$base, 0)>;
def : Pat<(store (PenumbraPtrVT GPRz:$val), GPR:$base),    (STW GPRz:$val, GPR:$base, 0)>;
def : Pat<(truncstorei16 (i32 GPRz:$val), GPR:$base),  (STH GPRz:$val, GPR:$base, 0)>;
def : Pat<(truncstorei8 (i32 GPRz:$val), GPR:$base),   (STB GPRz:$val, GPR:$base, 0)>;
