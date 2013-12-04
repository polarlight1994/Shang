add_subdirectory(abc)
set_property(GLOBAL PROPERTY LUA_BRIDGE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/LuaBridge/Source")

option(USE_LOCAL_LP_SOLVE "Use the local version of lp_solve" OFF)
if (USE_LOCAL_LP_SOLVE)
  add_subdirectory(lpsolve)
  get_property(LPSOLVE_INCLUDE_DIR_TMP GLOBAL PROPERTY LPSOLVE_INCLUDE_DIR)
  set(LPSOLVE_INCLUDE_DIR "${LPSOLVE_INCLUDE_DIR_TMP}/.."
      CACHE PATH "Include directories of lpsolve")
  get_property(LPSOLVE_LIBRARY_TMP GLOBAL PROPERTY LPSOLVE_LIBRARY)
  set(LPSOLVE_LIBRARY "${LPSOLVE_LIBRARY_TMP}" CACHE PATH "lpsolve library path")
else (USE_LOCAL_LP_SOLVE)
  FIND_PACKAGE(LpSolve REQUIRED)
endif (USE_LOCAL_LP_SOLVE)
