add_llvm_library(ShangBinding
  CompGraph.cpp
  PreSchedBinding.cpp
  SeqLiveVariables.cpp
  RegisterSharing.cpp
)

IF (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)
 set(EXTRA_LIBS colamd dl)
ELSE (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)
 set(EXTRA_LIBS "")
ENDIF (NOT MSVC AND NOT USE_LOCAL_LP_SOLVE)

target_link_libraries (
  ShangBinding
  ShangVAST
  ${LPSOLVE_LIBRARY}
  ${EXTRA_LIBS}
)

include_directories(${LPSOLVE_INCLUDE_DIR})
