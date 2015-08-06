get_property(LUA_BRIDGE_INCLUDE_DIR GLOBAL PROPERTY LUA_BRIDGE_INCLUDE_DIR)

include_directories(
  ${LUA_INCLUDE_DIR}
  ${LUA_BRIDGE_INCLUDE_DIR}
)

add_llvm_library(ShangScripting
  FUInfo.cpp
  LuaI.cpp
	luapp.inc
)

target_link_libraries (
  ShangScripting
  ${LUA_LIBRARY}
)