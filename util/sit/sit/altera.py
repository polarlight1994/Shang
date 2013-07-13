#!/usr/bin/env python
import sqlite3, argparse

class ConstraintGenerator:
  def __init__(self, sql_connection, path_constraints, output_script_path, script_prologue, script_on_path, script_epilog, period):
    self.path_constraints = path_constraints
    self.num_path_script_generated = 0
    self.cusor = sql_connection.cursor()

    #Other variables
    self.output_script_path = output_script_path
    self.period = period
    self.script_prologue = script_prologue
    self.script_on_path = script_on_path
    self.script_epilog = script_epilog

  def __getitem__(self, key):
    return self.__dict__[key]

  def generate_node_sets(self) :
    # Generate the collection for keepers.
    keeper_id = 0;
    self.keeper_map = {}

    keeper_query = '''SELECT DISTINCT src FROM mcps where %(path_constraints)s
                        UNION
                      SELECT DISTINCT dst FROM mcps where %(path_constraints)s''' % self
    for keeper_row in self.cusor.execute(keeper_query):
      keeper = keeper_row[0]
      self.keeper_map[keeper] = keeper_id
      keeper_line = '''set keepers%(id)s [get_keepers {%(keeper_patterns)s}]\n''' % {
                       'id':keeper_id, 'keeper_patterns' : keeper
                    }
      self.output_script.write(keeper_line)
      keeper_id += 1

    # Generate the collection for nets
    net_id = 0;
    self.net_map = { 'shang-null-node' : None }
    net_query = '''SELECT DISTINCT thu FROM mcps where %(path_constraints)s''' % self

    for net_row in self.cusor.execute(net_query):
      net = net_row[0]
      if net in self.net_map: continue

      self.net_map[net] = net_id
      net_line = '''set nets%(id)s [get_pins -compatibility_mode {%(net_patterns)s}]\n''' % {
                 'id':net_id, 'net_patterns' : net }
      self.output_script.write(net_line)
      net_id += 1

  # Generate the multi-cycle path constraints.
  def generate_script_for_path(self, **kwargs) :
    kwargs['period'] = self.period
    if kwargs['thu'] == 'netsNone' :
      kwargs['cnd_string'] = '''1''' % kwargs
      kwargs['path_fileter'] = '''-from $%(src)s -to $%(dst)s''' % kwargs
    else :
      kwargs['cnd_string'] = '''[get_collection_size $%(thu)s]''' % kwargs
      kwargs['path_fileter'] = '''-from $%(src)s -through $%(thu)s -to $%(dst)s''' % kwargs

    self.output_script.write(self.script_on_path % kwargs)
    self.num_path_script_generated = self.num_path_script_generated + 1

  def generate_script_from_src_to_dst(self, src, dst) :
    query = '''SELECT thu, cycles, normalized_delay FROM mcps
               where %(constraint)s and dst = '%(dst)s' and src = '%(src)s'
               ORDER BY cycles ASC, constraint_order ASC''' % {
               'constraint' : self.path_constraints,
               'dst' : dst,
               'src' : src
            }

    self.output_script.write('if { [get_collection_size $%(src)s] } {\n' % { 'src' : "keepers%s" % self.keeper_map[src] })
    for thu, cycles, delay in [ (row[0], row[1], row[2] ) for row in self.cusor.execute(query) ]:
      self.generate_script_for_path(src="keepers%s" % self.keeper_map[src], dst="keepers%s" % self.keeper_map[dst], thu="nets%s" % self.net_map[thu], cycles=cycles, delay=delay)
    self.output_script.write('}\n')

  def generate_script_for_dst(self, dst) :
    query = '''SELECT DISTINCT src FROM mcps where %(constraint)s and dst = '%(dst)s' ''' % {
               'constraint' : self.path_constraints, 'dst' : dst}
    self.output_script.write('if { [get_collection_size $%(dst)s] } {\n' % { 'dst' : "keepers%s" % self.keeper_map[dst] })
    for src in [ row[0] for row in self.cusor.execute(query) ]:
      self.generate_script_from_src_to_dst(src, dst)
    self.output_script.write('}\n')

  def generate_script(self):
    self.output_script = open(self.output_script_path, 'w')
    self.output_script.write(self.script_prologue % self)
    self.generate_node_sets()
    # Get all dst nodes.
    dst_query = '''SELECT DISTINCT dst FROM mcps where %(path_constraints)s''' % self
    for dst in [ row[0] for row in self.cusor.execute(dst_query) ]:
      self.generate_script_for_dst(dst)

    self.output_script.write(self.script_epilog % self)
    self.output_script.close()
    self.cusor.close()

    return self.num_path_script_generated

def generate_scripts(sql_path, sdc_path, report_path, period, factor) :
  # Initialize the sql database.
  sql_script = open(sql_path, 'r')
  con = sqlite3.connect(":memory:")

  # Build the multi-cycle path database.
  con.executescript(sql_script.read())
  con.commit()
  sql_script.close();

  sdc_generator = ConstraintGenerator(
    sql_connection = con,
    path_constraints = ''' cycles > 1 and (thu like 'shang-null-node' or normalized_delay > %f) ''' % factor,
    output_script_path = sdc_path,
    script_prologue = '''
create_clock -name "clk" -period %(period)sns [get_ports {clk}]
derive_pll_clocks -create_base_clocks
derive_clock_uncertainty
set_max_delay -from start %(period)sns
set num_not_applied 0
''',
    script_on_path = '''
# %(src)s -> %(thu)s -> %(dst)s %(cycles)s %(delay)s
if { %(cnd_string)s } {
  set_multicycle_path %(path_fileter)s -setup -end %(cycles)d
} else { incr num_not_applied }
post_message -type info "%(src)s -> %(thu)s -> %(dst)s %(cycles)s %(delay)s"

''',
    script_epilog = '''
post_message -type info "$num_not_applied constraints are not applied"
set_multicycle_path -from [get_clocks {clk}] -to [get_clocks {clk}] -hold -end 0
''',
    period = period)

  # Report the finish of script
  print sdc_generator.generate_script(), " scripts generated"

  report_generator = ConstraintGenerator(
    sql_connection = con,
    # Just include every path.
    path_constraints = ''' cycles >= 0 ''',
    output_script_path = report_path,
    script_prologue = '''
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

close $fmax_rpt

save_report_database
unload_report

set worst_slack 0.0
set worst_paths [get_timing_paths -nworst 1 -less_than_slack 0.0 ]
foreach_in_collection path $worst_paths {
  set worst_slack [get_path_info $path -slack]
  post_message -type info "Worst slack: $worst_slack"
}
set slack_threshold [expr $worst_slack * 0.9]

set report_path_info 0
''',
    script_on_path = '''
if { $report_path_info && (%(cnd_string)s) } {
    set fail_paths [get_timing_paths %(path_fileter)s -setup -npaths 1 -less_than_slack $slack_threshold ]
    foreach_in_collection path $fail_paths {
      set slack [get_path_info $path -slack]
      set delay [get_path_info $path -data_delay]
      post_message -type info "Timing violation: available cycles: %(cycles)d, estimated delay: [expr %(delay)f * %(period)f], actual delay: $delay, slack: $slack path begin:"
      report_timing %(path_fileter)s -setup -npaths 1 -less_than_slack $slack_threshold -detail full_path -stdout
      post_message -type info "end path"
    }
}
''',
    script_epilog = '',
    period = period)

  report_generator.generate_script()

  #Close the connection.
  con.close()

if __name__=='__main__':
  parser = argparse.ArgumentParser(description='Altera SDC Script Generator')
  parser.add_argument("--sql", type=str, help="The script to build the sql database")
  parser.add_argument("--sdc", type=str, help="The path to which the sdc script will be written")
  parser.add_argument("--report", type=str, help="The path to which the report script will be written")
  parser.add_argument("--period", type=float, help="The clock period")
  parser.add_argument("--factor", type=float, help="The factor to the critical delay", default=0.0)

  args = parser.parse_args()
  generate_scripts(sql_path = args.sql, sdc_path = args.sdc, report_path = args.report, period = args.period, factor = args.factor)
