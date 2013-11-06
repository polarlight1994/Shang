add_llvm_library(ShangIROpts
  FunctionFilter.cpp
  LowerAlloca.cpp
  PromoteGlobalVariables.cpp
  LowerIntrinsic.cpp
  LowerGetElementPtr.cpp
  HLSInliner.cpp
  GotoExpansion.cpp
  TrivialLoopUnrollPass.cpp
  BasicBlockTopOrder.cpp
  MemoryAccessCoalescing.cpp
  ShangTTI.cpp
  ObjectBasedAliasAnalysis.cpp
  )

llvm_config(ShangIROpts transformutils ipo)
