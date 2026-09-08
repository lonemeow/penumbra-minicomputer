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
// Subtarget features
//===----------------------------------------------------------------------===//
//
// Every generation implements the same ISA and accepts the same binaries, so
// a Penumbra subtarget feature is always a *tuning* property.  It belongs in
// the fourth (tune) list of a ProcessorModel below, never in the third
// (architecture) list: the architecture list decides what a binary may
// contain, the tune list only decides which of several correct lowerings the
// backend prefers — branch versus branchless, inline versus libcall,
// schedule, unroll factor.  Nothing in PenumbraLegalizerInfo's legality
// rules may branch on a feature.  That invariant is what lets an object
// built while tuning for one generation execute on any of them.
//
// Name a feature for the micro-architectural property it describes rather
// than for the processor that happens to have it, so a later generation
// sharing the property reuses the same codegen path instead of growing
// another `CPU ==` test.

//===----------------------------------------------------------------------===//
// Penumbra processors — tuning targets for one shared ISA
//===----------------------------------------------------------------------===//

// Scheduling model.  MCSubtargetInfo reads the model from the *tune* name
// rather than the architecture name, so a processor's model is part of its
// tuning answer.  These figures are calibrated against the microcoded
// single-issue core and are shared until a generation has measured numbers of
// its own: the scheduler acts on a speculative model just as confidently as
// on a measured one.
//
// LoadLatency = 1 (not the real ~4 cache-hit cycles) because our execution
// is microcoded and single-issue — each multi-cycle op stalls the pipeline,
// so the scheduler cannot actually fill load-delay slots with unrelated
// work.  Telling the scheduler "load result is available next cycle" stops
// it from reordering ALU ops ahead of loads/stores, which on its own does
// nothing useful but does extend live ranges and inflate register pressure.
// HighLatency = 8 still flags multi-cycle ops (MUL/DIV libcalls) for the
// "expensive" heuristics used by the allocator.
def PenumbraGenericModel : SchedMachineModel {
  let IssueWidth = 1;            // Single-issue, microcoded
  let MicroOpBufferSize = 0;     // In-order: no reorder buffer
  let LoadLatency = 1;           // Microcoded: no benefit from hiding load latency
  let HighLatency = 8;           // Threshold for "expensive" ops
  let MispredictPenalty = 0;     // No branch predictor
  let CompleteModel = 0;         // Don't require itineraries for every opcode
}

// The architecture list is empty for every processor — one ISA, one set of
// legal instructions — so these definitions differ only in their tune list
// and scheduling model.  Each name is accepted by both -mcpu and -mtune.

// Tuned to run acceptably on any generation, and chosen when nothing names a
// specific one.  Code that ships to more than one core wants this, which is
// why it is the default rather than the newest part.
def : ProcessorModel<"generic", PenumbraGenericModel, []>;

// The original minicomputer: single-issue, microcoded, no branch prediction.
def : ProcessorModel<"penumbra1", PenumbraGenericModel, []>;

// The pipelined core.
def : ProcessorModel<"penumbra2", PenumbraGenericModel, []>;

//===----------------------------------------------------------------------===//
// Assembly printer
//===----------------------------------------------------------------------===//

def PenumbraInstPrinter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
  bit isMCAsmWriter = 1;
}

//===----------------------------------------------------------------------===//
// Assembly parser
//===----------------------------------------------------------------------===//

// Emit MatchRegisterAltName so the ABI register aliases (the AltNames in
// PenumbraRegisterInfo.td) resolve through one generated table — shared by
// the assembler and by named-global-register lowering.  Each alias maps to a
// single register, so duplicate handling is not needed.
def PenumbraAsmParser : AsmParser {
  let ShouldEmitMatchRegisterAltName = 1;
}

//===----------------------------------------------------------------------===//
// Target declaration
//===----------------------------------------------------------------------===//

def Penumbra : Target {
  let InstructionSet = PenumbraInstrInfo;
  let AssemblyWriters = [PenumbraInstPrinter];
  let AssemblyParsers = [PenumbraAsmParser];
}

// Map LLVM's built-in ptr_rc pseudo-operands to our GPR class.
defm : RemapAllTargetPseudoPointerOperands<GPR>;
