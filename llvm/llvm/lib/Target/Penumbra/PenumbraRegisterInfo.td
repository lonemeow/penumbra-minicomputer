//===-- PenumbraRegisterInfo.td - Penumbra Register defs ---*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Penumbra has 16 general-purpose 32-bit registers (R0-R15):
//   R0  = hardwired zero
//   R12 = thread pointer (reserved, not allocatable)
//   R13 = link register (set by BL)
//   R14 = stack pointer (hardware-banked USP/SSP)
//   R15 = program counter (read-only from software)
//
// Calling convention (see doc/system/abi.md):
//   R1-R4   = arguments / return values (caller-saved)
//   R5-R10  = callee-saved; R10 = frame pointer if needed
//   R11     = scratch (caller-saved)
//   R12     = thread pointer (reserved)
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Alternate-name index
//===----------------------------------------------------------------------===//

// Semantic names for the special-purpose registers (zero, tp, lr, sp, pc).
// Registers with no semantic name fall back to their physical r<N> name via
// NoRegAltName, so only the five special registers need an alias entry.
def SemanticRegName : RegAltNameIndex {
  let FallbackRegAltNameIndex = NoRegAltName;
}

//===----------------------------------------------------------------------===//
// Register definitions
//===----------------------------------------------------------------------===//

class PenumbraReg<bits<4> num, string n, list<string> altNames = []>
    : Register<n, altNames> {
  field bits<4> Num;
  let Num = num;
  let HWEncoding{3-0} = num;
  let Namespace = "Penumbra";
  // DWARF register numbers match hardware encoding (0–15).
  let DwarfNumbers = [num];
  // A register given a semantic alias advertises it under SemanticRegName; the
  // rest carry no index and fall back to r<N>.  Deriving this from altNames
  // keeps the two lists in parity (one alias <-> one index).
  let RegAltNameIndices = !if(!empty(altNames), []<RegAltNameIndex>,
                              [SemanticRegName]);
}

class PenumbraGPR<bits<4> num, string n, list<string> altNames = []>
    : PenumbraReg<num, n, altNames>;

// R0 is hardwired to zero — mark isConstant so passes that distinguish
// "always reads as 0" from merely "reserved" (e.g. MCP's check at
// MachineCopyPropagation.cpp:830) treat $r0 as safe to forward.
// Caveat: MCP still won't actually forward through `$rN = COPY $r0` in
// practice because virtregrewriter doesn't mark the destination as
// renamable when the source is a reserved physreg, and MCP gates
// forwarding on the renamable flag.
let isConstant = true in
def R0  : PenumbraGPR<0,  "r0", ["zero"]>;

foreach i = 1-11 in {
  def R#i : PenumbraGPR<i, "r"#i>;
}

def R12 : PenumbraGPR<12, "r12", ["tp"]>;

def R13 : PenumbraGPR<13, "r13", ["lr"]>;
def R14 : PenumbraGPR<14, "r14", ["sp"]>;
def R15 : PenumbraGPR<15, "r15", ["pc"]>;

//===----------------------------------------------------------------------===//
// Status register (condition flags)
//===----------------------------------------------------------------------===//

class PenumbraSR<string n> : PenumbraReg<0, n>;

def SR : PenumbraSR<"sr">;

//===----------------------------------------------------------------------===//
// Register classes
//===----------------------------------------------------------------------===//

// All GPRs — used by instruction definitions so the assembler accepts any
// register, including reserved ones (R0, R12, R14, R15).
def GPR : RegisterClass<"Penumbra", [i32], 32, (add
  (sequence "R%u", 0, 15))>;

// Value/allocation class for i32.  The register allocator never assigns
// R0/R12/R14/R15 (held out by getReservedRegs), but R12 and R14 are listed as
// *members* so the register coalescer can substitute them for a virtual
// register — e.g. fold `%v = COPY $r12` when curlwp is pinned in R12, or the
// `%v = COPY $r14` that addresses outgoing stack arguments — the way RISC-V's
// GPR class contains its reserved thread and stack pointers.  They are listed
// last so the allocation order of the allocatable registers is unchanged.
// (R0/R15 remain class non-members; add them here too if their reads ever
// need the same coalescing.)
def GPR_Allocatable : RegisterClass<"Penumbra", [i32], 32, (add
  // Caller-saved (args + scratch + LR)
  (sequence "R%u", 1, 4), R11, R13,
  // Callee-saved
  (sequence "R%u", 5, 10),
  // Reserved members (excluded from allocation by getReservedRegs)
  R12, R14)> {
  let isAllocatable = 1;
}

def CCR : RegisterClass<"Penumbra", [i32], 32, (add SR)> {
  let CopyCost = -1;
  let isAllocatable = 0;
}
