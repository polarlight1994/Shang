#!/usr/bin/env python
import sqlite3, argparse


parser = argparse.ArgumentParser(description='Altera SDC Script Generator')
parser.add_argument("--sql", type=str, help="The script to build the sql database")
parser.add_argument("--sdc", type=str, help="The path to which the sdc script will be written")
parser.add_argument("--period", type=float, help="The clock period")
parser.add_argument("--factor", type=float, help="The factor to the critical delay", default=0.0)

args = parser.parse_args()

sql_script = open(args.sql, 'r')
sdc_script = open(args.sdc, 'w')

sdc_script.write('''
create_clock -name "clk" -period %sns [get_ports {clk}]
derive_pll_clocks -create_base_clocks
derive_clock_uncertainty
set_multicycle_path -from [get_clocks {clk}] -to [get_clocks {clk}] -hold -end 0
set num_not_applied 0
''' % args.period)

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
  sdc_script.write('''set keepers%(id)s [get_keepers {%(keeper_patterns)s}]\n''' % {
                   'id':keeper_id, 'keeper_patterns' : keeper
                   })
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
  sdc_script.write('''set nets%(id)s [get_nets {%(net_patterns)s}]\n''' % {
                   'id':net_id, 'net_patterns' : net }
                  )
  net_id += 1

# Generate the multi-cycle path constraints.
def generate_constraint(**kwargs) :
  if kwargs['thu'] == 'netsNone' :
    sdc_script.write('''if { [get_collection_size $%(src)s] && [get_collection_size $%(dst)s] } { set_multicycle_path -from $%(src)s -to $%(dst)s -setup -end %(cycles)d \n''' % kwargs)
  else :
    sdc_script.write('''if { [get_collection_size $%(src)s] && [get_collection_size $%(dst)s] && [get_collection_size $%(thu)s] } { set_multicycle_path -from $%(src)s -through $%(thu)s -to $%(dst)s -setup -end %(cycles)d \n''' % kwargs)
  global num_constraints_generated
  num_constraints_generated = num_constraints_generated + 1

def generate_constraints_from_src_to_dst(src, dst) :
  query = '''SELECT thu, cycles FROM mcps
             where %(constraint)s and dst = '%(dst)s' and src = '%(src)s'
             ORDER BY cycles ASC ''' % {
             'constraint' : path_constraints,
             'dst' : dst,
             'src' : src
          }
  for thu, cycles in [ (row[0], row[1]) for row in cusor.execute(query) ]:
    sdc_script.write('''# %(src)s -> %(thu)s -> %(dst)s %(cycles)s\n''' % {
                        'src' : src, 'dst' : dst, 'thu' : thu, 'cycles' : cycles})
    generate_constraint(src="keepers%s" % keeper_map[src], dst="keepers%s" % keeper_map[dst], thu="nets%s" % net_map[thu], cycles=cycles)
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
