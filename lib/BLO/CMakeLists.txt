add_llvm_library(ShangBLO
  LUTMapping.cpp
  DemandedOpt.cpp
  BitlevelOpt.cpp
  CarryChainOpt.cpp
  BitManipulate.cpp
)

get_property(ABC_INCLUDE_DIRS GLOBAL PROPERTY ABC_INCLUDE_DIRS)
get_property(ABC_LIBRARY GLOBAL PROPERTY ABC_LIBRARY)

target_link_libraries (
  ShangBLO
  ShangVAST
  ${ABC_LIBRARY}
)

include_directories(${ABC_INCLUDE_DIRS})
SetupABC()
