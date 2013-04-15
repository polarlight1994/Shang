load_package report
load_report

#Output the fail path
report_timing -from_clock { clk } -to_clock { clk } -setup -npaths 1 -detail full_path -stdout

qsta_utility::generate_all_summary_tables
report_clock_fmax_summary -panel_name "Fmax Summary"

# Write the fmax report.
set fmax_rpt [open "clk_fmax.rpt" w]

set domain_list [get_clock_fmax_info]
foreach domain $domain_list {
	set name [lindex $domain 0]
	set fmax [lindex $domain 1]
	set restricted_fmax [lindex $domain 2]

  # Write the restricted_fmax
  puts $fmax_rpt $restricted_fmax
	#puts $fmax_rpt "Clock $name : Fmax = $fmax (Restricted Fmax = $restricted_fmax)"
}

save_report_database
unload_report

