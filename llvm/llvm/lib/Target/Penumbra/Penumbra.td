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
// how long an instruction holds it and when its result can be read — and
// nothing about how the hardware arrives there: two implementations with the
// same figures are the same core to the compiler.
//
// Every generation issues at most one instruction a cycle, in order.
// LoadLatency is the latency of a load without a scheduling class, and every
// load instruction has one.
class PenumbraSchedModel : SchedMachineModel {
  let IssueWidth = 1;
  let MicroOpBufferSize = 0;
  let LoadLatency = 1;
  let MispredictPenalty = 0;     // No branch predictor
  let CompleteModel = 0;         // Don't require itineraries for every opcode
}

// Only a pipelined core makes a reader wait for its operand, so only there
// does reordering after register allocation pay.
class PenumbraPipelinedModel : PenumbraSchedModel { let PostRAScheduler = 1; }

// A model states the figures the timing reference
// (doc/system/instruction-timing.md) gives an instruction: its issue, the
// cycles it holds the core, and for one that writes a register its latency,
// the cycles from its start until a reader can start without waiting.  The
// core is the model's one resource: a single unit that nothing queues for, so
// an instruction holding it holds every instruction behind it.
// PenumbraSubtarget reads the issue figures back for cost decisions, so every
// model must state them for ALU operations, multiplies and divides.
class PenumbraCore : ProcResource<1> { let BufferSize = 0; }

// Each helper below is an LLVM WriteRes ("write resources"): what one
// SchedWrite costs in the enclosing model, as the resources it holds and its
// latency.  The only resource is the core.  A helper is named for the
// register its figures describe.

// An instruction that writes Rd: its issue, and the latency of Rd.
class PenumbraResult<SchedWrite write, PenumbraCore core, int issue,
                     int latency> : WriteRes<write, [core]> {
  let ReleaseAtCycles = [issue];
  let Latency = latency;
}

// An instruction that writes no register: its issue alone.
class PenumbraNoResult<SchedWrite write, PenumbraCore core, int issue>
    : WriteRes<write, [core]> {
  let ReleaseAtCycles = [issue];
}

// The high half a MUL or DIV pair form writes to Rdh: its latency alone,
// since the instruction's issue is stated with its Rd result.
class PenumbraHighHalf<SchedWrite write, int latency> : WriteRes<write, []> {
  let Latency = latency;
  let NumMicroOps = 0;
}

def Penumbra1Model : PenumbraSchedModel;
let SchedModel = Penumbra1Model in {
  def Penumbra1Core : PenumbraCore;
  def : PenumbraResult<WriteALU, Penumbra1Core, 2, 2>;
  def : PenumbraResult<WriteLoad, Penumbra1Core, 4, 4>;
  def : PenumbraNoResult<WriteStore, Penumbra1Core, 8>;
  def : PenumbraResult<WriteMul, Penumbra1Core, 37, 37>;
  def : PenumbraHighHalf<WriteMulHigh, 37>;
  def : PenumbraResult<WriteDiv, Penumbra1Core, 37, 37>;
  def : PenumbraHighHalf<WriteDivHigh, 37>;
  def : ReadAdvance<ReadALU, 0>;
}

// Penumbra/2 and Penumbra/2.5 hold the core for the same cycles per
// instruction.  Penumbra/2 has no forwarding, so a reader waits for its
// producer to leave WB.  Penumbra/2.5 forwards every result but a MUL's or
// DIV's high half, which arrives as late as on Penumbra/2.
def Penumbra2Model : PenumbraPipelinedModel;
let SchedModel = Penumbra2Model in {
  def Penumbra2Core : PenumbraCore;
  def : PenumbraResult<WriteALU, Penumbra2Core, 1, 4>;
  def : PenumbraResult<WriteLoad, Penumbra2Core, 2, 5>;
  def : PenumbraNoResult<WriteStore, Penumbra2Core, 6>;
  def : PenumbraResult<WriteMul, Penumbra2Core, 11, 14>;
  def : PenumbraHighHalf<WriteMulHigh, 14>;
  def : PenumbraResult<WriteDiv, Penumbra2Core, 35, 38>;
  def : PenumbraHighHalf<WriteDivHigh, 38>;
  def : ReadAdvance<ReadALU, 0>;
}

def Penumbra2_5Model : PenumbraPipelinedModel;
let SchedModel = Penumbra2_5Model in {
  def Penumbra2_5Core : PenumbraCore;
  def : PenumbraResult<WriteALU, Penumbra2_5Core, 1, 1>;
  def : PenumbraResult<WriteLoad, Penumbra2_5Core, 2, 3>;
  def : PenumbraNoResult<WriteStore, Penumbra2_5Core, 6>;
  def : PenumbraResult<WriteMul, Penumbra2_5Core, 11, 11>;
  def : PenumbraHighHalf<WriteMulHigh, 14>;
  def : PenumbraResult<WriteDiv, Penumbra2_5Core, 35, 35>;
  def : PenumbraHighHalf<WriteDivHigh, 38>;
  def : ReadAdvance<ReadALU, 0>;
}

// Penumbra/3 issues a load or a store in one cycle.  A load's reader waits
// two cycles longer than an ALU result's.  A store enters a store buffer, and
// its issue is the reference's typical figure, while the buffer has room.
def Penumbra3Model : PenumbraPipelinedModel;
let SchedModel = Penumbra3Model in {
  def Penumbra3Core : PenumbraCore;
  def : PenumbraResult<WriteALU, Penumbra3Core, 1, 1>;
  def : PenumbraResult<WriteLoad, Penumbra3Core, 1, 3>;
  def : PenumbraNoResult<WriteStore, Penumbra3Core, 1>;
  def : PenumbraResult<WriteMul, Penumbra3Core, 11, 11>;
  def : PenumbraHighHalf<WriteMulHigh, 14>;
  def : PenumbraResult<WriteDiv, Penumbra3Core, 35, 35>;
  def : PenumbraHighHalf<WriteDivHigh, 38>;
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

// The seven-stage pipelined core, with a store buffer and branch prediction at
// fetch.
def Penumbra3Tuning : PenumbraTuning<Penumbra3Model, []>;
def : PenumbraProcessor<"penumbra3", Penumbra3Tuning>;

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
