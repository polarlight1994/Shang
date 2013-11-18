add_llvm_library(ShangHLS
  TimingNetlist.cpp
  TimingEstimator.cpp
  TimingScriptGen.cpp
  ExternalTimingAnalysis.cpp
  Dataflow.cpp
  PreSchedBinding.cpp
  VASTScheduling.cpp
  InnerIterativeEngine.cpp
  ScheduleEmitter.cpp
  CrossBBDepVerifier.cpp
  CFGParallelizer.cpp
  SchedulerBase.cpp
  GlobalDepBuilder.cpp
  SDCScheduler.cpp
  IterativeSchedulingDriver.cpp
  SeqLiveVariables.cpp
  STGDistances.cpp
  SelectorPipelining.cpp
  SelectorSynthesis.cpp
  CompGraph.cpp
  RegisterSharing.cpp
)
get_property(LPSOLVE_LIBRARY GLOBAL PROPERTY LPSOLVE_LIBRARY)
get_property(LPSOLVE_INCLUDE_DIR GLOBAL PROPERTY LPSOLVE_INCLUDE_DIR)

target_link_libraries (
  ShangHLS
  ShangVAST
  ${LPSOLVE_LIBRARY}
)

include_directories("${LPSOLVE_INCLUDE_DIR}/..")
