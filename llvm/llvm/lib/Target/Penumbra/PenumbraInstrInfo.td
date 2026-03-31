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
// [31:30]=01  [29:27]=op  [26:23]=Rd  [22:16]=spare  [15:0]=imm16
class FormatL<bits<3> op, dag outs, dag ins, string asmstr,
              list<dag> pattern = []>
    : PenumbraInst<outs, ins, asmstr, pattern> {
  bits<4> Rd;
  bits<16> imm16;

  let Inst{31-30} = 0b01;
  let Inst{29-27} = op;
  let Inst{26-23} = Rd;
  let Inst{22-16} = 0;
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

def LLI  : FormatL<0b000, (outs GPR:$Rd), (ins imm16op:$imm16),
                   "lli\t$Rd, $imm16">;
def LLIS : FormatL<0b001, (outs GPR:$Rd), (ins imm16op:$imm16),
                   "llis\t$Rd, $imm16">;
let Constraints = "$Rd = $Rd_in" in
def LUI  : FormatL<0b010, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                   "lui\t$Rd, $imm16">;

let Defs = [SR] in {
  let Constraints = "$Rd = $Rd_in" in {
    def ADDi : FormatL<0b011, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                       "add\t$Rd, $imm16">;
    def SUBi : FormatL<0b100, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                       "sub\t$Rd, $imm16">;
  }
  def CMPi : FormatL<0b101, (outs), (ins GPR:$Rd, imm16op:$imm16),
                     "cmp\t$Rd, $imm16">;
  let Constraints = "$Rd = $Rd_in" in
  def ANDi : FormatL<0b110, (outs GPR:$Rd), (ins GPR:$Rd_in, imm16op:$imm16),
                     "and\t$Rd, $imm16">;
  def TESTi : FormatL<0b111, (outs), (ins GPR:$Rd, imm16op:$imm16),
                      "test\t$Rd, $imm16">;
}

//===----------------------------------------------------------------------===//
// Memory instructions (Format M, prefix=10)
//===----------------------------------------------------------------------===//

// Loads: L=1
def LDW  : FormatM<1, 0b10, 0, (outs GPR:$Rd), (ins GPR:$Rb, i32imm:$offset),
                   "ldw\t$Rd, [$Rb + $offset]">;
def LDH  : FormatM<1, 0b01, 0, (outs GPR:$Rd), (ins GPR:$Rb, i32imm:$offset),
                   "ldh\t$Rd, [$Rb + $offset]">;
def LDHS : FormatM<1, 0b01, 1, (outs GPR:$Rd), (ins GPR:$Rb, i32imm:$offset),
                   "ldhs\t$Rd, [$Rb + $offset]">;
def LDB  : FormatM<1, 0b00, 0, (outs GPR:$Rd), (ins GPR:$Rb, i32imm:$offset),
                   "ldb\t$Rd, [$Rb + $offset]">;
def LDBS : FormatM<1, 0b00, 1, (outs GPR:$Rd), (ins GPR:$Rb, i32imm:$offset),
                   "ldbs\t$Rd, [$Rb + $offset]">;

// Stores: L=0
def STW  : FormatM<0, 0b10, 0, (outs), (ins GPR:$Rd, GPR:$Rb, i32imm:$offset),
                   "stw\t$Rd, [$Rb + $offset]">;
def STH  : FormatM<0, 0b01, 0, (outs), (ins GPR:$Rd, GPR:$Rb, i32imm:$offset),
                   "sth\t$Rd, [$Rb + $offset]">;
def STB  : FormatM<0, 0b00, 0, (outs), (ins GPR:$Rd, GPR:$Rb, i32imm:$offset),
                   "stb\t$Rd, [$Rb + $offset]">;

//===----------------------------------------------------------------------===//
// Branch instructions (Format B, prefix=11)
//===----------------------------------------------------------------------===//

let isBranch = 1, isTerminator = 1 in {
  def B    : FormatB<0b0000, (outs), (ins brtarget22:$target), "b\t$target"> {
    let isBarrier = 1;
  }
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

let isCall = 1, Defs = [R13] in
def BL   : FormatB<0b1111, (outs), (ins brtarget22:$target), "bl\t$target">;

//===----------------------------------------------------------------------===//
// System instructions (Format R, prefix=00, op 0x10-0x1C)
//===----------------------------------------------------------------------===//

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def JMP  : FormatR_Rs<0b11000, (outs), (ins GPR:$Rs), "jmp\t$Rs">;

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def ERET : FormatR_NoReg<0b10110, (outs), (ins), "eret">;

def EI   : FormatR_NoReg<0b11001, (outs), (ins), "ei">;
def DI   : FormatR_NoReg<0b11010, (outs), (ins), "di">;

def SYSCALL : FormatR_NoReg<0b10100, (outs), (ins), "syscall">;
def BREAK   : FormatR_NoReg<0b10101, (outs), (ins), "break">;

//===----------------------------------------------------------------------===//
// Pseudo-instructions
//===----------------------------------------------------------------------===//

let isPseudo = 1 in {
  def NOP : PenumbraInst<(outs), (ins), "nop">;
}
