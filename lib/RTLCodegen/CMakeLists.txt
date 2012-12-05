add_llvm_library(VTMRTLCodegen
  RTLCodegenPrepare.cpp
  VerilogModuleAnalysis.cpp
  VASTExprBuilder.cpp
  VerilogASTBuilder.cpp
  VerilogASTWriter.cpp
  IR2Datapath.cpp
  DesignMetrics.cpp
  BitLevelDelayInfo.cpp
  MachineFunction2Datapath.cpp
  MFDatapathContainer.cpp
  PreSchedRTLOpt.cpp
  ControlLogicBuilder.cpp
  CombPathDelayAnalysis.cpp
  RtlSSAAnalysis.cpp
)

add_dependencies(VTMRTLCodegen VerilogBackendTableGen)
