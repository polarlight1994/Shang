set(ENV{all_benchmark} "")

macro(add_benchmark_stats file_name)
	add_shang_test(test_ll "${file_name}.cpp" ${file_name})
  set(ENV{all_benchmark} "$ENV{all_benchmark} ${test_ll}")
  add_test_config("${file_name}_hardware_sim" ${test_ll} ${file_name}_ir pure_hw_sim)
  add_test_config("${file_name}_altera_syn" ${test_ll} ${file_name}_ir altera_syn)
endmacro(add_benchmark_stats)

add_subdirectory(DSPStone)
add_subdirectory(CHStone)
add_subdirectory(DFGs)
add_subdirectory(legup_chstone)

# Run all trivial tests.
add_test_config(benchmark_simulation $ENV{all_benchmark} test_ir pure_hw_sim)
add_test_config(benchmark_altera_synthesis $ENV{all_benchmark} test_ir altera_syn)
