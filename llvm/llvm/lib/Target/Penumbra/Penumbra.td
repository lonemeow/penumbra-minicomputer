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

// Scheduling models.  MCSubtargetInfo reads the model from the *tune* name
// rather than the architecture name, so a processor's model is part of its
// tuning answer.  A model states what the compiler can observe of a core —
// how many cycles an instruction holds it — and nothing about how the
// hardware arrives there: two implementations with the same counts are the
// same core to the compiler.
//
// The parameters every model shares are calibrated against the microcoded
// single-issue core and stay shared until a generation has measured numbers
// of its own: the scheduler acts on a speculative model just as confidently
// as on a measured one.
//
// LoadLatency = 1 (not the real ~4 cache-hit cycles) because our execution
// is microcoded and single-issue — each multi-cycle op stalls the pipeline,
// so the scheduler cannot actually fill load-delay slots with unrelated
// work.  Telling the scheduler "load result is available next cycle" stops
// it from reordering ALU ops ahead of loads/stores, which on its own does
// nothing useful but does extend live ranges and inflate register pressure.
// HighLatency = 8 still flags multi-cycle ops (MUL/DIV libcalls) for the
// "expensive" heuristics used by the allocator.
class PenumbraSchedModel : SchedMachineModel {
  let IssueWidth = 1;            // Single-issue, microcoded
  let MicroOpBufferSize = 0;     // In-order: no reorder buffer
  let LoadLatency = 1;           // Microcoded: no benefit from hiding load latency
  let HighLatency = 8;           // Threshold for "expensive" ops
  let MispredictPenalty = 0;     // No branch predictor
  let CompleteModel = 0;         // Don't require itineraries for every opcode
}

// Each model states the issue cycles of an ALU operation, a multiply and a
// divide -- how long an instruction holds its core -- from the timing
// reference (doc/system/instruction-timing.md).  PenumbraSubtarget reads them
// back for cost decisions, so every model must state all three.
def Penumbra1Model : PenumbraSchedModel;
let SchedModel = Penumbra1Model in {
  def : WriteRes<WriteALU, []> { let Latency = 2; }
  def : WriteRes<WriteMul, []> { let Latency = 37; }
  def : WriteRes<WriteDiv, []> { let Latency = 37; }
  def : ReadAdvance<ReadALU, 0>;   // serial: nothing waits on a producer
}

// Penumbra/2 and Penumbra/2.5 hold the core for the same cycles per
// instruction.  They differ in how long a consumer waits for its producer:
// Penumbra/2 has no forwarding, so an ALU instruction reads a register only
// once the producer has left WB, three cycles later at distance one;
// Penumbra/2.5 forwards it (doc/system/instruction-timing.md, "Issue and
// latency").
multiclass Penumbra2InstrCycles {
  def : WriteRes<WriteALU, []> { let Latency = 1; }
  def : WriteRes<WriteMul, []> { let Latency = 11; }
  def : WriteRes<WriteDiv, []> { let Latency = 35; }
}

def Penumbra2Model : PenumbraSchedModel;
let SchedModel = Penumbra2Model in {
  defm : Penumbra2InstrCycles;
  def : ReadAdvance<ReadALU, -3>;
}

def Penumbra2_5Model : PenumbraSchedModel;
let SchedModel = Penumbra2_5Model in {
  defm : Penumbra2InstrCycles;
  def : ReadAdvance<ReadALU, 0>;
}

// The architecture list is empty for every processor — one ISA, one set of
// legal instructions — so processors differ only in their tune list and
// scheduling model.  A PenumbraTuning carries that pair as one record, so a
// name that borrows another core's tuning borrows both halves together and
// cannot pair one core's features with another's schedule.
class PenumbraTuning<SchedMachineModel m, list<SubtargetFeature> tunef> {
  SchedMachineModel Model = m;
  list<SubtargetFeature> TuneFeatures = tunef;
}

// Each name is accepted by both -mcpu and -mtune.
class PenumbraProcessor<string n, PenumbraTuning t>
    : ProcessorModel<n, t.Model, [], t.TuneFeatures>;

// The original minicomputer: single-issue, microcoded, no branch prediction.
def Penumbra1Tuning : PenumbraTuning<Penumbra1Model, []>;
def : PenumbraProcessor<"penumbra1", Penumbra1Tuning>;

// The pipelined core, without GPR forwarding.
def Penumbra2Tuning : PenumbraTuning<Penumbra2Model, []>;
def : PenumbraProcessor<"penumbra2", Penumbra2Tuning>;

// The pipelined core with GPR forwarding.
def Penumbra2_5Tuning : PenumbraTuning<Penumbra2_5Model, []>;
def : PenumbraProcessor<"penumbra2_5", Penumbra2_5Tuning>;

// The name a build uses when it names no core.  It is the most-used variant's
// tuning under a stable name — never a blend across generations — so moving
// the default to another variant is exactly a change of this reference.
def : PenumbraProcessor<"generic", Penumbra2_5Tuning>;

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
  // Lets post-RA passes change allocated registers: no instruction needs a
  // register its operand classes and tied constraints do not state.
  let AllowRegisterRenaming = 1;
}

// Map LLVM's built-in ptr_rc pseudo-operands to our GPR class.
defm : RemapAllTargetPseudoPointerOperands<GPR>;
