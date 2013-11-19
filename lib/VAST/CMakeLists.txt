add_llvm_library(ShangVAST
  Allocation.cpp
  SimpleBlockRAMAllocation.cpp
  MemoryPartition.cpp
  BasicBlockTopOrder.cpp
  VASTModulePass.cpp
  VASTExpr.cpp
  VASTExprBuilder.cpp
  VASTExprOptAnd.cpp
  VASTExprOptCmp.cpp
  VASTExprOptAddMult.cpp
  VASTMemoryPort.cpp
  Strash.cpp
  DatapathContainer.cpp
  MinimalDatapathContext.cpp
  IR2Datapath.cpp
  LowerDiv.cpp
  DesignMetrics.cpp
  LUTMapping.cpp
  IterativeBitlevelOpt.cpp
  OptimizePHINodes.cpp
  VASTSeqOp.cpp
  VASTSeqValue.cpp
  VASTSlot.cpp
  VASTSubModules.cpp
  VASTMisc.cpp
  ScriptingSupport.cpp
  ControlLogicSynthesis.cpp
  DatapathNamer.cpp
  RTLCodeGen.cpp
)

get_property(ABC_INCLUDE_DIRS GLOBAL PROPERTY ABC_INCLUDE_DIRS)
get_property(ABC_LIBRARY GLOBAL PROPERTY ABC_LIBRARY)

target_link_libraries (ShangVAST ${ABC_LIBRARY})

include_directories(${ABC_INCLUDE_DIRS})
SetupABC()
