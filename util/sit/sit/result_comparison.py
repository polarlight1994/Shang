#!/usr/bin/env python

import sqlite3
import urllib2
import re

import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf as pdf
import numpy as np
import math

from os import path

def import_db(db_script_url, submit_cur, parameter_parser, flow) :
  conn = sqlite3.connect(':memory:')
  conn.executescript(urllib2.urlopen(db_script_url).read())
  conn.commit()

  cur = conn.cursor()

  rows = cur.execute('''select sim.name, syn.fmax, sim.cycles, syn.les + syn.alm,
                               syn.lut2 + syn.lut3 + syn.lut4 + syn.lut6,
                               syn.regs, syn.mult9 + syn.mult18, syn.mem_bit, sim.parameter,
                               syn.parameter
                        from simulation sim
                          left join synthesis syn
                            on sim.name = syn.name and syn.parameter = sim.parameter
                      order by syn.les DESC''').fetchall()
  for name, fmax, cycles, les, luts, regs, mults, membits, sim_parameter, \
      syn_parameter in rows :
    #if not syn_parameter:
    #  continue

    parameter = sim_parameter if not syn_parameter else syn_parameter

    target_fmax = parameter_parser(parameter)
    if not target_fmax :
      continue
    #print name, fmax, cycles, les, luts, regs, mults, membits, iter_num

    submit_cur.execute('''
INSERT INTO benchmarks(name,
                       fmax, cycles,
                       les, luts, regs, mults, membits,
                       flow, target_fmax)
       VALUES ('%(name)s',
                %(fmax)s, %(cycles)s,
                %(les)s, %(luts)s, %(regs)s, %(mults)s, %(membits)s,
                '%(flow)s', %(target_fmax)s)''' % {
      'name' : name,
      'fmax' : fmax or 0.0, 'cycles' : cycles,
      'les' :les or 0, 'luts' : luts or 0, 'regs' : regs or 0, 'mults' : mults or 0,
      'membits' : membits or 0,
      'flow' : flow,
      'target_fmax' : target_fmax })

def create_compare_db() :
  sql_db = ''':memory:'''
  conn = sqlite3.connect(sql_db)
  conn.executescript('''
  create table benchmarks(
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT,
      fmax REAL,
      cycles INTEGER,

      les INTEGER,
      luts INTERGER,
      regs INTEGER,
      mults INTEGER,
      membits INTEGER,

      flow Text,
      target_fmax INTEGER
  );

  create table runtime(
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT,
      hls_time REAL,
      quartus_time REAL,

      flow Text,
      target_fmax INTEGER
  );
  ''')
  conn.commit()
  return conn.cursor()



def get_metric(benchmark, metric, flow, target_fmax) :
  query = '''select %(metric)s from benchmarks where name = '%(benchmark)s' and flow = '%(flow)s'  and target_fmax = %(target_fmax)s''' % {
             'metric' : metric,
             'benchmark' : benchmark,
             'flow' : flow,
             'target_fmax' : target_fmax
           }
  data = cursor.execute(query).fetchone()
  return float(data[0]) if data else 0.0

def geomean(nums):
  nums = [ d for d in nums
           if d and d > 0 and not math.isnan(d) and not math.isinf(d)]
  if len(nums) != 0 :
    return (reduce(lambda x, y: x*y, nums))**(1.0/len(nums))
  return 0


def get_data(benchmarks, metric, flow, target_fmax) :
  return np.array([ get_metric(benchmark, metric = metric, flow = flow,
                               target_fmax = target_fmax)
                    for benchmark in benchmarks ])

def get_data_for_flows(benchmarks, metric, flows, target_fmax) :
  return [ get_data(benchmarks, metric = metric, flow = flow,
                    target_fmax = target_fmax)
           for flow in flows ]

def nomalize_data(data) :
  base_line = data[0]
  return [ row / base_line for row in data ]

def draw_bars(data, col_label, benchmarks, tilte, ylabel, show_improve,
              extra_action = None) :
  geomeans = []
  scale = 0.9
  figsize=(10.0 * scale, 3.0 * scale)

  num_rows = len(data)
  num_cols = len(benchmarks) + 1
  width = 0.8 / num_rows

  benchmarks_and_geomean = benchmarks[:]
  benchmarks_and_geomean.append('geomean')
  ind = np.arange(len(benchmarks))
  ind = np.append(ind, num_cols - 0.7)


  fig = plt.figure(figsize=figsize)
  fig.canvas.set_window_title(tilte)

  colors = ['SlateGray', 'Cornsilk', 'Navy', 'DeepSkyBlue', 'Indigo']
  idx = 0
  for row, label in zip(data, col_label) :
    mean = geomean(row)
    row_with_geomean = np.append(row, mean)
    geomeans.append(mean)
    if show_improve :
       row_with_geomean = [ 1 - d if d > 0 else 0 for d in row_with_geomean]

    plt.bar(ind + idx * width, row_with_geomean, width, color=colors[idx],
            figure=fig, label = label, log = False)
    idx += 1

  #if not show_improve :
  #  plt.hlines(1.0, 0, num_cols, colors='black', linestyles='solid')

  plt.xticks(ind + num_rows * width / 2., benchmarks_and_geomean)
  #Trim the space on the x axis
  plt.xlim(0, ind[-1] + num_rows * width)

  plt.grid(b=True, which='major', color='k', linestyle='-', axis='y')

  plt.ylabel(ylabel)

  # plt.yscale('log')
  if extra_action:
    extra_action(num_cols, num_rows, width)
  legend_rect = plt.legend(ncol=len(col_label) - 1, # Don't forget the fmax error
                          prop={'size':12}, loc='lower left',
                          bbox_to_anchor=(0, 1))

  if not show_improve :
    plt.hlines(1.0, 0, ind[-1] + num_rows * width,
               colors='k', linestyles='solid')
  #plt.hlines(0.5, 0, num_cols - 1 + num_rows * width,
  #           colors='k', linestyles='solid')

  #Adjust the layout
  fig.autofmt_xdate()
  fig.tight_layout()

  pp = pdf.PdfPages(re.sub('[/ ]', '_', tilte) + '.pdf')
  pp.savefig(fig, bbox_inches='tight', bbox_extra_artists=[legend_rect])
  pp.close()

  #plt.show()
  return geomeans #zip(col_label, geomeans)

def compare_metrics(metrics, benchmarks, baseline_flow, compare_flow,
                    target_fmax, show_improve = 0) :
  flows = [ baseline_flow, compare_flow ]
  data = [ get_data_for_flows(benchmarks, metric, flows, target_fmax)
          for metric in metrics ]
  # Normalize
  normalized_data = [ nomalize_data(row) for row in data ]
  # Discard the baseline
  ratios = [ row[1] for row in normalized_data ]

  return draw_bars(ratios, metrics, benchmarks,
                   '%s / %s at %s' % (compare_flow, baseline_flow, target_fmax),
                   show_improve)

def compare_flows(flows, benchmarks, metric, target_fmax, labels = None,
                  ylabel = None, show_improve = False, prefix = '',
                  hide_baseline = False, file_name = None) :
  data = get_data_for_flows(benchmarks, metric, flows, target_fmax)
  normalized_data = [ row / float(target_fmax) for row in data ] \
                      if metric == 'fmax' and not show_improve else \
                    nomalize_data(data)

  if metric == 'fmax' and show_improve :
    normalized_data = [ np.ones(len(row)) / [ d if d > 0 else 1.0 for d in row ] \
                        for row in normalized_data ]

  if not labels :
    labels = flows[:]

  if hide_baseline:
    normalized_data.pop(0)
    labels = labels[1:]

  if not ylabel :
    ylabel = metric

  if not file_name :
    file_name = '%s%s %s' % (prefix, metric, target_fmax)

  return draw_bars(normalized_data, labels, benchmarks, file_name, ylabel,
                   show_improve)

def parameter_parser_comb_rom(parameter, max_addr_width) :
  max_addr_width_grp = re.search(r'\["vast_max_combinational_rom_logic_level", ([\.\d]+)\]', parameter)
  if not max_addr_width_grp :
    return None

  cur_max_addr_width = int(max_addr_width_grp.group(1))

  if cur_max_addr_width != max_addr_width :
    return None

  return 400


def parameter_parser_final_iteration(parameter) :
  target_fmax_grp = re.search(r'\["fmax", (\d+)\]', parameter)
  target_fmax = 400 if not target_fmax_grp else int(target_fmax_grp.group(1))
  #target_fmax = 446 if target_fmax == 450 else target_fmax

  if '["vast_external_enable_timing_constraint", "true"]' in parameter :
    return None

  #iter_num_grp = re.search(r'\["shang_max_scheduling_iteration", (\d+)\]', syn_parameter)
  #iter_num = 0 if not iter_num_grp else int(iter_num_grp.group(1))

  if 'shang_max_scheduling_iteration' in parameter:
    return None

  return target_fmax


def parameter_parser_first_iteration(parameter) :
  target_fmax_grp = re.search(r'\["fmax", (\d+)\]', parameter)
  target_fmax = 400 if not target_fmax_grp else int(target_fmax_grp.group(1))
  #target_fmax = 446 if target_fmax == 450 else target_fmax

  if '["vast_external_enable_timing_constraint", "true"]' in parameter :
    return None

  iter_num_grp = re.search(r'\["shang_max_scheduling_iteration", (\d+)\]', parameter)
  iter_num = 0 if not iter_num_grp else int(iter_num_grp.group(1))

  if iter_num != 1:
    return None

  return target_fmax

if __name__=='__main__':
  cursor = create_compare_db()
  CombROM = r'''http://192.168.1.253:8010/builders/LongTerm/builds/787/steps/custom%20target/logs/data/text'''

  comb_rom_addr_widths = [0, 1, 2]
  for i in comb_rom_addr_widths :
    import_db(db_script_url = CombROM, submit_cur = cursor,
              parameter_parser = lambda parameter : parameter_parser_comb_rom(parameter, i),
              flow = 'comb_rom_%s_ratio' % i)

  NoPAR = r'''http://192.168.1.253:8010/builders/Synthesis/builds/214/steps/test%20benchmarks/logs/data/text'''

  import_db(db_script_url = NoPAR, submit_cur = cursor,
            parameter_parser = parameter_parser_first_iteration,
            flow = 'NoPAR_Baseline')

  import_db(db_script_url = NoPAR, submit_cur = cursor,
            parameter_parser = parameter_parser_final_iteration,
            flow = 'NoPAR')

  benchmarks = [t[0] for t in cursor.execute('''select distinct name from benchmarks''')]

  flows = [ 'comb_rom_%s_ratio' % i for i in comb_rom_addr_widths ]
  labels = [ 'Comb ROM %s' % i for i in comb_rom_addr_widths ]

  target_fmax = 400

  means = compare_flows(flows,
                        benchmarks, 'cycles', target_fmax, labels,
                        ylabel = 'Normalized cycles latency',
                        hide_baseline = False, show_improve = False,
                        file_name = 'comb_rom_compare_%s' % target_fmax)
  print target_fmax, ', cycles,', ','.join([ str(d) for d in means])

  flows = [ 'NoPAR_Baseline', 'NoPAR' ]
  labels = [ 'Baseline', 'NoPAR' ]

  target_fmax = 400

  means = compare_flows(flows,
                        benchmarks, 'cycles / fmax', target_fmax, labels,
                        ylabel = 'Normalized latency',
                        hide_baseline = False, show_improve = False,
                        file_name = 'no_par_latency_%s' % target_fmax)
  means = compare_flows(flows,
                        benchmarks, 'cycles', target_fmax, labels,
                        ylabel = 'Normalized latency',
                        hide_baseline = False, show_improve = False,
                        file_name = 'no_par_cycles_%s' % target_fmax)
  means = compare_flows(flows,
                        benchmarks, 'fmax', target_fmax, labels,
                        ylabel = 'Normalized latency',
                        hide_baseline = False, show_improve = False,
                        file_name = 'no_par_fmax_%s' % target_fmax)
  print target_fmax, ', latency,', ','.join([ str(d) for d in means])
