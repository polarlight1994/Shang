set_property(GLOBAL PROPERTY BOOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boost")

add_subdirectory(abc)
add_subdirectory(luabind)

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
