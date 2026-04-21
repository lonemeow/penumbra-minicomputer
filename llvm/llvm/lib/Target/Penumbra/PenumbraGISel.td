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
// ComplexPattern → GISel C++ method equivalences
//===----------------------------------------------------------------------===//
//
// Tell the GISel importer which C++ method implements each SDAG
// ComplexPattern.  The method is defined on PenumbraInstructionSelector and
// returns a ComplexRendererFns describing how to populate the matched
// sub-operands.

def GIAddrRegImm :
  GIComplexOperandMatcher<s32, "selectAddrRegImm">,
  GIComplexPatternEquiv<AddrRegImm>;

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

// Pointer arithmetic: G_PTR_ADD on p0.  Uses the dedicated `ptradd` SDAG
// node (which the GISel importer maps to G_PTR_ADD), not plain `add` which
// is reserved for integer-only G_ADD.  Reg-imm variant folds a constant
// offset into ADDi — critical for GEPs with constant indices (struct
// field access, p[i] with constant i, p++ in tight loops).
def : Pat<(ptradd GPR:$src, GPR:$off),    (ADD  GPR:$src, GPR:$off)>;
def : Pat<(ptradd GPR:$src, uimm16:$imm), (ADDi GPR:$src, uimm16:$imm)>;

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
// AddrRegImm decomposes the address into (base, simm16 offset) so constant
// GEP offsets and frame indices fold into the Rb/offset slots directly.
// The fallback case (bare register with no useful offset) still matches with
// offset=0.

def : Pat<(i32 (load (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDW GPR:$base, memoffset16:$off)>;
def : Pat<(PenumbraPtrVT (load (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDW GPR:$base, memoffset16:$off)>;
def : Pat<(i32 (zextloadi16 (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDH GPR:$base, memoffset16:$off)>;
def : Pat<(i32 (sextloadi16 (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDHS GPR:$base, memoffset16:$off)>;
def : Pat<(i32 (extloadi16 (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDH GPR:$base, memoffset16:$off)>;
def : Pat<(i32 (zextloadi8 (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDB GPR:$base, memoffset16:$off)>;
def : Pat<(i32 (sextloadi8 (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDBS GPR:$base, memoffset16:$off)>;
def : Pat<(i32 (extloadi8 (AddrRegImm GPR:$base, memoffset16:$off))),
          (LDB GPR:$base, memoffset16:$off)>;

//===----------------------------------------------------------------------===//
// Stores
//===----------------------------------------------------------------------===//
//
// Same AddrRegImm address folding as loads.  Store-zero falls out of R0
// being a valid GPR: the selector maps G_CONSTANT 0 to `COPY $r0` and
// MachineCopyPropagation forwards $r0 into the STW/STH/STB value slot.

def : Pat<(store (i32 GPR:$val), (AddrRegImm GPR:$base, memoffset16:$off)),
          (STW GPR:$val, GPR:$base, memoffset16:$off)>;
def : Pat<(store (PenumbraPtrVT GPR:$val), (AddrRegImm GPR:$base, memoffset16:$off)),
          (STW GPR:$val, GPR:$base, memoffset16:$off)>;
def : Pat<(truncstorei16 (i32 GPR:$val), (AddrRegImm GPR:$base, memoffset16:$off)),
          (STH GPR:$val, GPR:$base, memoffset16:$off)>;
def : Pat<(truncstorei8 (i32 GPR:$val), (AddrRegImm GPR:$base, memoffset16:$off)),
          (STB GPR:$val, GPR:$base, memoffset16:$off)>;
