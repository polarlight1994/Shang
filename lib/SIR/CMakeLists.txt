add_llvm_library(ShangSIR
SIR.cpp
SIRBuild.cpp
SIRBitMaskAnalysis.cpp
SIRAddMulChain.cpp
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
include_directories(${PYTHON_INCLUDE_DIR})

if (MSVC)
  target_link_libraries(ShangSIR LLVMLinker ${LPSOLVE_LIBRARY} ${PYTHON_LIBRARY})
endif ()

if (NOT MSVC)
  target_link_libraries(ShangSIR LLVMLinker ${LPSOLVE_LIBRARY} ${PYTHON_LIBRARY} ${UTIL_LIBRARY})
endif ()
