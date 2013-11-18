add_subdirectory(abc)
add_subdirectory(luabind)

option(USE_LOCAL_LP_SOLVE "Use the local version of lp_solve" OFF)
if (USE_LOCAL_LP_SOLVE)
  add_subdirectory(lp_solve)
endif (USE_LOCAL_LP_SOLVE)
