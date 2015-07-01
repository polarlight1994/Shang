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
SIRRegisterSynthesis.cpp
SIRFSMSynthesis.cpp
SIRPass.cpp
SIRRTLCodeGen.cpp
)

target_link_libraries(ShangSIR LLVMLinker)
