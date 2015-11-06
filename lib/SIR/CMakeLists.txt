add_llvm_library(ShangSIR
SIR.cpp
SIRBuild.cpp
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
SIRRTLCodeGen.cpp
)

include_directories(${LPSOLVE_INCLUDE_DIR})

target_link_libraries(ShangSIR LLVMLinker ${LPSOLVE_LIBRARY})
