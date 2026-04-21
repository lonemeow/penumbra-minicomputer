//===-- Penumbra.td - Describe the Penumbra Target ---------*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Top-level TableGen file for the Penumbra target. The Penumbra ISA is a
// 32-bit little-endian RISC architecture with 16 registers, 4 instruction
// formats (R/L/M/B), and ARM-style NZCV condition flags.
//
// This file defines the ISA — not any specific implementation. CPU models
// (scheduling, latencies) are defined separately per implementation.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Register and instruction definitions
//===----------------------------------------------------------------------===//

include "PenumbraRegisterInfo.td"
include "PenumbraInstrInfo.td"
include "PenumbraCallingConv.td"
include "GISel/PenumbraRegisterBanks.td"

//===----------------------------------------------------------------------===//
// Penumbra processors — specific implementations of the ISA
//===----------------------------------------------------------------------===//

// The original minicomputer: single-issue, microcoded, no branch prediction.
//
// LoadLatency = 1 (not the real ~4 cache-hit cycles) because our execution
// is microcoded and single-issue — each multi-cycle op stalls the pipeline,
// so the scheduler cannot actually fill load-delay slots with unrelated
// work.  Telling the scheduler "load result is available next cycle" stops
// it from reordering ALU ops ahead of loads/stores, which on its own does
// nothing useful but does extend live ranges and inflate register pressure.
// HighLatency = 8 still flags multi-cycle ops (MUL/DIV libcalls) for the
// "expensive" heuristics used by the allocator.
def Penumbra1Model : SchedMachineModel {
  let IssueWidth = 1;            // Single-issue, microcoded
  let MicroOpBufferSize = 0;     // In-order: no reorder buffer
  let LoadLatency = 1;           // Microcoded: no benefit from hiding load latency
  let HighLatency = 8;           // Threshold for "expensive" ops
  let MispredictPenalty = 0;     // No branch predictor
  let CompleteModel = 0;         // Don't require itineraries for every opcode
}
def : ProcessorModel<"penumbra1", Penumbra1Model, []>;

//===----------------------------------------------------------------------===//
// Assembly printer
//===----------------------------------------------------------------------===//

def PenumbraInstPrinter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
  bit isMCAsmWriter = 1;
}

//===----------------------------------------------------------------------===//
// Target declaration
//===----------------------------------------------------------------------===//

def Penumbra : Target {
  let InstructionSet = PenumbraInstrInfo;
  let AssemblyWriters = [PenumbraInstPrinter];
}

// Map LLVM's built-in ptr_rc pseudo-operands to our GPR class.
defm : RemapAllTargetPseudoPointerOperands<GPR>;
