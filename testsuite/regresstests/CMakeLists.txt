add_custom_target(regress_tests COMMENT "Run the regress tests")

macro(add_regress_test file_name)
	add_shang_test(test_ll "${file_name}.cpp" ${file_name})
endmacro(add_regress_test)

add_subdirectory(delayestimation)
add_subdirectory(memorybus)
add_subdirectory(dualportram)
