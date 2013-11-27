add_llvm_library(ShangScheduling
  VASTScheduling.cpp
  InnerIterativeEngine.cpp
  ScheduleEmitter.cpp
  CrossBBDepVerifier.cpp
  CFGParallelizer.cpp
  SchedulerBase.cpp
  GlobalDepBuilder.cpp
  SDCScheduler.cpp
  ControlHazardResolver.cpp
  LagSDCSolver.cpp
  IterativeSchedulingDriver.cpp
)

IF (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)
 set(EXTRA_LIBS colamd dl)
ELSE (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)
 set(EXTRA_LIBS "")
ENDIF (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)

target_link_libraries (
  ShangScheduling
  ShangVAST
  ${LPSOLVE_LIBRARY}
  ${EXTRA_LIBS}
)

include_directories(${LPSOLVE_INCLUDE_DIR})

# Need to link against the analysis in linux, otherwise we will get a link
# error.
llvm_config(ShangScheduling analysis)
