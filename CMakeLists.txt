#Modifid from clang cmakelist.txt

set(SHANG_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SHANG_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

if(MSVC) # Work around for the fucking msvc bug on rtti.
  set(LLVM_REQUIRES_RTTI 1)
endif (MSVC)
# Add appropriate flags for GCC
# if (CMAKE_COMPILER_IS_GNUCXX)
  # FIXME: Turn off exceptions, RTTI:
  # -fno-exceptions -fno-rtti
  # set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-common -Woverloaded-virtual -Wno-long-long -Wall -W -Wno-unused-parameter -Wwrite-strings")
# endif ()

#Add path for custom modules
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${SHANG_SOURCE_DIR}/cmake")

FIND_PACKAGE(BOOST REQUIRED)
FIND_PACKAGE(Lua REQUIRED)
FIND_PACKAGE(LuaBind REQUIRED)
FIND_PACKAGE(LpSolve REQUIRED)

option(ENABLE_LOGIC_SYNTHESIS
       "Enable the Pre-schedule bitlevel optimization by ABC" OFF)
if (ENABLE_LOGIC_SYNTHESIS)
  FIND_PACKAGE(ABC REQUIRED)
endif(ENABLE_LOGIC_SYNTHESIS)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/include
  )

install(DIRECTORY include
  DESTINATION .
  PATTERN ".svn" EXCLUDE
  )

add_definitions( -D_GNU_SOURCE )

add_subdirectory(include)
add_subdirectory(lib)
option(ENABLE_TESTSUITE "Run the testsuite" ON)
if (ENABLE_TESTSUITE)
  add_subdirectory(testsuite)
endif (ENABLE_TESTSUITE)

IF (NOT MSVC)
  add_subdirectory(util/TestAllDelayandLEs)
ENDIF (NOT MSVC)

# TODO: docs.

# Create the shang HLS executable.
set(LLVM_LINK_COMPONENTS  bitreader asmparser codegen SelectionDAG transformutils ipo)

set(LLVM_REQUIRES_RTTI 1)
set(LLVM_REQUIRES_EH 1)

include_directories(
  ${BOOST_INCLUDE_DIR}
  ${LUA_INCLUDE_DIR}
  ${LUABIND_INCLUDE_DIR}
  ${LUABIND_INCLUDE_DIR}/luabind
)

add_llvm_tool(shang
  shang.cpp
 )

target_link_libraries(shang ShangHighLevelOpt ShangVAST ShangHLS ShangScripting)