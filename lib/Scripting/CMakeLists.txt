get_property(LUA_BRIDGE_INCLUDE_DIR GLOBAL PROPERTY LUA_BRIDGE_INCLUDE_DIR)

include_directories(
  ${LUA_INCLUDE_DIR}
  ${LUA_BRIDGE_INCLUDE_DIR}
)

add_llvm_library(ShangScripting
  FUInfo.cpp
  LuaI.cpp
)

macro(luamodule luasrc)
  add_custom_target(${luasrc}Lua_gen
                    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${luasrc}.inc)
endmacro(luamodule)

luamodule(luapp)

target_link_libraries (
  ShangScripting
  ${LUA_LIBRARY}
)

add_dependencies(ShangScripting luappLua_gen)
