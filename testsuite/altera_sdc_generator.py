#!/usr/bin/env python
import sqlite3, argparse


parser = argparse.ArgumentParser(description='Altera SDC Script Generator')
parser.add_argument("--sql", type=str, help="The script to build the sql database")
parser.add_argument("--sdc", type=str, help="The path to which the sdc script will be written")
parser.add_argument("--period", type=float, help="The clock period")
parser.add_argument("--ratio", type=float, help="The clock period", default=1.0)

args = parser.parse_args()

sql_script = open(args.sql, 'r')
sdc_script = open(args.sdc, 'w')

sdc_script.write('''
create_clock -name "clk" -period %sns [get_ports {clk}]
derive_pll_clocks -create_base_clocks
derive_clock_uncertainty
set_multicycle_path -from [get_clocks {clk}] -to [get_clocks {clk}] -hold -end 0
''' % args.period)

con = sqlite3.connect(":memory:")

# Build the multi-cycle path database.
con.executescript(sql_script.read())
con.commit()

cusor = con.cursor()

# Generate the collection for keepers.
keeper_id = 0;
keeper_map = {}

for keeper_row in cusor.execute('''SELECT DISTINCT src FROM mcps UNION SELECT DISTINCT dst FROM mcps'''):
  keeper = keeper_row[0]
  keeper_map[keeper] = keeper_id
  sdc_script.write('''set keepers%(id)s [get_keepers {%(keeper_patterns)s}]\n''' % { 'id':keeper_id, 'keeper_patterns' : keeper })
  keeper_id += 1

# Generate the collection for nets
net_id = 0;
net_map = { 'shang-null-node' : None }
for net_row in cusor.execute('''SELECT DISTINCT thu FROM mcps where thu not like 'shang-null-node' '''):
  nets = net_row[0]
  for net in nets.split():
    if net in net_map: continue

    net_map[net] = net_id
    sdc_script.write('''set nets%(id)s [get_nets {%(net_patterns)s}]\n''' % { 'id':net_id, 'net_patterns' : net })
    net_id += 1

# Generate the multi-cycle path constraints.
def generate_constraint(**kwargs) :
  if kwargs['thu'] == 'netsNone' :
    sdc_script.write('''if { [get_collection_size $%(src)s] && [get_collection_size $%(dst)s] } { set_multicycle_path -from $%(src)s -to $%(dst)s -setup -end %(cycles)d }\n''' % kwargs)
  else :
    sdc_script.write('''if { [get_collection_size $%(src)s] && [get_collection_size $%(dst)s] && [get_collection_size $%(thu)s] } { set_multicycle_path -from $%(src)s -through $%(thu)s -to $%(dst)s -setup -end %(cycles)d }\n''' % kwargs)

rows = cusor.execute('''SELECT * FROM mcps ORDER BY dst, src, cycles ASC''').fetchall()

constraints_to_generate = int(args.ratio * len(rows))

print constraints_to_generate

for i in range(0, constraints_to_generate):
  row = rows[i]
  normalized_delay = row[5]
  thu_patterns = row[3]
  cycles = row[4]
#  if normalized_delay > 1.0:
  for thu_pattern in thu_patterns.split() :
    src_pattern = row[1]
    src = "keepers%s" % keeper_map[src_pattern]
    dst_pattern = row[1]
    dst = "keepers%s" % keeper_map[dst_pattern]
    thu = "nets%s" % net_map[thu_pattern]
    generate_constraint(src=src, dst=dst, thu=thu, cycles=cycles)
    sdc_script.write('else')
  sdc_script.write(''' { post_message -type warning {Constraints are not able to applied to %(src)s->%(thu)s->%(dst)s cycles:%(cycles)s normalized_delay:%(normalized_delay)s } }\n\n''' % {
    'src' : src_pattern,
    'dst' : dst_pattern,
    'thu' : thu_patterns,
    'cycles' : cycles,
    'normalized_delay' : normalized_delay })

sdc_script.close()
