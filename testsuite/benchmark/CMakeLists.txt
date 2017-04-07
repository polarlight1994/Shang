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

macro(add_hls_target test_name)
set(TEST "${test_name}")
add_high_level_synthesis(${TEST})
add_dependencies(benchmark_hls ${TEST}_hls)

endmacro(add_hls_target)

add_subdirectory(DSPStone)
add_subdirectory(CHStone)
add_subdirectory(DFGs)
#add_subdirectory(SimpleStone)
#add_subdirectory(mediabench)
#add_subdirectory(Mutil_Operand_Adder)
#add_subdirectory(legup_chstone)