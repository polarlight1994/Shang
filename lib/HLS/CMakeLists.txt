add_subdirectory(Scheduling)
add_subdirectory(Binding)

add_llvm_library(ShangHLS
  TimingNetlist.cpp
  TimingEstimator.cpp
  TimingScriptGen.cpp
  ExternalTimingAnalysis.cpp
  Dataflow.cpp
  STGDistances.cpp
  SelectorPipelining.cpp
  SelectorSynthesis.cpp
)

target_link_libraries (
  ShangHLS
  ShangVAST
  ShangScheduling
  ShangBinding
)
