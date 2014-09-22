add_subdirectory(Scheduling)
add_subdirectory(Binding)

add_llvm_library(ShangHLS
  TimingAnalysis.cpp
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
  ShangSIR
  ShangScheduling
  ShangBinding
)
