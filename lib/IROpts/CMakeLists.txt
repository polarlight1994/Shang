add_llvm_library(ShangIROpts
  FunctionFilter.cpp
  LowerAlloca.cpp
  HLSInliner.cpp
  TrivialLoopUnrollPass.cpp
  DatapathCodehMotion.cpp
  BasicBlockTopOrder.cpp
  )

llvm_config(ShangIROpts transformutils ipo)
