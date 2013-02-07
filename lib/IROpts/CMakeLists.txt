add_llvm_library(ShangIROpts
  FunctionFilter.cpp
  LowerAlloca.cpp
  HLSInliner.cpp
  TrivialLoopUnrollPass.cpp
  DatapathHoisting.cpp
  BasicBlockTopOrder.cpp
  ShangTTI.cpp
  )

llvm_config(ShangIROpts transformutils ipo)
