load_package report
load_report

#Output the false path.
report_timing -from_clock { clk } -to_clock { clk } -setup -npaths 1 -detail full_path -stdout

qsta_utility::generate_all_summary_tables
report_clock_fmax_summary -panel_name "Fmax Summary"

save_report_database
unload_report

