add_llvm_component_library(LLVMPenumbraAsmParser
  PenumbraAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  PenumbraDesc
  PenumbraInfo
  Support

  ADD_TO_COMPONENT
  Penumbra
  )
