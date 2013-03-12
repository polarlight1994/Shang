

add_custom_target(legup_simulation
          COMMENT "Compiling all the main test obj file into exe file")

add_custom_target(legup_synthesis
          COMMENT "Compiling all the main test obj file into exe file")

add_custom_target(legup_hls
          COMMENT "Compiling all the main test obj file into exe file")

macro(add_general_legup_test benchmark bc_file config_file altera_setup_prj_file)
  set(TEST_NAME "${benchmark}")
  set(TEST     "${TEST_NAME}")
  set(DUT_NAME "${TEST_NAME}_main_DUT")

  set(TEST_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
  set(TEST_BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME}_main")
  set(MAIN_OBJ_PRJ_ROOT	"${TEST_BINARY_ROOT}/obj_dir")

  set(MAIN_ORIG_BC        "${TEST_SOURCE_ROOT}/${bc_file}")

  set(MAIN_RTL_ENTITY     "${DUT_NAME}_RTL")
  set(MAIN_RTL_SRC        "${TEST_BINARY_ROOT}/${MAIN_RTL_ENTITY}.sv")
  set(MAIN_SDC_SRC        "${TEST_BINARY_ROOT}/${MAIN_RTL_ENTITY}.sdc")
  set(MAIN_DELAY_VERIFY_SRC "${TEST_BINARY_ROOT}/${MAIN_RTL_ENTITY}_delay_verify.tcl")
  set(MAIN_INT_TOP        "${TEST_BINARY_ROOT}/INTF_${MAIN_RTL_ENTITY}.v")
  set(MAIN_MODELDO_FILE   "${TEST_BINARY_ROOT}/modeldofile")
  set(MAIN_TB_FILE        "${TEST_BINARY_ROOT}/DUT_TOP_tb.sv")
  set(MAIN_RTL_STA        "${TEST_BINARY_ROOT}/${MAIN_RTL_ENTITY}.sta.rpt")
  set(CycleCounter        "${TEST_BINARY_ROOT}/${MAIN_RTL_ENTITY}.txt")

  configure_file (
    "${VTS_SOURCE_ROOT}/common_config.lua.in"
    "${TEST_BINARY_ROOT}/common_config.lua"
  )
  configure_file (
    "${config_file}"
    "${TEST_BINARY_ROOT}/${TEST}_config_main.lua"
    )

  add_custom_command(OUTPUT ${MAIN_RTL_SRC} ${MAIN_SDC_SRC} ${MAIN_UCF_SRC} ${MAIN_INT_TOP} ${MAIN_INT_BRAM} 
                            ${MAIN_INT_BRAMINIT} ${MAIN_TB_FILE} ${MAIN_MODELDO_FILE}
    COMMAND echo "Bad RTL source!" > ${MAIN_RTL_SRC}
    COMMAND ${SHANG} ${TEST_BINARY_ROOT}/${TEST}_config_main.lua  -timing-model=bit-level -vast-disable-mux-slack  -shang-enable-mux-pipelining=false ${EXTRA_HLS_OPTION}
    DEPENDS ${MAIN_ORIG_BC} ${SHANG} "${TEST_BINARY_ROOT}/${TEST}_config_main.lua"
    WORKING_DIRECTORY ${TEST_BINARY_ROOT}
    COMMENT "High-levle Synthesising RTL module and interface for ${benchmark}"
  )
  add_custom_target(${TEST}_main_hls DEPENDS ${MAIN_RTL_SRC})
  add_dependencies(legup_hls ${TEST}_main_hls)

  configure_file (
    "${altera_setup_prj_file}" "${TEST_BINARY_ROOT}/${TEST}_setup_prj_symain.tcl"
  )

  add_custom_command(OUTPUT ${CycleCounter}
    COMMAND timeout ${TIMEOUTS}s sh "${MAIN_MODELDO_FILE}"
    DEPENDS ${MAIN_RTL_SRC} ${MAIN_INT_TOP} ${MAIN_INT_BRAM} ${MAIN_INT_BRAMINIT}
    WORKING_DIRECTORY ${TEST_BINARY_ROOT}
    COMMENT "Simulating RTL module and interface for ${benchmark}"
  )
  add_custom_target(${TEST}_simulate_main DEPENDS ${CycleCounter})
  add_dependencies(legup_simulation ${TEST}_simulate_main)

  add_custom_command(OUTPUT ${MAIN_RTL_STA}
    COMMAND qsub -cwd -j y -n "${benchmark}" ${QUARTUS_BIN_DIR}/quartus_sh --64bit -t ${TEST_BINARY_ROOT}/${TEST}_setup_prj_symain.tcl
    DEPENDS ${MAIN_RTL_SRC} "${TEST_BINARY_ROOT}/${TEST}_setup_prj_symain.tcl" ${MAIN_SDC_SRC} ${MAIN_INT_TOP} ${MAIN_INT_BRAM} ${MAIN_INT_BRAMINIT}
    WORKING_DIRECTORY ${TEST_BINARY_ROOT}
    COMMENT "Synthesising RTL module and interface for ${benchmark}"
  )
  add_custom_target(${TEST}_synthesis_main DEPENDS ${MAIN_RTL_STA})
  add_dependencies(legup_synthesis ${TEST}_synthesis_main)

  
endmacro(add_general_legup_test)

macro(add_test_legup_cases_impl benchmark bc_file family_name device period)
set(FAMILY_NAME ${family_name})
set(DEVICE ${device})
set(Period ${period})

add_general_legup_test("${benchmark}${period}${device}"
        ${bc_file}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_legup.lua.in"
        "${CMAKE_CURRENT_SOURCE_DIR}/setup_legup_symain.tcl.in"
        )
endmacro(add_test_legup_cases_impl)

macro(add_test_legup_cases_period benchmark bc_file period)
add_test_legup_cases_impl(${benchmark} ${bc_file} "Cyclone II" "EP2C70F896C6" ${period})
add_test_legup_cases_impl(${benchmark} ${bc_file} "Cyclone IV E" "EP4CE75F29C6" ${period})
endmacro(add_test_legup_cases_period)

macro(add_test_legup_cases benchmark bc_file)
  foreach(period RANGE 5 20 [1])
    add_test_legup_cases_period(${benchmark} ${bc_file} ${period})
  endforeach(loop_input_num)
endmacro(add_test_legup_cases)

add_test_legup_cases(dfadd dfadd.bc)
add_test_legup_cases(dfmul dfmul.bc)
add_test_legup_cases(dfdiv dfdiv.bc)
add_test_legup_cases(dfsin dfsin.bc)

add_test_legup_cases(gsm gsm.bc)

add_test_legup_cases(mpeg2 mpeg2.bc)

add_test_legup_cases(sha sha_driver.bc)
add_test_legup_cases(aes aes.bc)

#Deming's benchmark
add_test_legup_cases(pr pr.bc)
add_test_legup_cases(wang wang.bc)
add_test_legup_cases(chem chem.bc)
add_test_legup_cases(arai arai.bc)
add_test_legup_cases(feig_dct feig_dct.bc)
add_test_legup_cases(u5ml u5ml.bc)
add_test_legup_cases(mcm mcm.bc)
add_test_legup_cases(lee lee.bc)
add_test_legup_cases(aircraft aircraft.bc)
add_test_legup_cases(dir dir.bc)

#These there are very large design
add_test_legup_cases(adpcm adpcm.bc)

add_test_legup_cases(jpeg main.bc)
add_test_legup_cases(blowfish bf.bc)

add_test_legup_cases(mips mips.bc)
