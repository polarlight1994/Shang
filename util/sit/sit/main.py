#!/usr/bin/env python

"""
sit - Shang Integrated Tester.

"""

import math, os, platform, random, re, sys, time, threading, itertools, json, argparse

import drmaa
import sqlite3

from jinja2 import Environment, FileSystemLoader, Template

from logparser import SimLogParser
from teststeps import TestStep, HLSStep, Session

def ParseOptions() :
  parser = argparse.ArgumentParser(description='The Shang Integrated Tester')
  parser.add_argument("--mode", type=str, choices=[TestStep.HybridSim, TestStep.PureHWSim, TestStep.AlteraSyn], help="the mode of sit", required=True)
  parser.add_argument("--tests", type=str, help="tests to run", required=True)
  parser.add_argument("--config_bin_dir", type=str, help="base binary dir of the test suit (to locate the config templates)", required=True)
  parser.add_argument("--sge_queue", type=str, help="the destinate sge queue to submit job", required=True)

  return parser.parse_args()

def main(builtinParameters = {}):
  args = ParseOptions()
  basic_config = vars(args).copy()

  # Load the basic configuration.
  with open(os.path.join(args.config_bin_dir, 'basic_config.json'), 'r') as jsondata :
    basic_config.update(json.load(jsondata))
  jsondata.close()

  print "Starting the Shang Integrated Tester in", args.mode, "mode..."


  active_jobs = []

  # Initialize the database connection
  con = sqlite3.connect(":memory:")

  # Create the tables for the experimental results.
  # We create 3 tables: HLS results, simulation results, and synthesis results
  con.executescript('''
    create table logfile(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT,
        parameter TEXT,
        stdout TEXT,
        stderr TEXT,
        test_file TEXT,
        synthesis_config_file TEXT,
        status TEXT
    );

    create table simulation(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT,
        parameter TEXT,
        cycles INTEGER,
        mem_cycles INTEGER
    );

    create table synthesis(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT,
        parameter TEXT,

        fmax REAL,

        mem_bit INTEGER,

        regs INTEGER,

        alut INTEGER,
        alm  INTEGER,

        les        INTEGER,
        les_wo_reg INTEGER,
        les_w_reg_only INTEGER,
        les_and_reg    INTEGER,
        les_normal INTEGER,
        les_arit   INTEGER,

        lut2 INTEGER,
        lut3 INTEGER,
        lut4 INTEGER,
        lut6 INTEGER,

        mult9  INTEGER,
        mult18 INTEGER
    );
  ''')
  # This is not necessary since we only have 1 connection.
  con.commit()

  # Build the opation space from the configuration.
  option_space_dict = {}
  # Constrains the option space
  # HLS options
  option_space_dict['vast_disable_mux_slack'] = [ 'true' ]
  option_space_dict['shang_enable_mux_pipelining'] = [ 'true' ]
  option_space_dict['shang_baseline_scheduling_only'] = [ 'false' ]
  option_space_dict['shang_enable_memory_optimization'] = [ 'true' ]
  option_space_dict['shang_enable_memory_partition'] = [ 'true' ]
  option_space_dict['shang_enable_pre_schedule_lut_mapping'] = [ 'true' ]
  option_space_dict['shang_enable_register_sharing'] = [ 'false' ]
  option_space_dict['shang_constraints_factor'] = [ -0.1 ]

  option_space_dict['timing_model'] = [ 'external' if args.mode == TestStep.AlteraSyn else 'bit-level' ]

  option_space_dict['fmax'] = [ 100 ]
  option_space_dict['device_family'] = [ 'CycloneII' ]

  option_space = [ dict(itertools.izip(option_space_dict, opt))  for opt in itertools.product(*option_space_dict.itervalues()) ]

  # Collect the option space of the fail cases
  fail_space = dict([ (k, set()) for k in option_space_dict.iterkeys() ])

  for test_path in args.tests.split() :
    basedir = os.path.dirname(test_path)
    test_file = os.path.basename(test_path)
    test_name = os.path.splitext(test_file)[0]

    #test_option = random.choice(option_space)
    for test_option in option_space :
      # TODO: Provide the keyword constructor
      # Expand the test_option so that the test option always override the basic_config.
      hls_step = HLSStep(dict(basic_config, **test_option))
      hls_step.test_name = test_name
      hls_step.hardware_function = test_name if args.mode == TestStep.HybridSim else 'main'
      hls_step.test_file = test_path
      hls_step.option = test_option
      hls_step.option_space_dict = option_space_dict

      hls_step.prepareTest()
      hls_step.runTest()
      time.sleep(1)

      active_jobs.append(hls_step)

  # Examinate the status of the jobs
  while active_jobs :
    next_active_jobs = []
    for job in active_jobs:
      status = job.jobStatus()

      if status == 'running' :
        next_active_jobs.append(job)
        continue

      if status == 'passed' :
        # Now the job finished successfully
        print "Test", job.test_name, job.step_name, "passed"
        # Generate subtest.
        # FIXME: Only generate the subtest if the previous test passed.
        for subtest in job.generateSubTests() :
          subtest.prepareTest()
          subtest.runTest()
          time.sleep(1)
          next_active_jobs.append(subtest)
      elif status == 'failed' :
        print "Test", job.getStepDesc(), "failed"
        # Remember the options on the fail case
        for k, v in job.option.iteritems() :
          fail_space[k].add(v)

      job.submitResults(con, status)

    time.sleep(5)
    active_jobs = next_active_jobs[:]
    print len(active_jobs), "tests left"
    sys.stdout.flush()

  # Wait untill all HLS jobs finish
  #s.synchronize([ drmaa.Session.JOB_IDS_SESSION_ALL ], drmaa.Session.TIMEOUT_WAIT_FOREVER)

  #  #print 'Job: ' + str(retval.jobId) + ' finished with status ' + str(retval.hasExited)

  # Finialize the gridengine
  Session.exit()

  cur = con.cursor()

  with open(os.path.join(args.config_bin_dir, 'data.sql'), 'w') as database_script:
    for line in con.iterdump():
      database_script.write(line)
      database_script.write('\n')
  database_script.close()

  # Analysis the fail cases
  print 'Fail space:', [ (k, v) for k, v in fail_space.iteritems() if v < set(option_space_dict[k]) ]

if __name__=='__main__':
    main()
