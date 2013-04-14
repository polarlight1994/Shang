macro(add_benchmark_stats file_name)
	add_shang_test(test_ll "${file_name}.cpp" ${file_name})
  add_test_config("${file_name}_hardware_sim" ${test_ll} ${file_name}_ir pure_hw_sim)
endmacro(add_benchmark_stats)

add_subdirectory(DSPStone)
add_subdirectory(CHStone)
add_subdirectory(DFGs)
add_subdirectory(legup_chstone)
