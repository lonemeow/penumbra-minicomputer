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
tablegen(LLVM PenumbraGenPreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="PenumbraPreLegalizerCombiner")
tablegen(LLVM PenumbraGenO0PreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="PenumbraO0PreLegalizerCombiner")
tablegen(LLVM PenumbraGenPostLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="PenumbraPostLegalizerCombiner")

add_public_tablegen_target(PenumbraCommonTableGen)

add_llvm_target(PenumbraCodeGen
  GISel/PenumbraCallLowering.cpp
  GISel/PenumbraInstructionSelector.cpp
  GISel/PenumbraO0PreLegalizerCombiner.cpp
  GISel/PenumbraPostLegalizerCombiner.cpp
  GISel/PenumbraPreLegalizerCombiner.cpp
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
