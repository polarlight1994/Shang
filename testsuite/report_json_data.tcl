set resource_rpt [open "resource.rpt" w]

puts $resource_rpt "{"

load_report

puts $resource_rpt "\"les\":\"[get_fitter_resource_usage -resource {Total logic elements}]\","
#puts $resource_rpt "\"comb\":\"[get_fitter_resource_usage -comb -used]]\","
# for stratix LE = ALM, comb = ALUT
puts $resource_rpt "\"alm\":\"[get_fitter_resource_usage -alm -used]\","
puts $resource_rpt "\"alut\":\"[get_fitter_resource_usage -alut -used]\","

puts $resource_rpt "\"les_wo_reg\":\"[get_fitter_resource_usage -resource {*Combinational with no register}]\","
puts $resource_rpt "\"les_w_reg_only\":\"[get_fitter_resource_usage -resource {*Register only}]\","
puts $resource_rpt "\"les_and_reg\":\"[get_fitter_resource_usage -resource {*Combinational with a register}]\","

puts $resource_rpt "\"lut6\":\"[get_fitter_resource_usage -resource {*6 input functions}]\","
puts $resource_rpt "\"lut4\":\"[get_fitter_resource_usage -resource {*4 input functions}]\","
puts $resource_rpt "\"lut3\":\"[get_fitter_resource_usage -resource {*3 input functions}]\","
puts $resource_rpt "\"lut2\":\"[get_fitter_resource_usage -resource {*2 input functions}]\","

puts $resource_rpt "\"mem_bit\":\"[get_fitter_resource_usage -mem_bit]\","

puts $resource_rpt "\"les_normal\":\"[get_fitter_resource_usage -resource {*normal mode}]\","
puts $resource_rpt "\"les_arit\":\"[get_fitter_resource_usage -resource {*arithmetic mode}]\","

puts $resource_rpt "\"regs\":\"[get_fitter_resource_usage -resource {*Total registers}]\","

puts $resource_rpt "\"mult9\":\"[get_fitter_resource_usage -resource {*Embedded Multiplier 9-bit elements}]\","
puts $resource_rpt "\"mult18\":\"[get_fitter_resource_usage -resource {*DSP block 18-bit elements}]\","
puts $resource_rpt "\"ave_ic\":\"[get_fitter_resource_usage -resource {*Average interconnect usage (total/H/V)}]\","
puts $resource_rpt "\"peak_ic\":\"[get_fitter_resource_usage -resource {*Peak interconnect usage (total/H/V)}]\""

unload_report

puts $resource_rpt "}"

close $resource_rpt
