set_property(GLOBAL PROPERTY BOOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boost")

add_subdirectory(abc)
add_subdirectory(luabind)

option(USE_LOCAL_LP_SOLVE "Use the local version of lp_solve" OFF)
#if (USE_LOCAL_LP_SOLVE)
add_subdirectory(lpsolve)
#endif (USE_LOCAL_LP_SOLVE)
