cmake_minimum_required(VERSION 2.6)

set(VTS_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(VTS_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${VTS_SOURCE_ROOT}/cmake")

FIND_PACKAGE(Verilator  REQUIRED)
FIND_PACKAGE(SystemC    REQUIRED)

#Set up the llvm binaries.
set(LLC ${LLVM_TOOLS_BINARY_DIR}/llc)
set(LLI ${LLVM_TOOLS_BINARY_DIR}/lli)
set(CLANG ${LLVM_TOOLS_BINARY_DIR}/clang)
set(LLVM_AS ${LLVM_TOOLS_BINARY_DIR}/llvm-as)
set(LLVM_DIS ${LLVM_TOOLS_BINARY_DIR}/llvm-dis)
set(LLVM_LINK ${LLVM_TOOLS_BINARY_DIR}/llvm-link)
#FIXME: Support the standalone build.
set(SHANG ${LLVM_TOOLS_BINARY_DIR}/shang)

add_custom_target(test_ir COMMENT "Compiling all source to llvm ir")

macro(add_shang_test test_file test_name)
  set(TEST_NAME "${test_name}")
  set(DUT_NAME "${TEST_NAME}_DUT")

  set(TEST_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
  set(TEST_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})

  set(TEST_SRC       "${TEST_SOURCE_ROOT}/${test_file}")
  set(TEST_BC        "${TEST_BINARY_ROOT}/${TEST}.bc")
  set(TEST_LL        "${TEST_BINARY_ROOT}/${TEST}.ll")

  #Compile the source file to IR.
  add_custom_command(OUTPUT ${TEST_LL}
    COMMAND ${CLANG} ${TEST_SRC}
            -O0 -c -emit-llvm
            -o ${TEST_BC}
    COMMAND ${LLVM_LINK} ${TEST_BC}
            ${VTS_SOURCE_ROOT}/liblegup.bc
            -o=${TEST_BC}
    COMMAND ${LLVM_DIS} ${TEST_BC} -o ${TEST_LL}
    DEPENDS ${TEST_SRC} ${LLVM_LINK} ${CLANG} ${LLVM_DIS}
    WORKING_DIRECTORY ${TEST_BINARY_ROOT}
    COMMENT "Compiling ${TEST_SRC} to ${TEST_BC}"
  )
  add_custom_target(${TEST}_ir DEPENDS ${TEST_LL})
  add_dependencies(test_ir ${TEST}_ir)
endmacro(add_shang_test)

macro(add_test_cases file_name)
	add_shang_test("${file_name}.cpp" ${file_name})
endmacro(add_test_cases)

add_subdirectory(SimpleTest)
add_subdirectory(Loops)
add_subdirectory(benchmark)
add_subdirectory(regresstests)
