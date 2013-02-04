add_llvm_library(ShangIROpts
  FunctionFilter.cpp
  HLSInliner.cpp
  TrivialLoopUnrollPass.cpp
  DatapathCodehMotion.cpp
  BasicBlockTopOrder.cpp
  )

llvm_config(ShangIROpts transformutils ipo)
