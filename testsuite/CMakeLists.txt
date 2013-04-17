cmake_minimum_required(VERSION 2.6)

set(VTS_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(VTS_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${VTS_SOURCE_ROOT}/cmake")

FIND_PACKAGE(Verilator  REQUIRED)
FIND_PACKAGE(SystemC    REQUIRED)

if (CMAKE_SIZEOF_VOID_P MATCHES 8)
  set(POINTER_SIZE_IN_BITS 64)
  set(SYSTEMC_LIB_PATH ${SYSTEMC}/lib-linux64)
  set(LLC_MARCH "x86-64")
else (CMAKE_SIZEOF_VOID_P MATCHES 8)
  set(POINTER_SIZE_IN_BITS 32)
  set(SYSTEMC_LIB_PATH ${SYSTEMC}/lib-linux)
  set(LLC_MARCH "x86")
endif(CMAKE_SIZEOF_VOID_P MATCHES 8)

#Set up the llvm binaries.
set(LLC ${LLVM_TOOLS_BINARY_DIR}/llc)
set(LLI ${LLVM_TOOLS_BINARY_DIR}/lli)
set(CLANG ${LLVM_TOOLS_BINARY_DIR}/clang)
set(LLVM_AS ${LLVM_TOOLS_BINARY_DIR}/llvm-as)
set(LLVM_DIS ${LLVM_TOOLS_BINARY_DIR}/llvm-dis)
set(LLVM_LINK ${LLVM_TOOLS_BINARY_DIR}/llvm-link)
#FIXME: Support the standalone build.
set(SHANG ${LLVM_TOOLS_BINARY_DIR}/shang)
set(SIT ${SHANG_SOURCE_DIR}/util/sit/sit.py)

add_custom_target(test_ir COMMENT "Compiling all source to llvm ir")

set(ENV{all_trivial_tests} "")

configure_file(
  ${VTS_SOURCE_ROOT}/basic_config.json.in
  ${VTS_BINARY_ROOT}/basic_config.json
)

macro(add_test_config name test_lls depends mode)
  add_custom_target(${name}
                    COMMAND ${SIT}
                            --mode=${mode}
                            --tests=${test_lls}
                            --config_bin_dir=${VTS_BINARY_ROOT}
                            --sge_queue=\${QUEUE}
                    DEPENDS ${depends} ${SHANG}
                    COMMENT "Run trivial tests")
endmacro(add_test_config)

macro(add_shang_test test_ll test_file test_name)
  set(TEST "${test_name}")
  set(DUT_NAME "${TEST}_DUT")

  set(TEST_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
  set(TEST_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})

  set(TEST_SRC       "${TEST_SOURCE_ROOT}/${test_file}")
  set(TEST_BC        "${TEST_BINARY_ROOT}/${TEST}.bc")
  set(TEST_LL        "${TEST_BINARY_ROOT}/${TEST}.ll")
  set(test_ll        "${TEST_LL}")

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
	add_shang_test(test_ll "${file_name}.cpp" ${file_name})
  set(ENV{all_trivial_tests} "$ENV{all_trivial_tests} ${test_ll}")
  add_test_config("${file_name}_trivial_test" ${test_ll} ${file_name}_ir hybrid_sim)
endmacro(add_test_cases)

add_subdirectory(SimpleTest)
add_subdirectory(Loops)
add_subdirectory(benchmark)
add_subdirectory(regresstests)

# Run all trivial tests.
add_test_config(trivial_tests $ENV{all_trivial_tests} test_ir hybrid_sim)
