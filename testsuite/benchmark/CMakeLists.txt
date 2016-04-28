macro(add_benchmark_cosimulation_test test_name)
set(TEST "${test_name}")
add_cosimulation_test(${TEST})
add_dependencies(benchmark_cosimulation ${TEST}_cosimulation)

endmacro(add_benchmark_cosimulation_test)

macro(add_benchmark_synthesis_test test_name)
set(TEST "${test_name}")
add_synthesis_test(${TEST})
add_dependencies(benchmark_synthesis ${TEST}_synthesis)

endmacro(add_benchmark_synthesis_test)

#add_subdirectory(DSPStone)
#add_subdirectory(CHStone)
add_subdirectory(DFGs)
#add_subdirectory(legup_chstone)