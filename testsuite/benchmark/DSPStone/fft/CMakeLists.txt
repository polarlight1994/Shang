file(GLOB test_sources *.cpp)
foreach(test_source ${test_sources})
  get_filename_component(file_name ${test_source} NAME_WE)
  add_benchmark_cosimulation_test(${file_name})
  add_benchmark_synthesis_test(${file_name})
  add_hls_target(${file_name})
endforeach()
