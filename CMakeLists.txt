# If we are not building as a part of LLVM, build lld as a standalone project,
# using LLVM as an external library.

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(shang)
  cmake_minimum_required(VERSION 2.8)

  set(SHANG_PATH_TO_LLVM_SOURCE "" CACHE PATH
    "Path to LLVM source code. Not necessary if using an installed LLVM.")
  set(SHANG_PATH_TO_LLVM_BUILD "" CACHE PATH
    "Path to the directory where LLVM was built or installed.")

  if (SHANG_PATH_TO_LLVM_SOURCE)
    if (NOT EXISTS "${SHANG_PATH_TO_LLVM_SOURCE}/cmake/config-ix.cmake")
      message(FATAL_ERROR "Please set SHANG_PATH_TO_LLVM_SOURCE to the root "
              "directory of LLVM source code.")
    else()
      get_filename_component(LLVM_MAIN_SRC_DIR ${SHANG_PATH_TO_LLVM_SOURCE}
                             ABSOLUTE)
      list(APPEND CMAKE_MODULE_PATH "${LLVM_MAIN_SRC_DIR}/cmake/modules")
    endif()
  endif()

  list(APPEND CMAKE_MODULE_PATH "${SHANG_PATH_TO_LLVM_BUILD}/share/llvm/cmake")

  get_filename_component(PATH_TO_LLVM_BUILD ${SHANG_PATH_TO_LLVM_BUILD}
                         ABSOLUTE)

  include(AddLLVM)
  include(TableGen)
  include("${SHANG_PATH_TO_LLVM_BUILD}/share/llvm/cmake/LLVMConfig.cmake")
  include(HandleLLVMOptions)

  set(PACKAGE_VERSION "${LLVM_PACKAGE_VERSION}")

  set(LLVM_MAIN_INCLUDE_DIR "${LLVM_MAIN_SRC_DIR}/include")
  set(LLVM_BINARY_DIR ${CMAKE_BINARY_DIR})

  set(CMAKE_INCLUDE_CURRENT_DIR ON)
  include_directories("${PATH_TO_LLVM_BUILD}/include"
                      "${LLVM_MAIN_INCLUDE_DIR}")
  link_directories("${PATH_TO_LLVM_BUILD}/lib")

  if( EXISTS "${SHANG_PATH_TO_LLVM_BUILD}/bin/llvm-tblgen${CMAKE_EXECUTABLE_SUFFIX}" )
    set(LLVM_TABLEGEN_EXE "${PATH_TO_LLVM_BUILD}/bin/llvm-tblgen${CMAKE_EXECUTABLE_SUFFIX}")
  else()
    # FIXME: This is an utter hack.
    set(LLVM_TABLEGEN_EXE "${PATH_TO_LLVM_BUILD}/bin/Debug/llvm-tblgen${CMAKE_EXECUTABLE_SUFFIX}")
  endif()

  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

  set(SHANG_BUILT_STANDALONE 1)
endif()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed. CMake would overwrite "
"the makefiles distributed with LLVM. Please create a directory and run cmake "
"from there, passing the path to this source directory as the last argument. "
"This process created the file `CMakeCache.txt' and the directory "
"`CMakeFiles'. Please delete them.")
endif()

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
# Load the macro to setup the platform variable for ABC
include(SetupABC)

FIND_PACKAGE(BOOST REQUIRED)
FIND_PACKAGE(Lua REQUIRED)
FIND_PACKAGE(LuaBind REQUIRED)
FIND_PACKAGE(LpSolve REQUIRED)

# Add the external libraries before we add the source code of VAST.
add_subdirectory(external_libs)

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
set(LLVM_LINK_COMPONENTS
    irreader bitreader asmparser codegen SelectionDAG transformutils ipo)

add_llvm_tool(shang
  shang.cpp
 )
target_link_libraries(shang ShangIROpts ShangVAST ShangHLS ShangScripting ABCLogicSynthesis)
