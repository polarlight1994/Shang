cmake_minimum_required(VERSION 2.6)

set(SIR_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(SIR_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${SIR_SOURCE_ROOT}/cmake")

FIND_PACKAGE(Verilator REQUIRED)
FIND_PACKAGE(SystemC   REQUIRED)

FIND_PATH(QUARTUS_BIN_DIR quartus_sh)

#Set up the llvm binaries.
set(LLC ${LLVM_TOOLS_BINARY_DIR}/llc)
set(LLI ${LLVM_TOOLS_BINARY_DIR}/lli)
set(CLANG ${LLVM_TOOLS_BINARY_DIR}/clang)
set(LLVM_AS ${LLVM_TOOLS_BINARY_DIR}/llvm-as)
set(LLVM_DIS ${LLVM_TOOLS_BINARY_DIR}/llvm-dis)
set(LLVM_LINK ${LLVM_TOOLS_BINARY_DIR}/llvm-link)
set(SHANG ${LLVM_TOOLS_BINARY_DIR}/shang)

set(FMAX "100.0" CACHE STRING "The maximum frequency of the synthesized designs")
set(TIMEOUT "2400" CACHE STRING "Maximum time allow the simulation to run")

set(BenchmarkSummaryTmp "${SIR_BINARY_ROOT}/benchmark.summary.json.tmp")

#Set up some environment values according to the platform.
if (CMAKE_SIZEOF_VOID_P MATCHES 8)
  set(POINTER_SIZE_IN_BITS 64)
  set(SYSTEMC_LIB_PATH ${SYSTEMC}/lib-linux64)
  set(LLC_MARCH "x86-64")
else (CMAKE_SIZEOF_VOID_P MATCHES 8)
  set(POINTER_SIZE_IN_BITS 32)
  set(SYSTEMC_LIB_PATH ${SYSTEMC}/lib-linux)
  set(LLC_MARCH "x86")
endif(CMAKE_SIZEOF_VOID_P MATCHES 8)

add_custom_target(benchmark_cosimulation
									COMMENT "Run the co-simulation of whole benchmark")

add_custom_target(benchmark_synthesis
									COMMENT "Run the synthesis of whole benchmark")

macro(add_synthesis_test test_name)
	set(TEST "${test_name}")
	set(TEST_RTL_ENTITY		"main")

	set(TEST_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
	set(TEST_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})
	set(TEST_RTL_OBJ_ROOT "${TEST_BINARY_ROOT}/obj_dir")

	set(SIR_MemIntrinsic_BC "${SIR_SOURCE_ROOT}/sir_mem.bc")

	set(TEST_SRC       		"${TEST_SOURCE_ROOT}/${TEST}.cpp")
	set(TEST_BC        		"${TEST_BINARY_ROOT}/${TEST}.bc")
	set(TEST_LL        		"${TEST_BINARY_ROOT}/${TEST}.ll")

	set(TEST_RTL_SRC   		"${TEST_BINARY_ROOT}/${TEST_RTL_ENTITY}.sv")
	set(TEST_SDC     		"${TEST_BINARY_ROOT}/${TEST_RTL_ENTITY}.sdc")

	set(CycleCounter		"${TEST_BINARY_ROOT}/${test_name}_DUT_RTL.txt")

	set(DEBUG_FINAL_IR		"${TEST_BINARY_ROOT}/${TEST}.finalir")
	set(DEBUG_SDC_RESULT	"${TEST_BINARY_ROOT}/${TEST}.sdcresult")
	set(DEBUG_LS_RESULT		"${TEST_BINARY_ROOT}/${TEST}.lsresult")
	set(DEBUG_IMS_RESULT  	"${TEST_BINARY_ROOT}/${TEST}.imsresult")
	set(DEBUG_SchedGraph	"${TEST_BINARY_ROOT}/${TEST}.schedgraph")

	set(enableCoSimulation false)

	configure_file(
		"${SIR_SOURCE_ROOT}/common_config.lua.in"
		"${TEST_BINARY_ROOT}/common_config.lua"
	)
	configure_file(
		"${SIR_SOURCE_ROOT}/test_config_main.lua.in"
		"${TEST_BINARY_ROOT}/test_config_main.lua"
	)
	configure_file(
		"${SIR_SOURCE_ROOT}/EP2C70F896C6.lua"
		"${TEST_BINARY_ROOT}/EP2C70F896C6.lua"
	)
	configure_file(
		"${SIR_SOURCE_ROOT}/verilog_synthesis_altera.tcl.in"
		"${TEST_BINARY_ROOT}/${TEST}_verilog_synthesis_altera.tcl"
	)

	#Compile the source file to IR.
  add_custom_command(OUTPUT ${TEST_BC} ${TEST_LL}
		#Use Clang to compile to BC.
    COMMAND ${CLANG} ${TEST_SRC}
            -O0 -c -emit-llvm
            -o ${TEST_BC}
		#Use LLVMDIS to compile to IR.
    COMMAND ${LLVM_DIS} ${TEST_BC} -o ${TEST_LL}
    DEPENDS ${TEST_SRC} ${LLVM_LINK} ${CLANG} ${LLVM_DIS}
    WORKING_DIRECTORY ${TEST_BINARY_ROOT}
    COMMENT "Compiling ${TEST_SRC} to ${TEST_BC}"
  )

	#Run the Shang.exe to compile cpp into verilog and software part.
	add_custom_command(OUTPUT ${TEST_RTL_SRC} ${TEST_SDC}
		COMMAND timeout ${TIMEOUT}s sh -c "${SHANG} ${TEST_BINARY_ROOT}/test_config_main.lua"
		DEPENDS ${TMIEOUT} ${TEST_BC} ${SHANG} "${TEST_BINARY_ROOT}/test_config_main.lua"
		WORKING_DIRECTORY ${TEST_BINARY_ROOT}
		COMMENT "Run ${SHANG} to get verilog and software part"
	)

	#Run the quartus to compile the verilog and get report.
	add_custom_command(OUTPUT ${BenchmarkSummaryTmp}
		COMMAND ${QUARTUS_BIN_DIR}/quartus_sh -t "${TEST_BINARY_ROOT}/${TEST}_verilog_synthesis_altera.tcl" > /dev/null
		DEPENDS ${TEST_RTL_SRC} ${TEST_SDC} "${TEST_BINARY_ROOT}/${TEST}_verilog_synthesis_altera.tcl" ${CycleCounter}
		WORKING_DIRECTORY ${TEST_BINARY_ROOT}
		COMMENT "Compile the verilog and get report"
	)
	add_custom_target(${TEST}_synthesis	DEPENDS ${BenchmarkSummaryTmp})

endmacro(add_synthesis_test)

macro(add_cosimulation_test test_name)
	set(TEST "${test_name}")
	set(DUT_NAME "${TEST}_DUT")
	set(TEST_RTL_ENTITY		"${DUT_NAME}_RTL")

	set(TEST_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
	set(TEST_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})
	set(TEST_RTL_OBJ_ROOT "${TEST_BINARY_ROOT}/obj_dir")

	set(SIR_MemIntrinsic_BC "${SIR_SOURCE_ROOT}/sir_mem.bc")
	
	set(TEST_SRC       		"${TEST_SOURCE_ROOT}/${TEST}.cpp")
	set(TEST_BC        		"${TEST_BINARY_ROOT}/${TEST}.bc")
	set(TEST_LL        		"${TEST_BINARY_ROOT}/${TEST}.ll")
	
	set(TEST_RTL_SRC   		"${TEST_BINARY_ROOT}/${TEST_RTL_ENTITY}.sv")
	set(TEST_SDC     		"${TEST_BINARY_ROOT}/${TEST_RTL_ENTITY}.sdc")
	set(TEST_SW_LL				"${TEST_BINARY_ROOT}/${DUT_NAME}_SW.ll")

	set(TEST_IF_SRC				"${TEST_BINARY_ROOT}/${DUT_NAME}_IF.cpp")

	set(TEST_SC_O					"${TEST_BINARY_ROOT}/${DUT_NAME}_IF.o")
	set(TEST_SW_O					"${TEST_BINARY_ROOT}/${TEST}_SW.o")

	set(TEST_RTL_ALL_O		"${TEST_RTL_OBJ_ROOT}/V${DUT_NAME}_RTL__ALL*.o")
	set(TEST_VERILATED_O	"${TEST_RTL_OBJ_ROOT}/verilated.o")

	set(TEST_SC_EXE				"${TEST_RTL_OBJ_ROOT}/V${DUT_NAME}_RTL")

	set(CycleCounter			"${TEST_BINARY_ROOT}/${TEST_RTL_ENTITY}.txt")

	set(DEBUG_FINAL_IR		"${TEST_BINARY_ROOT}/${TEST}.finalir")
	set(DEBUG_SDC_RESULT	"${TEST_BINARY_ROOT}/${TEST}.sdcresult")
	set(DEBUG_LS_RESULT		"${TEST_BINARY_ROOT}/${TEST}.lsresult")
	set(DEBUG_IMS_RESULT  "${TEST_BINARY_ROOT}/${TEST}.imsresult")
	set(DEBUG_SchedGraph	"${TEST_BINARY_ROOT}/${TEST}.schedgraph")

	set(enableCoSimulation true)

	configure_file(
		"${SIR_SOURCE_ROOT}/common_config.lua.in"
		"${TEST_BINARY_ROOT}/common_config.lua"
	)
	configure_file(
		"${SIR_SOURCE_ROOT}/test_config.lua.in"
		"${TEST_BINARY_ROOT}/test_config.lua"
	)
	configure_file(
		"${SIR_SOURCE_ROOT}/EP2C70F896C6.lua"
		"${TEST_BINARY_ROOT}/EP2C70F896C6.lua"
	)

	#Compile the source file to IR.
  add_custom_command(OUTPUT ${TEST_BC} ${TEST_LL}
		#Use Clang to compile to BC.
    COMMAND ${CLANG} ${TEST_SRC}
            -O0 -c -emit-llvm
            -o ${TEST_BC}
		#Use LLVMDIS to compile to IR.
    COMMAND ${LLVM_DIS} ${TEST_BC} -o ${TEST_LL}
    DEPENDS ${TEST_SRC} ${LLVM_LINK} ${CLANG} ${LLVM_DIS}
    WORKING_DIRECTORY ${TEST_BINARY_ROOT}
    COMMENT "Compiling ${TEST_SRC} to ${TEST_BC}"
  )

	#Run the IR to get expected output.
	add_custom_command(OUTPUT ${TEST_BINARY_ROOT}/Expected.output
		COMMAND ${LLI} ${TEST_BC} > "${TEST_BINARY_ROOT}/Expected.output"
		DEPENDS ${LLI} ${TEST_BC}
		WORKING_DIRECTORY ${TEST_BINARY_ROOT}
		COMMENT "Run ${TEST_BC} to get expected output"
	)
	add_custom_target(${TEST}_expected_output DEPENDS "${TEST_BINARY_ROOT}/Expected.output")

	#Run the Shang.exe to compile cpp into verilog and software part.
	add_custom_command(OUTPUT ${TEST_RTL_SRC} ${TEST_SDC} ${TEST_SW_LL}
		COMMAND timeout ${TIMEOUT}s sh -c "${SHANG} ${TEST_BINARY_ROOT}/test_config.lua"
		DEPENDS ${TMIEOUT} ${TEST_BC} ${SHANG} "${TEST_BINARY_ROOT}/test_config.lua"
		WORKING_DIRECTORY ${TEST_BINARY_ROOT}
		COMMENT "Run ${SHANG} to get verilog and software part"
	)

	#Use the LLC to compile the software part into obj file.
	add_custom_command(OUTPUT ${TEST_SW_O}
		COMMAND ${LLC}
						-march=${LLC_MARCH} ${TEST_SW_LL}
						-filetype=obj
						-mc-relax-all
						-o
						${TEST_SW_O}
		DEPENDS ${LLC} ${TEST_SW_LL}
		WORKING_DIRECTORY ${TEST_BINARY_ROOT}
		COMMENT "Compile the software part into obj file"
	)

	#Use the SystemC to compile the verilog into obj file.
	add_custom_command(OUTPUT "${TEST_RTL_OBJ_ROOT}/V${DUT_NAME}_RTL.mk"
		COMMAND rm -rf "${TEST_BINARY_ROOT}/obj_dir/*"
		COMMAND ${VERILATOR_EXECUTABLE} ${TEST_RTL_SRC} -Wall --sc -D__DEBUG_IF +define+__VERILATOR_SIM -Wno-fatal --top-module ${TEST_RTL_ENTITY}
		DEPENDS ${VERILATOR_EXECUTABLE} ${TEST_RTL_SRC}
		WORKING_DIRECTORY ${TEST_BINARY_ROOT}
		COMMENT "Compile the verilog into obj file"
	)

	#Link and compile the obj file and software part into exe file.
	add_custom_command(OUTPUT ${TEST_SC_EXE}
		COMMAND make -C "${TEST_BINARY_ROOT}/obj_dir/" -j -f "V${DUT_NAME}_RTL.mk" "V${DUT_NAME}_RTL__ALL.a"
		COMMAND make -C "${TEST_BINARY_ROOT}/obj_dir/" -j -f "V${DUT_NAME}_RTL.mk" ${TEST_SC_O} "verilated.o"
		COMMAND g++ -L${SYSTEMC_LIB_PATH}
						${TEST_SC_O}
						${TEST_SW_O}
						${TEST_RTL_ALL_O}
						${TEST_VERILATED_O}
						-o
						${TEST_SC_EXE}
						-lsystemc
		DEPENDS ${TEST_SW_O} ${TEST_RTL_O} "${TEST_RTL_OBJ_ROOT}/V${DUT_NAME}_RTL.mk"
		WORKING_DIRECTORY ${TEST_RTL_OBJ_ROOT}
		COMMENT "Link and compile to build the exe"
	)

	#Run the exe to get test output.
	add_custom_command(OUTPUT ${TEST_BINARY_ROOT}/Test.output ${CycleCounter}
		COMMAND	rm -rf "${TEST_BINARY_ROOT}/Test.output"
		COMMAND "${TEST_SC_EXE}" > "${TEST_BINARY_ROOT}/Test.output"
		DEPENDS ${TEST_SC_EXE}
		WORKING_DIRECTORY ${TEST_RTL_OBJ_ROOT}
		COMMENT "Run ${TEST_SC_EXE} to get test output"
	)
	add_custom_target(${TEST}_test_output DEPENDS "${TEST_BINARY_ROOT}/Test.output" ${CycleCounter})

	#Compare the output.
	add_custom_target(${TEST}_cosimulation
		COMMAND [ -f "${TEST_BINARY_ROOT}/Test.output" ] && diff "${TEST_BINARY_ROOT}/Expected.output" "${TEST_BINARY_ROOT}/Test.output"
		DEPENDS "${TEST_BINARY_ROOT}/Test.output" "${TEST_BINARY_ROOT}/Expected.output"
		WORKING_DIRECTORY ${TEST_BINARY_ROOT}
		COMMENT "Compare the output"
	)
	add_dependencies(${TEST}_cosimulation ${TEST}_test_output ${TEST}_expected_output)

	#Do not clean these file.
	set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES
				${MAIN_RTL_SRC}
        ${CycleCounter}
				${TEST_BINARY_ROOT}/Expected.output ${TEST_BINARY_ROOT}/Test.output
				${TEST_BINARY_ROOT}/obj_dir/V${DUT_NAME}_RTL ${TEST}_origin)
endmacro(add_cosimulation_test)

add_subdirectory(benchmark)