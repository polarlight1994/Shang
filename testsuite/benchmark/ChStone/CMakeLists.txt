##################################
#Add the banchmark of simple algorithm
  set(DP_ASAP 		TRUE)		# pipeline_type = DontPipeline, schedule_type = ASAP
  set(ILPMS_ILP 	FALSE)		# pipeline_type = ILPMS, schedule_tpye = ILP
  set(IMS_ASAP 		TRUE)		# pipeline_type = IMS, schedule_type = ASAP

add_subdirectory(dfadd)
add_subdirectory(dfmul)
add_subdirectory(dfdiv)
add_subdirectory(dfsin)

##################################
#Add the benchmark of encryption 

add_subdirectory(sha)
add_subdirectory(aes)
add_subdirectory(adpcm)
add_subdirectory(blowfish)

##################################
#Add the benchmark of other categories

add_subdirectory(gsm)
add_subdirectory(mips)
add_subdirectory(jpeg)
add_subdirectory(motion)

##################################
#Add Deming's benchmarks
add_subdirectory(aircraft)
add_subdirectory(arai)
add_subdirectory(chem)
add_subdirectory(dir)
add_subdirectory(feig_dct)
add_subdirectory(honda)
add_subdirectory(lee)
add_subdirectory(mcm)
add_subdirectory(pr)
add_subdirectory(u5ml)
add_subdirectory(wang)

#message(STATUS "Chstore Benchmarks : " ${Benchmarks})

