cmake_minimum_required(VERSION 2.6)

set(FU_EST_SOUCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(FU_EST_BINARY_ROOT ${CMAKE_CURRENT_BINARY_DIR})

add_custom_command(OUTPUT ${FU_EST_BINARY_ROOT}/data.sql
	COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/micro_benchmarking.py
	WORKING_DIRECTORY ${FU_EST_BINARY_ROOT}
	COMMENT "Runing micro benchmarks"
	)
add_custom_target(micro_benchmarking DEPENDS ${FU_EST_BINARY_ROOT}/data.sql)
