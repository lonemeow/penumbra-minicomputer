add_llvm_component_library(LLVMPenumbraDesc
  PenumbraAsmBackend.cpp
  PenumbraELFObjectWriter.cpp
  PenumbraInstPrinter.cpp
  PenumbraMCAsmInfo.cpp
  PenumbraMCCodeEmitter.cpp
  PenumbraMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  PenumbraInfo

  ADD_TO_COMPONENT
  Penumbra
  )
