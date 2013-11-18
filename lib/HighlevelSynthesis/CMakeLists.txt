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

IF (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)
 set(EXTRA_LIBS colamd dl)
ELSE (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)
 set(EXTRA_LIBS "")
ENDIF (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)

target_link_libraries (
  ShangHLS
  ShangVAST
  ${LPSOLVE_LIBRARY}
  ${EXTRA_LIBS}
)

include_directories(${LPSOLVE_INCLUDE_DIR})
