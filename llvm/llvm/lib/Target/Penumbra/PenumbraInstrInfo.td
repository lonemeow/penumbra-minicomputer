//===-- PenumbraInstrInfo.td - Penumbra Instruction defs ----*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//
// Penumbra instruction definitions.
//
// Encoding formats (all 32-bit, 2-bit prefix):
//   Format R [00]: Register-register ALU and system ops
//   Format L [01]: Immediate operations
//   Format M [10]: Memory load/store (register + offset)
//   Format B [11]: Branch (conditional/unconditional/BL)
//
//===----------------------------------------------------------------------===//

def PenumbraInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// Instruction format base classes
//===----------------------------------------------------------------------===//

// Base class for all Penumbra instructions — 32-bit fixed width.
class PenumbraInst<dag outs, dag ins, string asmstr,
                   list<dag> pattern = []>
    : Instruction {
  let Namespace = "Penumbra";
  let Size = 4;

  field bits<32> Inst;
  field bits<32> SoftFail = 0;

  dag OutOperandList = outs;
  dag InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;
}

//--- Format R: prefix = 00 ---------------------------------------------------
// [31:30]=00  [29:25]=op  [24:21]=Rd  [20:17]=Rs  [16]=F  [15:0]=spare
class FormatR<bits<5> op, dag outs, dag ins, string asmstr,
              list<dag> pattern = []>
    : PenumbraInst<outs, ins, asmstr, pattern> {
  bits<4> Rd;
  bits<4> Rs;
  bit F = 0;

  let Inst{31-30} = 0b00;
  let Inst{29-25} = op;
  let Inst{24-21} = Rd;
  let Inst{20-17} = Rs;
  let Inst{16}    = F;
  let Inst{15-0}  = 0;
}

// Format R with only Rs (e.g. JMP Rs) — Rd and F fixed to 0.
class FormatR_Rs<bits<5> op, dag outs, dag ins, string asmstr,
                 list<dag> pattern = []>
    : PenumbraInst<outs, ins, asmstr, pattern> {
  bits<4> Rs;

  let Inst{31-30} = 0b00;
  let Inst{29-25} = op;
  let Inst{24-21} = 0;
  let Inst{20-17} = Rs;
  let Inst{16}    = 0;
  let Inst{15-0}  = 0;
}

// Format R with no register operands (e.g. ERET, EI, DI, SYSCALL, BREAK).
class FormatR_NoReg<bits<5> op, dag outs, dag ins, string asmstr,
                    list<dag> pattern = []>
    : PenumbraInst<outs, ins, asmstr, pattern> {
  let Inst{31-30} = 0b00;
  let Inst{29-25} = op;
  let Inst{24-21} = 0;
  let Inst{20-17} = 0;
  let Inst{16}    = 0;
  let Inst{15-0}  = 0;
}

//--- Format L: prefix = 01 ---------------------------------------------------
// [31:30]=01  [29:26]=op  [25:22]=Rd  [21:16]=spare  [15:0]=imm16
class FormatL<bits<4> op, dag outs, dag ins, string asmstr,
              list<dag> pattern = []>
    : PenumbraInst<outs, ins, asmstr, pattern> {
  bits<4> Rd;
  bits<16> imm16;

  let Inst{31-30} = 0b01;
  let Inst{29-26} = op;
  let Inst{25-22} = Rd;
  let Inst{21-16} = 0;
  let Inst{15-0}  = imm16;
}

//--- Format M: prefix = 10 ---------------------------------------------------
// [31:30]=10  [29]=L  [28:27]=sz  [26]=SE  [25:22]=Rd  [21:18]=Rb
// [17:2]=offset16  [1:0]=spare
class FormatM<bit L, bits<2> sz, bit SE, dag outs, dag ins, string asmstr,
              list<dag> pattern = []>
    : PenumbraInst<outs, ins, asmstr, pattern> {
  bits<4> Rd;
  bits<4> Rb;
  bits<16> offset;

  let Inst{31-30} = 0b10;
  let Inst{29}    = L;
  let Inst{28-27} = sz;
  let Inst{26}    = SE;
  let Inst{25-22} = Rd;
  let Inst{21-18} = Rb;
  let Inst{17-2}  = offset;
  let Inst{1-0}   = 0;
}

//--- Format B: prefix = 11 ---------------------------------------------------
// [31:30]=11  [29:26]=cond  [25:4]=offset22  [3:0]=spare
class FormatB<bits<4> cond, dag outs, dag ins, string asmstr,
              list<dag> pattern = []>
    : PenumbraInst<outs, ins, asmstr, pattern> {
  bits<22> target;

  let Inst{31-30} = 0b11;
  let Inst{29-26} = cond;
  let Inst{25-4}  = target;
  let Inst{3-0}   = 0;
}

//===----------------------------------------------------------------------===//
// Custom operand types
//===----------------------------------------------------------------------===//

// ── Immediate predicates for GlobalISel pattern matching ────────────────────

// Unsigned 16-bit immediate (0–65535) — fits in a single LLI or ALU-imm field.
def uimm16 : ImmLeaf<i32, [{return isUInt<16>(Imm);}]>;

// Signed 16-bit immediate (-32768–32767) — fits in LLIS sign-extended field.
def simm16 : ImmLeaf<i32, [{return isInt<16>(Imm);}]>;

// 5-bit shift amount (0–31).
def uimm5 : ImmLeaf<i32, [{return isUInt<5>(Imm);}]>;

// ── Operand types with encoder methods ──────────────────────────────────────

// 22-bit PC-relative branch target (word offset, stored in bits [25:4]).
// The EncoderMethod handles symbolic expressions by creating a fixup.
def brtarget22 : Operand<i32> {
  let EncoderMethod = "encodeBranchTarget";
}

// 16-bit immediate that may reference a symbol (e.g. LLI Rd, #label_lo).
// The EncoderMethod handles symbolic expressions by creating a fixup.
def imm16op : Operand<i32> {
  let EncoderMethod = "encodeImm16";
}

// 16-bit memory offset for Format M (bits [17:2]).
// Expression operands create fixup_penumbra_memoffset16 targeting bits [17:2].
def memoffset16 : Operand<i32> {
  let EncoderMethod = "encodeMemOffset16";
}

//===----------------------------------------------------------------------===//
// ALU instructions (Format R, prefix=00, op 0x00-0x09)
//===----------------------------------------------------------------------===//

let Defs = [SR], Constraints = "$Rd = $Rd_in" in {
  // 2-operand destructive: Rd = Rd op Rs
  def ADD  : FormatR<0b00000, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "add\t$Rd, $Rs">;
  def SUB  : FormatR<0b00001, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "sub\t$Rd, $Rs">;
  def AND  : FormatR<0b00010, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "and\t$Rd, $Rs">;
  def OR   : FormatR<0b00011, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "or\t$Rd, $Rs">;
  def XOR  : FormatR<0b00100, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "xor\t$Rd, $Rs">;
  def SHL  : FormatR<0b00101, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "shl\t$Rd, $Rs">;
  def SHR  : FormatR<0b00110, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "shr\t$Rd, $Rs">;
  def SAR  : FormatR<0b00111, (outs GPR:$Rd), (ins GPR:$Rd_in, GPR:$Rs),
                     "sar\t$Rd, $Rs">;
}

// MOV doesn't set flags (per ISA doc)
def MOV  : FormatR<0b01000, (outs GPR:$Rd), (ins GPR:$Rs),
                   "mov\t$Rd, $Rs">;
// NOT doesn't set flags
def NOT  : FormatR<0b01001, (outs GPR:$Rd), (ins GPR:$Rs),
                   "not\t$Rd, $Rs">;

// Flag-only variants (F=1): CMP = SUB with F=1, TEST = AND with F=1
let F = 1 in {
  let Defs = [SR] in {
    def CMP  : FormatR<0b00001, (outs), (ins GPR:$Rd, GPR:$Rs),
                       "cmp\t$Rd, $Rs">;
    def TEST : FormatR<0b00010, (outs), (ins GPR:$Rd, GPR:$Rs),
                       "test\t$Rd, $Rs">;
  }
}

//===----------------------------------------------------------------------===//
// Immediate instructions (Format L, prefix=01)
//===----------------------------------------------------------------------===//

def LLI  : FormatL<0b0000, (outs GPR:$Rd), (ins imm16op:$imm16),
                   "lli\t$Rd, $imm16">;
def LLIS : FormatL<0b0001, (outs GPR:$Rd), (ins imm16op:$imm16),
                   "llis\t$Rd, $imm16">;
let Constraints = "$Rd = $Rd_in" in
def LUI  : FormatL<0b0010, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                   "lui\t$Rd, $imm16">;

let Defs = [SR] in {
  let Constraints = "$Rd = $Rd_in" in {
    def ADDi : FormatL<0b0011, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                       "add\t$Rd, $imm16">;
    def SUBi : FormatL<0b0100, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                       "sub\t$Rd, $imm16">;
  }
  def CMPi : FormatL<0b0101, (outs), (ins GPR:$Rd, imm16op:$imm16),
                     "cmp\t$Rd, $imm16">;
  let Constraints = "$Rd = $Rd_in" in
  def ANDi : FormatL<0b0110, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                     "and\t$Rd, $imm16">;
  def TESTi : FormatL<0b0111, (outs), (ins GPR:$Rd, imm16op:$imm16),
                      "test\t$Rd, $imm16">;

  // Immediate shifts: shift amount in low 5 bits of imm16 field.
  let Constraints = "$Rd = $Rd_in" in {
    def SHLi : FormatL<0b1000, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                       "shl\t$Rd, $imm16">;
    def SHRi : FormatL<0b1001, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                       "shr\t$Rd, $imm16">;
    def SARi : FormatL<0b1010, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                       "sar\t$Rd, $imm16">;
  }
}

//===----------------------------------------------------------------------===//
// Memory instructions (Format M, prefix=10)
//===----------------------------------------------------------------------===//

// Loads: L=1
let mayLoad = 1 in {
  def LDW  : FormatM<1, 0b10, 0, (outs GPR:$Rd), (ins GPR:$Rb, memoffset16:$offset),
                     "ldw\t$Rd, [$Rb + $offset]">;
  def LDH  : FormatM<1, 0b01, 0, (outs GPR:$Rd), (ins GPR:$Rb, memoffset16:$offset),
                     "ldh\t$Rd, [$Rb + $offset]">;
  def LDHS : FormatM<1, 0b01, 1, (outs GPR:$Rd), (ins GPR:$Rb, memoffset16:$offset),
                     "ldhs\t$Rd, [$Rb + $offset]">;
  def LDB  : FormatM<1, 0b00, 0, (outs GPR:$Rd), (ins GPR:$Rb, memoffset16:$offset),
                     "ldb\t$Rd, [$Rb + $offset]">;
  def LDBS : FormatM<1, 0b00, 1, (outs GPR:$Rd), (ins GPR:$Rb, memoffset16:$offset),
                     "ldbs\t$Rd, [$Rb + $offset]">;
}

// Stores: L=0
let mayStore = 1 in {
  def STW  : FormatM<0, 0b10, 0, (outs), (ins GPRz:$Rd, GPR:$Rb, memoffset16:$offset),
                     "stw\t$Rd, [$Rb + $offset]">;
  def STH  : FormatM<0, 0b01, 0, (outs), (ins GPRz:$Rd, GPR:$Rb, memoffset16:$offset),
                     "sth\t$Rd, [$Rb + $offset]">;
  def STB  : FormatM<0, 0b00, 0, (outs), (ins GPRz:$Rd, GPR:$Rb, memoffset16:$offset),
                     "stb\t$Rd, [$Rb + $offset]">;
}

//===----------------------------------------------------------------------===//
// Branch instructions (Format B, prefix=11)
//===----------------------------------------------------------------------===//

let isBranch = 1, isTerminator = 1 in {
  def B    : FormatB<0b0000, (outs), (ins brtarget22:$target), "b\t$target"> {
    let isBarrier = 1;
  }

  // Conditional branches read the status register (Z, N, C, V flags).
  let Uses = [SR] in {
    def BEQ  : FormatB<0b0001, (outs), (ins brtarget22:$target), "beq\t$target">;
    def BNE  : FormatB<0b0010, (outs), (ins brtarget22:$target), "bne\t$target">;
    def BCS  : FormatB<0b0011, (outs), (ins brtarget22:$target), "bcs\t$target">;
    def BCC  : FormatB<0b0100, (outs), (ins brtarget22:$target), "bcc\t$target">;
    def BMI  : FormatB<0b0101, (outs), (ins brtarget22:$target), "bmi\t$target">;
    def BPL  : FormatB<0b0110, (outs), (ins brtarget22:$target), "bpl\t$target">;
    def BVS  : FormatB<0b0111, (outs), (ins brtarget22:$target), "bvs\t$target">;
    def BVC  : FormatB<0b1000, (outs), (ins brtarget22:$target), "bvc\t$target">;
    def BHI  : FormatB<0b1001, (outs), (ins brtarget22:$target), "bhi\t$target">;
    def BLS  : FormatB<0b1010, (outs), (ins brtarget22:$target), "bls\t$target">;
    def BGE  : FormatB<0b1011, (outs), (ins brtarget22:$target), "bge\t$target">;
    def BLT  : FormatB<0b1100, (outs), (ins brtarget22:$target), "blt\t$target">;
    def BGT  : FormatB<0b1101, (outs), (ins brtarget22:$target), "bgt\t$target">;
    def BLE  : FormatB<0b1110, (outs), (ins brtarget22:$target), "ble\t$target">;
  }
}

let isCall = 1, Defs = [R13] in
def BL   : FormatB<0b1111, (outs), (ins brtarget22:$target), "bl\t$target">;

// Branch mnemonic aliases (ARM-style unsigned comparison synonyms).
def : MnemonicAlias<"bhs", "bcs">;  // unsigned higher or same = carry set
def : MnemonicAlias<"blo", "bcc">;  // unsigned lower = carry clear

//===----------------------------------------------------------------------===//
// System instructions (Format R, prefix=00, op 0x10-0x1C)
//===----------------------------------------------------------------------===//

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def JMP  : FormatR_Rs<0b11000, (outs), (ins GPR:$Rs), "jmp\t$Rs">;

// Indirect branch (non-return) — same encoding as JMP but not marked isReturn,
// so the epilogue inserter doesn't place stack restore before it.
// Used by jump tables where the targets are within the same function.
let isBranch = 1, isTerminator = 1, isBarrier = 1, isIndirectBranch = 1 in
def BRIND : FormatR_Rs<0b11000, (outs), (ins GPR:$Rs), "jmp\t$Rs">;

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def ERET : FormatR_NoReg<0b10110, (outs), (ins), "eret">;

def EI   : FormatR_NoReg<0b11001, (outs), (ins), "ei">;
def DI   : FormatR_NoReg<0b11010, (outs), (ins), "di">;

// WRSYS Rd, #dev, #reg — write Rd to system register.
// Hardware reads source from Rd field (bits [24:21]).
// Device ID in bits [15:12], register ID in bits [11:8].
let hasSideEffects = 1 in
def WRSYS : PenumbraInst<(outs), (ins GPR:$Rd, i32imm:$dev, i32imm:$reg),
                         "wrsys\t$Rd, $dev, $reg"> {
  bits<4> Rd;
  bits<4> dev;
  bits<4> reg;

  let Inst{31-30} = 0b00;
  let Inst{29-25} = 0b10000;   // op = 16
  let Inst{24-21} = Rd;
  let Inst{20-17} = 0;
  let Inst{16}    = 0;
  let Inst{15-12} = dev;
  let Inst{11-8}  = reg;
  let Inst{7-0}   = 0;
}

// RDSYS Rd, #dev, #reg — read system register into Rd.
let hasSideEffects = 1 in
def RDSYS : PenumbraInst<(outs GPR:$Rd), (ins i32imm:$dev, i32imm:$reg),
                         "rdsys\t$Rd, $dev, $reg"> {
  bits<4> Rd;
  bits<4> dev;
  bits<4> reg;

  let Inst{31-30} = 0b00;
  let Inst{29-25} = 0b10001;   // op = 17
  let Inst{24-21} = Rd;
  let Inst{20-17} = 0;
  let Inst{16}    = 0;
  let Inst{15-12} = dev;
  let Inst{11-8}  = reg;
  let Inst{7-0}   = 0;
}

// RDSPR Rd, #spr — read special-purpose register into Rd.
// SPR number in bits [15:12].  SPRs: EPC=0, ESR=1, USP=2.
let hasSideEffects = 1 in
def RDSPR : PenumbraInst<(outs GPR:$Rd), (ins i32imm:$spr),
                         "rdspr\t$Rd, $spr"> {
  bits<4> Rd;
  bits<4> spr;

  let Inst{31-30} = 0b00;
  let Inst{29-25} = 0b11100;   // op = 28, dispatch 0x58
  let Inst{24-21} = Rd;
  let Inst{20-17} = 0;
  let Inst{16}    = 0;
  let Inst{15-12} = spr;
  let Inst{11-0}  = 0;
}

// WRSPR #spr, Rd — write Rd to special-purpose register.
// Hardware reads source from Rd field (bits [24:21]), not Rs.
let hasSideEffects = 1 in
def WRSPR : PenumbraInst<(outs), (ins i32imm:$spr, GPR:$Rd),
                         "wrspr\t$spr, $Rd"> {
  bits<4> spr;
  bits<4> Rd;

  let Inst{31-30} = 0b00;
  let Inst{29-25} = 0b11011;   // op = 27, dispatch 0x56
  let Inst{24-21} = Rd;
  let Inst{20-17} = 0;
  let Inst{16}    = 0;
  let Inst{15-12} = spr;
  let Inst{11-0}  = 0;
}

def SYSCALL : FormatR_NoReg<0b10100, (outs), (ins), "syscall">;
def BREAK   : FormatR_NoReg<0b10101, (outs), (ins), "break">;

//===----------------------------------------------------------------------===//
// Pseudo-instructions
//===----------------------------------------------------------------------===//

let isPseudo = 1 in {
  def NOP : PenumbraInst<(outs), (ins), "nop">;

  // Stack frame adjustment pseudo-instructions. Required by PenumbraGenInstrInfo.
  // Converted to real SUBi/ADDi SP adjustments by PenumbraFrameLowering.
  def ADJCALLSTACKDOWN : PenumbraInst<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                                      "# ADJCALLSTACKDOWN $amt1, $amt2">;
  def ADJCALLSTACKUP   : PenumbraInst<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                                      "# ADJCALLSTACKUP $amt1, $amt2">;

  // RET pseudo: expands to JMP R13 (return via link register).
  let isReturn = 1, isTerminator = 1, isBarrier = 1 in
  def RET : PenumbraInst<(outs), (ins), "ret">;

  // LEAfi: load effective address of a frame index (stack slot).
  // Materialises SP + offset into a register.  The frame index is
  // embedded in operand 1; eliminateFrameIndex replaces it with R14
  // and fills in the real offset.  expandPostRAPseudo expands into
  // MOV Rd, R14 + ADDi Rd, #offset.
  def LEAfi : PenumbraInst<(outs GPR:$Rd), (ins GPR:$base, i32imm:$offset),
                           "# LEAfi $Rd, $base, $offset">;

  // SELECT_GPR: conditional select pseudo, expanded after regalloc by
  // EmitInstrWithCustomInserter into a diamond of basic blocks + PHI.
  // Uses TEST+BNE on the condition register.
  let usesCustomInserter = 1 in
  def SELECT_GPR : PenumbraInst<(outs GPR:$dst),
                                (ins GPR:$trueval, GPR:$falseval, GPR:$cond),
                                "# SELECT_GPR $dst, $trueval, $falseval, $cond">;

  // SELECT_CC_GPR: like SELECT_GPR, but carries CMP operands and a branch
  // opcode directly — avoids the TEST+BNE round-trip when the condition
  // originates from G_ICMP.  The $cc immediate is a Penumbra branch opcode.
  let usesCustomInserter = 1 in
  def SELECT_CC_GPR : PenumbraInst<(outs GPR:$dst),
      (ins GPR:$trueval, GPR:$falseval, GPR:$lhs, GPR:$rhs, i32imm:$cc),
      "# SELECT_CC_GPR $dst, $trueval, $falseval, $lhs, $rhs, $cc">;
}
