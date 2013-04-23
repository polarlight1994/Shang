#!/usr/bin/env python
import sqlite3, argparse


parser = argparse.ArgumentParser(description='Altera SDC Script Generator')
parser.add_argument("--sql", type=str, help="The script to build the sql database")
parser.add_argument("--sdc", type=str, help="The path to which the sdc script will be written")
parser.add_argument("--period", type=str, help="The clock period")

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

# Generate the path for the critical constraints.
cusor = con.cursor()

def generate_constraint(**kwargs) :
  if kwargs['thu'] == "shang-null-node" :
    sdc_script.write('''set_multicycle_path -from {%(src)s} -to {%(dst)s} -setup -end %(cycles)d\n''' % kwargs)
  else :
    sdc_script.write('''set_multicycle_path -from {%(src)s} -through {%(thu)s} -to {%(dst)s} -setup -end %(cycles)d\n''' % kwargs)

for row in cusor.execute('''SELECT * FROM mcps ORDER BY cycles''') :
  generate_constraint(src=row[1], dst=row[2], thu=row[3], cycles=row[4])

sdc_script.close()
