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
// Calling convention (see doc/abi/penumbra-abi.md):
//   R1-R4   = arguments / return values (caller-saved)
//   R5-R10  = callee-saved; R10 = frame pointer if needed
//   R11     = scratch (caller-saved)
//   R12     = thread pointer (reserved)
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Register definitions
//===----------------------------------------------------------------------===//

class PenumbraReg<bits<4> num, string n, list<string> altNames = []>
    : Register<n, altNames> {
  field bits<4> Num;
  let Num = num;
  let HWEncoding{3-0} = num;
  let Namespace = "Penumbra";
}

class PenumbraGPR<bits<4> num, string n, list<string> altNames = []>
    : PenumbraReg<num, n, altNames>;

def R0  : PenumbraGPR<0,  "r0", ["zero"]>;

foreach i = 1-12 in {
  def R#i : PenumbraGPR<i, "r"#i>;
}

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

// Allocatable subset — used by the register allocator during codegen.
// Excludes R0 (zero), R12 (thread pointer), R14 (SP), R15 (PC).
// Caller-saved listed first (preferred), then callee-saved.
def GPR_Allocatable : RegisterClass<"Penumbra", [i32], 32, (add
  // Caller-saved (args + scratch + LR)
  (sequence "R%u", 1, 4), R11, R13,
  // Callee-saved
  (sequence "R%u", 5, 10))> {
  let isAllocatable = 1;
}

def CCR : RegisterClass<"Penumbra", [i32], 32, (add SR)> {
  let CopyCost = -1;
  let isAllocatable = 0;
}
