add_llvm_library(ShangSIR
SIR.cpp
SIRBuild.cpp
DFGBuild.cpp
DFGOpt.cpp
DFGAnalysis.cpp
SIRBitMaskAnalysis.cpp
SIRMOAOpt.cpp
SIRLowerIntrinsic.cpp
SIRAllocation.cpp
SIRTimingAnalysis.cpp
SIRSchedGraph.cpp
SIRSchedulerBase.cpp
SIRScheduling.cpp
SIRListScheduler.cpp
SIRSDCScheduler.cpp
SIRIMSScheduler.cpp
SIRRegisterSynthesis.cpp
SIRPass.cpp
SIRSTGDistance.cpp
SIRTimingScriptGen.cpp
SIRRTLCodeGen.cpp
)

include_directories(${LPSOLVE_INCLUDE_DIR})

if (MSVC)
  target_link_libraries(ShangSIR LLVMLinker ${LPSOLVE_LIBRARY})
endif ()

if (NOT MSVC)
  target_link_libraries(ShangSIR LLVMLinker ${LPSOLVE_LIBRARY})
endif ()
