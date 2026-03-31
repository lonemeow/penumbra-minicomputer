add_llvm_component_group(Penumbra)

set(LLVM_TARGET_DEFINITIONS Penumbra.td)

tablegen(LLVM PenumbraGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM PenumbraGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM PenumbraGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM PenumbraGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM PenumbraGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM PenumbraGenCallingConv.inc -gen-callingconv)
tablegen(LLVM PenumbraGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(PenumbraCommonTableGen)

add_llvm_target(PenumbraCodeGen
  PenumbraFrameLowering.cpp
  PenumbraInstrInfo.cpp
  PenumbraRegisterInfo.cpp
  PenumbraTargetMachine.cpp

  LINK_COMPONENTS
  CodeGen
  CodeGenTypes
  Core
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
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
