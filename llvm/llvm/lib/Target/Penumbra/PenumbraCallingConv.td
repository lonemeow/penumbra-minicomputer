//===-- PenumbraCallingConv.td - Calling Conventions -------*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Calling convention for Penumbra (see doc/abi/penumbra-abi.md):
//   Arguments:  R1-R4 (first 4 scalar args), then stack (4-byte slots)
//   Return:     R1 (32-bit), R1+R2 (64-bit)
//   Callee-saved: R5-R10
//   Caller-saved: R1-R4, R11 (scratch), R13 (LR)
//   Reserved:   R0 (zero), R12 (TP), R14 (SP), R15 (PC)
//
//===----------------------------------------------------------------------===//


def CC_Penumbra : CallingConv<[
    CCIfType<[i1, i8, i16], CCPromoteToType<i32>>,
    CCIfType<[i32], CCAssignToReg<[R1, R2, R3, R4]>>,
    CCIfType<[i32], CCAssignToStack<4, 4>>
]>;

def RetCC_Penumbra : CallingConv<[
    CCIfType<[i1, i8, i16], CCPromoteToType<i32>>,
    CCIfType<[i32], CCAssignToReg<[R1]>>
]>;

// R13 (LR) is included so LLVM saves/restores it in functions that make
// calls — without it, nested BL clobbers the return address.
def CSR_Penumbra : CalleeSavedRegs<(add (sequence "R%u", 5, 10), R13)>;
