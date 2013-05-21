add_llvm_library(ShangHLS
  TimingNetlist.cpp
  TimingEstimator.cpp
  TimingScriptGen.cpp
  ExternalTimingAnalysis.cpp
  VASTScheduling.cpp
  ScheduleEmitter.cpp
  RegisterFolding.cpp
  CrossBBChainFixer.cpp
  SchedulerBase.cpp
  LinearOrderBuilder.cpp
  SDCScheduler.cpp
  SeqLiveVariables.cpp
  OverlappedSlots.cpp
  STGDistances.cpp
  SelectorPipelining.cpp
  SelectorSynthesis.cpp
  RegisterSharing.cpp
)

IF (NOT MSVC)
 set(EXTRA_LIBS colamd dl)
ELSE (NOT MSVC)
 set(EXTRA_LIBS "")
ENDIF (NOT MSVC)

target_link_libraries (
  ShangHLS
  ShangVAST
  ${LPSOLVE_LIBRARY}
  ${EXTRA_LIBS}
)

include_directories(${LPSOLVE_INCLUDE_DIR})
