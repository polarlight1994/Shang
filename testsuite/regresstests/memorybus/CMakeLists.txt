macro(add_membus_regress_test file_name)
  add_regress_test(${file_name})

  #Override the configuration of the design, use default mode (dual-port mode disabled).
  set(SIMPLEDUALPORT_TEST_NAME "${file_name}_${PipelineType}_${ScheduleType}")
  set(SIMPLEDUALPORT_TEST_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR}/${SIMPLEDUALPORT_TEST_NAME}_main)
  set(SIMPLEDUALPORT_CONFIG_LUA "${SIMPLEDUALPORT_TEST_BINARY_ROOT}/${SIMPLEDUALPORT_TEST_NAME}_config_main.lua")
  file(APPEND ${SIMPLEDUALPORT_CONFIG_LUA} "FUs.BRam.Mode = 0\n")
endmacro(add_membus_regress_test)

add_membus_regress_test(membus_read)
