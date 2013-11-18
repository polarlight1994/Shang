set(LLVM_REQUIRES_RTTI 1)
set(LLVM_REQUIRES_EH 1)

file(GLOB_RECURSE ALL_LUABIND_SOURCES globbing "src/*.cpp")
#MESSAGE(STATUS "Found ABC: ${ALL_ABC_SOURCES}")

add_llvm_library(Luabind ${ALL_LUABIND_SOURCES})

#add_definitions( -DLUABIND_NO_RTTI )
#LUABIND_NO_EXCEPTIONS

set(LUABIND_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE PATH "Include directories of ABC")
set(LUABIND_LIBRARY "Luabind"
    CACHE PATH "Include directories of ABC")

include_directories(
  ${BOOST_INCLUDE_DIR}
  ${LUA_INCLUDE_DIR}
  ${LUABIND_INCLUDE_DIR}
  ${LUABIND_INCLUDE_DIR}/luabind
)
