add_llvm_component_group(Penumbra)

set(LLVM_TARGET_DEFINITIONS Penumbra.td)

tablegen(LLVM PenumbraGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM PenumbraGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM PenumbraGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM PenumbraGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM PenumbraGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM PenumbraGenCallingConv.inc -gen-callingconv)
tablegen(LLVM PenumbraGenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM PenumbraGenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM PenumbraGenRegisterBank.inc -gen-register-bank)

set(LLVM_TARGET_DEFINITIONS PenumbraGISel.td)
tablegen(LLVM PenumbraGenGlobalISel.inc -gen-global-isel)

add_public_tablegen_target(PenumbraCommonTableGen)

add_llvm_target(PenumbraCodeGen
  GISel/PenumbraCallLowering.cpp
  GISel/PenumbraInstructionSelector.cpp
  PenumbraAsmPrinter.cpp
  GISel/PenumbraLegalizerInfo.cpp
  GISel/PenumbraRegisterBankInfo.cpp
  PenumbraFrameLowering.cpp
  PenumbraISelLowering.cpp
  PenumbraInstrInfo.cpp
  PenumbraRegisterInfo.cpp
  PenumbraSubtarget.cpp
  PenumbraTargetMachine.cpp

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  GlobalISel
  MC
  PenumbraAsmParser
  PenumbraDesc
  PenumbraInfo
  Support
  Target
  TargetParser

  ADD_TO_COMPONENT
  Penumbra
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
