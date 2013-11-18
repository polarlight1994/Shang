add_llvm_library(lpsolve
lp_MDO.c
shared/commonlib.c
shared/mmio.c
shared/myblas.c
ini.c
colamd/colamd.c
lp_rlp.c
lp_crash.c
bfp/bfp_LUSOL/lp_LUSOL.c
bfp/bfp_LUSOL/LUSOL/lusol.c
lp_Hash.c
lp_lib.c
lp_wlp.c
lp_matrix.c
lp_mipbb.c
lp_MPS.c
lp_params.c
lp_presolve.c
lp_price.c
lp_pricePSE.c
lp_report.c
lp_scale.c
lp_simplex.c
lp_SOS.c
lp_utils.c
yacc_read.c)

add_definitions( -DCHECK_SOLUTION )
add_definitions( -DYY_NEVER_INTERACTIVE )
add_definitions( -DPARSER_LP )
add_definitions( -DINVERSE_ACTIVE=INVERSE_LUSOL )
add_definitions( -DRoleIsExternalInvEngine )

set(LPSOLVE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(GLOBAL PROPERTY LPSOLVE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(GLOBAL PROPERTY LPSOLVE_LIBRARY "Luabind")

include_directories(
  ${LPSOLVE_INCLUDE_DIR}
  ${LPSOLVE_INCLUDE_DIR}/colamd
  ${LPSOLVE_INCLUDE_DIR}/shared
  ${LPSOLVE_INCLUDE_DIR}/bfp/
  ${LPSOLVE_INCLUDE_DIR}/bfp/bfp_LUSOL/
  ${LPSOLVE_INCLUDE_DIR}/bfp/bfp_LUSOL/LUSOL
)
