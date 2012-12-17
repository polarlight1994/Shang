add_custom_target(regress_tests COMMENT "Run the regress tests")

macro(add_regress_test file_name)
  set(TEST_NAME "${test_file}_${PipelineType}_${ScheduleType}")
  add_general_main_test(${file_name}
          "${VTS_SOURCE_ROOT}/test_config_main.lua.in"
          "${VTS_SOURCE_ROOT}/setup_proj_symain.tcl.in"
          "cpp")
  
  add_dependencies(regress_tests ${TEST}_synthesis_main)
endmacro(add_regress_test)

add_subdirectory(delayestimation)
add_subdirectory(memorybus)
