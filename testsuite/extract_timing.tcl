load_package report
load_report

#Output the false path.
report_timing -from_clock { clk } -to_clock { clk } -setup -npaths 1 -detail full_path -stdout

qsta_utility::generate_all_summary_tables
report_clock_fmax_summary -panel_name "Fmax Summary"

foreach_in_collection path [get_timing_paths -setup  -npath 1 -detail path_only] {
  set Data_Delay [get_path_info $path -num_logic_levels]
  post_message -type info "DELAY-ESTIMATION-VERIFY: Critical Path Logic Levels $Data_Delay"
}

# Delete the folder if it already existed.
set folderId [get_report_panel_id "Timing Reports"]
if {$folderId != -1} {
  delete_report_panel -id $folderId
}

create_report_panel -folder "Timing Reports"
set id  [create_report_panel -table "Timing Reports||Timging Path"]
add_row_to_table -id $id [list {total} {incr} {rf} {ty} {name} {fanout} {loc}]

set i 0
foreach_in_collection path [get_timing_paths -setup  -npath 8 -detail path_only] {
  incr i
  
  set pathSlack [get_path_info $path -slack]
  #Only record the paths with a negative slack
  if {$pathSlack < 0} {
    # Path summary
    add_row_to_table -id $id \
                    [list "Path #$i" \
                          [get_path_info $path -required_time] \
                          $pathSlack \
                          [get_path_info $path -data_delay] \
                          [get_path_info $path -num_logic_levels] \
                          [get_node_info -name [get_path_info $path -from]]\
                          [get_node_info -name [get_path_info $path -to]]]

    foreach_in_collection pt [ get_path_info $path -arrival_points ] {
      set total     [get_point_info $pt -total]
      set incr      [get_point_info $pt -incr]
      set node_id   [get_point_info $pt -node]
      set type      [get_point_info $pt -type]
      set rf        [get_point_info $pt -rise_fall]
      set fanout    [get_point_info $pt -number_of_fanout]
      set loc       [get_point_info $pt -location]

      set node_name "$node_id"

      if { [string match {node_[0-9]*} $node_id] } {
        set node_name [ get_node_info $node_id -name]
      }
      
      add_row_to_table -id $id [list $total $incr $rf $type $node_name $fanout $loc]
    }
  }
}


create_slack_histogram -clock_name clk -num_bins 64 -panel_name "Timing Reports||Slack Histogram"
save_report_database
unload_report

