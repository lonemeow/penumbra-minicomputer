add_llvm_component_library(LLVMPenumbraInfo
  PenumbraTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  Penumbra
  )
