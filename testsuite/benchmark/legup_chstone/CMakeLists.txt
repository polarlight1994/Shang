add_custom_target(legup_ir)

set(ENV{all_legup_benchmark} "")
macro(add_test_legup_cases benchmark bc_file)
  set(src_bc_path "${CMAKE_CURRENT_SOURCE_DIR}/${bc_file}")
  set(dst_bc_path "${CMAKE_CURRENT_BINARY_DIR}/${benchmark}.bc")
  add_custom_command(OUTPUT ${dst_bc_path}
    # Only update the real output file if there are any differences.
    # This prevents recompilation of all the files depending on it if there
    # aren't any.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${src_bc_path} ${dst_bc_path}
    DEPENDS ${src_bc_path}
    COMMENT "Copying ${src_bc_path} ..."
  )

  add_custom_target(legup_${benchmark}_ir DEPENDS ${dst_bc_path})
  add_dependencies(legup_ir legup_${benchmark}_ir)

  add_test_config("legup_${benchmark}_hardware_sim" ${dst_bc_path} legup_${benchmark}_ir pure_hw_sim)
  add_test_config("legup_${benchmark}_altera_syn" ${dst_bc_path} legup_${benchmark}_ir altera_syn)
  add_test_config("legup_${benchmark}_altera_nls" ${dst_bc_path} legup_${benchmark}_ir altera_nls)
  set(ENV{all_legup_benchmark} "$ENV{all_legup_benchmark} ${dst_bc_path}")
endmacro(add_test_legup_cases)

add_test_legup_cases(dfmul dfmul.bc)
add_test_legup_cases(dfdiv dfdiv.bc)
add_test_legup_cases(dfsin dfsin.bc)

add_test_legup_cases(gsm gsm.bc)

add_test_legup_cases(mpeg2 mpeg2.bc)

add_test_legup_cases(sha sha_driver.bc)
add_test_legup_cases(aes aes.bc)

add_test_legup_cases(mips mips.bc)

#Deming's benchmark
add_test_legup_cases(pr pr.bc)
add_test_legup_cases(wang wang.bc)
add_test_legup_cases(chem chem.bc)
add_test_legup_cases(arai arai.bc)
add_test_legup_cases(feig_dct feig_dct.bc)
add_test_legup_cases(u5ml u5ml.bc)
add_test_legup_cases(mcm mcm.bc)
add_test_legup_cases(lee lee.bc)
add_test_legup_cases(aircraft aircraft.bc)
add_test_legup_cases(dir dir.bc)
add_test_legup_cases(matrixmultiply matrixmultiply.bc)

add_test_legup_cases(fft fft.bc)

#add_test_legup_cases(monte_carlo monte_carlo.bc)

#add_test_legup_cases(option_pricing option_pricing.bc)

#These there are very large design
#add_test_legup_cases(ogg ogg.bc)

add_test_legup_cases(adpcm adpcm.bc)

add_test_legup_cases(blowfish bf.bc)

add_test_legup_cases(jpeg main.bc)
add_test_legup_cases(dfadd dfadd.bc)

add_test_config(legup_simulation $ENV{all_legup_benchmark} legup_ir pure_hw_sim)
add_test_config(legup_synthesis  $ENV{all_legup_benchmark} legup_ir altera_syn)
add_test_config(legup_netlistsim  $ENV{all_legup_benchmark} legup_ir altera_nls)
