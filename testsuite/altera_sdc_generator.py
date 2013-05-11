#!/usr/bin/env python
import sqlite3, argparse


parser = argparse.ArgumentParser(description='Altera SDC Script Generator')
parser.add_argument("--sql", type=str, help="The script to build the sql database")
parser.add_argument("--sdc", type=str, help="The path to which the sdc script will be written")
parser.add_argument("--report", type=str, help="The path to which the report script will be written")
parser.add_argument("--period", type=float, help="The clock period")
parser.add_argument("--factor", type=float, help="The factor to the critical delay", default=0.0)

args = parser.parse_args()

sql_script = open(args.sql, 'r')
sdc_script = open(args.sdc, 'w')
report_script = open(args.report, 'w')

#Write the header of the sdc
sdc_script.write('''
create_clock -name "clk" -period %sns [get_ports {clk}]
derive_pll_clocks -create_base_clocks
derive_clock_uncertainty
set_multicycle_path -from [get_clocks {clk}] -to [get_clocks {clk}] -hold -end 0
set num_not_applied 0
''' % args.period)

#Write the header of the report script
report_script.write('''
load_package report
load_report

#Output the fail path
#report_timing -from_clock { clk } -to_clock { clk } -setup -npaths 1 -detail full_path -stdout

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
''')

con = sqlite3.connect(":memory:")

# Build the multi-cycle path database.
con.executescript(sql_script.read())
con.commit()

cusor = con.cursor()

path_constraints = ''' cycles >= 0 and (thu like 'shang-null-node' or normalized_delay > %f) ''' % args.factor
num_constraints_generated = 0

# Generate the collection for keepers.
keeper_id = 0;
keeper_map = {}

keeper_query = '''SELECT DISTINCT src FROM mcps where %(constraint)s
                    UNION
                  SELECT DISTINCT dst FROM mcps where %(constraint)s''' % {
                    'constraint' : path_constraints
                }
for keeper_row in cusor.execute(keeper_query):
  keeper = keeper_row[0]
  keeper_map[keeper] = keeper_id
  keeper_line = '''set keepers%(id)s [get_keepers {%(keeper_patterns)s}]\n''' % {
                   'id':keeper_id, 'keeper_patterns' : keeper
                }
  sdc_script.write(keeper_line)
  report_script.write(keeper_line)
  keeper_id += 1

# Generate the collection for nets
net_id = 0;
net_map = { 'shang-null-node' : None }
net_query = '''SELECT DISTINCT thu FROM mcps where %(constraint)s''' % {
               'constraint' : path_constraints
            }
for net_row in cusor.execute(net_query):
  net = net_row[0]
  if net in net_map: continue

  net_map[net] = net_id
  net_line = '''set nets%(id)s [get_nets {%(net_patterns)s}]\n''' % {
             'id':net_id, 'net_patterns' : net }
  sdc_script.write(net_line)
  report_script.write(net_line)
  net_id += 1

# Generate the multi-cycle path constraints.
def generate_constraint(**kwargs) :
  if kwargs['thu'] == 'netsNone' :
    kwargs['cnd_string'] = '''[get_collection_size $%(src)s] && [get_collection_size $%(dst)s]''' % kwargs
    kwargs['path_fileter'] = '''-from $%(src)s -to $%(dst)s''' % kwargs
  else :
    kwargs['cnd_string'] = '''[get_collection_size $%(src)s] && [get_collection_size $%(dst)s] && [get_collection_size $%(thu)s]''' % kwargs
    kwargs['path_fileter'] = '''-from $%(src)s -through $%(thu)s -to $%(dst)s''' % kwargs

  sdc_script.write('''if { %(cnd_string)s } { set_multicycle_path %(path_fileter)s -setup -end %(cycles)d \n''' % kwargs)
  report_script.write(
'''if { %(cnd_string)s } {
  set fail_paths [get_timing_paths %(path_fileter)s -setup -npaths 1 -less_than_slack $slack_threshold ]
  foreach_in_collection path $fail_paths {
    set slack [get_path_info $path -slack]
    set delay [get_path_info $path -data_delay]
    post_message -type info "Available cycles: %(cycles)d, estimated delay: %(delay)f, actual delay: $delay, slack: $slack"
    report_timing %(path_fileter)s -setup -npaths 1 -less_than_slack $slack_threshold -detail full_path -stdout
  }
}\n''' % kwargs)

  global num_constraints_generated
  num_constraints_generated = num_constraints_generated + 1

def generate_constraints_from_src_to_dst(src, dst) :
  query = '''SELECT thu, cycles, normalized_delay FROM mcps
             where %(constraint)s and dst = '%(dst)s' and src = '%(src)s'
             ORDER BY cycles ASC ''' % {
             'constraint' : path_constraints,
             'dst' : dst,
             'src' : src
          }
  for thu, cycles, delay in [ (row[0], row[1], row[2] ) for row in cusor.execute(query) ]:
    sdc_script.write('''# %(src)s -> %(thu)s -> %(dst)s %(cycles)s\n''' % {
                        'src' : src, 'dst' : dst, 'thu' : thu, 'cycles' : cycles})
    generate_constraint(src="keepers%s" % keeper_map[src], dst="keepers%s" % keeper_map[dst], thu="nets%s" % net_map[thu], cycles=cycles, delay=delay)
    sdc_script.write('''} else''')
    sdc_script.write(''' { incr num_not_applied }\n''')
    sdc_script.write('''post_message -type info "."\n\n''')


def generate_constraints_for_dst(dst) :
  query = '''SELECT DISTINCT src FROM mcps where %(constraint)s and dst = '%(dst)s' ''' % {
             'constraint' : path_constraints, 'dst' : dst}
  for src in [ row[0] for row in cusor.execute(query) ]:
    generate_constraints_from_src_to_dst(src, dst)

# Get all dst nodes.
dst_query = '''SELECT DISTINCT dst FROM mcps where %(constraint)s''' % { 'constraint' : path_constraints}
for dst in [ row[0] for row in cusor.execute(dst_query) ]:
  generate_constraints_for_dst(dst)

# Report the finish of script
sdc_script.write('''post_message -type info "$num_not_applied constraints are not applied"\n''')

sdc_script.close()
print num_constraints_generated, ' constraints genrated'
