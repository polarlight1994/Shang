add_llvm_library(ShangIROpts
  FunctionFilter.cpp
  LowerAlloca.cpp
  PromoteGlobalVariables.cpp
  LowerIntrinsic.cpp
  HLSInliner.cpp
  GotoExpansion.cpp
  TrivialLoopUnrollPass.cpp
  DatapathHoisting.cpp
  BasicBlockTopOrder.cpp
  MemoryAccessCoalescing.cpp
  ShangTTI.cpp
  )

llvm_config(ShangIROpts transformutils ipo)
