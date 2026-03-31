add_llvm_component_library(LLVMPenumbraDesc
  PenumbraMCAsmInfo.cpp
  PenumbraMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  PenumbraInfo

  ADD_TO_COMPONENT
  Penumbra
  )
