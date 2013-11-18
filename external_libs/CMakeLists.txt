set_property(GLOBAL PROPERTY BOOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boost")

add_subdirectory(abc)
add_subdirectory(luabind)

option(USE_LOCAL_LP_SOLVE "Use the local version of lp_solve" OFF)
if (USE_LOCAL_LP_SOLVE)
  add_subdirectory(lpsolve)
  #get_property(LPSOLVE_LIBRARY GLOBAL PROPERTY LPSOLVE_LIBRARY)
  #get_property(LPSOLVE_INCLUDE_DIR GLOBAL PROPERTY LPSOLVE_INCLUDE_DIR)
else (USE_LOCAL_LP_SOLVE)
  FIND_PACKAGE(LpSolve REQUIRED)
endif (USE_LOCAL_LP_SOLVE)
