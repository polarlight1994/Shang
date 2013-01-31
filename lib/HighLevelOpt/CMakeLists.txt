add_llvm_library(ShangHighLevelOpt
  FunctionFilter.cpp
  HLSInliner.cpp
  TrivialLoopUnrollPass.cpp
  DatapathCodehMotion.cpp
  BasicBlockTopOrder.cpp
  )

llvm_config(ShangHighLevelOpt transformutils ipo)
