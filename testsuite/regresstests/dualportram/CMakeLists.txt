macro(add_simpledualport_regress_test file_name)
  add_regress_test(${file_name})

  #Override the configuration of the design, use simple dual-port mode.
  set(SIMPLEDUALPORT_TEST_NAME "${file_name}_${PipelineType}_${ScheduleType}")
  set(SIMPLEDUALPORT_TEST_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/${SIMPLEDUALPORT_TEST_NAME}_main)
  set(SIMPLEDUALPORT_CONFIG_LUA "${SIMPLEDUALPORT_TEST_BINARY_ROOT}/${SIMPLEDUALPORT_TEST_NAME}_config_main.lua")
  file(APPEND ${SIMPLEDUALPORT_CONFIG_LUA} "FUs.BRam.Mode = 1\n")
endmacro(add_simpledualport_regress_test)

add_simpledualport_regress_test(simpledualport)
