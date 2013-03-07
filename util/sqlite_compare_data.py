#! /usr/bin/python

import json
import sqlite3
import re
import sys
from datetime import datetime

def build_cell(benchmark, baseline_id, compare_id, metric) :
  baseline_val = cursor.execute("select %s from synthesis_run where experiment_id == %s and name like '%s'" % (metric, base_line_id, benchmark)).fetchone()[0]
  compare_val = cursor.execute("select %s from synthesis_run where experiment_id == %s and name like '%s'" % (metric, compare_id, benchmark)).fetchone()[0]
  return (compare_val, baseline_val)

  
def format_ratio(t) :
  if t[1] != 0 :
    return "%.2f" % (float(t[0]) / float(t[1]))
  elif t[0] != 0 :
    return "%s / %s" % (t[0], t[1])
  return  "1" #"%s / %s" % (t[0], t[1])
  

def format_ratio_invert(t) :
  return format_ratio((t[1], t[0]))

def append(list, data):
    if data != 0 :
        list.append(data)
    return

sql_db = sys.argv[1]
conn = sqlite3.connect(sql_db)
cursor = conn.cursor()

base_line_id = sys.argv[2]
compare_id = sys.argv[3]

print "Basline:"
print cursor.execute("select comments  from experiment_comment where id == %s" % base_line_id).fetchone()[0]
print "Compare:"
print cursor.execute("select comments  from experiment_comment where id == %s" % compare_id).fetchone()[0]

benchmarks = cursor.execute("select name from synthesis_run where experiment_id == %s" % compare_id).fetchall()

print "benchmark, period, cycles, latency, les, regs, mults, membits, all_les, latency_area"
for benchmark_row in benchmarks:
  benchmark = benchmark_row[0]
  fmax = build_cell(benchmark, base_line_id, compare_id, "fmax")
  cycles = build_cell(benchmark, base_line_id, compare_id, "cycles")
  latency = (cycles[0] / fmax[0], cycles[1] / fmax[1])
  les    = build_cell(benchmark, base_line_id, compare_id, "les")
  regs   = build_cell(benchmark, base_line_id, compare_id, "regs")
  mults = build_cell(benchmark, base_line_id, compare_id, "mults")
  membits = build_cell(benchmark, base_line_id, compare_id, "membits")
  all_les = les + 115 * mults
  latency_area = (latency[0] * all_les[0], latency[1] * all_les[1])
  print benchmark, ',', format_ratio_invert(fmax), ',', format_ratio(cycles), ',', format_ratio(latency), ',', format_ratio(les), ',', format_ratio(regs), ',', format_ratio(mults), ',', format_ratio(membits), ',', format_ratio(all_les), ',', format_ratio(latency_area)

#  print "benchmark, fmax, cycles, les, regs, mults, membits"


  
conn.commit()
conn.close()
