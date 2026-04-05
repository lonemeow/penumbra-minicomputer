add_llvm_component_library(LLVMPenumbraDisassembler
  PenumbraDisassembler.cpp

  LINK_COMPONENTS
  CodeGenTypes
  MCDisassembler
  PenumbraDesc
  PenumbraInfo
  Support

  ADD_TO_COMPONENT
  Penumbra
  )
