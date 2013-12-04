set(LLVM_REQUIRES_RTTI 1)
set(LLVM_REQUIRES_EH 1)

get_property(LUABIND_INCLUDE_DIR GLOBAL PROPERTY LUABIND_INCLUDE_DIR)
get_property(LUABIND_LIBRARY GLOBAL PROPERTY LUABIND_LIBRARY)
get_property(BOOST_INCLUDE_DIR GLOBAL PROPERTY BOOST_INCLUDE_DIR)

include_directories(
  ${BOOST_INCLUDE_DIR}
  ${LUA_INCLUDE_DIR}
  ${LUABIND_INCLUDE_DIR}
  ${LUABIND_INCLUDE_DIR}/luabind
)

add_llvm_library(ShangScripting
  FUInfo.cpp
  LuaScript.cpp
)

target_link_libraries (
  ShangScripting
  ${LUA_LIBRARY}
  ${LUABIND_LIBRARY}
)

add_dependencies(ShangScripting luappLua_gen)
