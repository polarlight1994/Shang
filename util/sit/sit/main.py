#!/usr/bin/env python

"""
sit - Shang Integrated Tester.

"""

import math, os, platform, random, re, sys, time, threading, itertools, json

import drmaa
import sqlite3

from jinja2 import Environment, FileSystemLoader, Template

from logparser import SimLogParser
from teststeps import TestStep, HLSStep

def ParseOptions() :
  import argparse
  parser = argparse.ArgumentParser(description='The Shang Integrated Tester')
  parser.add_argument("--mode", type=str, choices=[TestStep.HybridSim, TestStep.PureHWSim, TestStep.AlteraSyn], help="the mode of sit", required=True)
  parser.add_argument("--tests", type=str, help="tests to run", required=True)
  parser.add_argument("--config_bin_dir", type=str, help="base binary dir of the test suit (to locate the config templates)", required=True)

  return parser.parse_args()

def main(builtinParameters = {}):
  args = ParseOptions()
  basic_config = vars(args).copy()

  # Load the basic configuration.
  with open(os.path.join(args.config_bin_dir, 'basic_config.json'), 'r') as jsondata :

    basic_config.update(json.load(jsondata))

  print "Starting the Shang Integrated Tester in", args.mode, "mode..."

  # Initialize the gridengine
  s = drmaa.Session()
  s.initialize()
  active_jobs = []

  # Initialize the database connection
  con = sqlite3.connect(":memory:")
  con.cursor()

  # Create the tables for the experimental results.
  # We create 3 tables: HLS results, simulation results, and synthesis results
  con.executescript('''
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
  option_space = basic_config['option_space']
  option_space = [ dict(itertools.izip(option_space, opt))  for opt in itertools.product(*option_space.itervalues()) ]

  for test_path in args.tests.split() :
    basedir = os.path.dirname(test_path)
    test_file = os.path.basename(test_path)
    test_name = os.path.splitext(test_file)[0]

    test_option = random.choice(option_space)

    # TODO: Provide the keyword constructor
    hls_step = HLSStep(basic_config)
    hls_step.test_name = test_name
    hls_step.hardware_function = test_name if args.mode == TestStep.HybridSim else 'main'
    hls_step.test_file = test_path
    hls_step.fmax = test_option['fmax']

    hls_step.parameter = "f%(fmax)s" % test_option

    hls_step.prepareTest()
    hls_step.runTest(s)

    active_jobs.append(hls_step)

  fail_steps = []

  # Examinate the status of the jobs
  while active_jobs :
    next_active_jobs = []
    for job in active_jobs:
      status = s.jobStatus(job.jobid)
      if status == drmaa.JobState.DONE or status == drmaa.JobState.FAILED:
        retval = s.wait(job.jobid, drmaa.Session.TIMEOUT_WAIT_FOREVER)
        if not retval.hasExited or retval.exitStatus != 0 :
          print "Test", job.getStepDesc(), "FAIL"
          fail_steps.append(job.getStepDict())
          continue

        # Now the job finished successfully
        print "Test", job.getStepDesc(), "passed"
        job.submitResults(con)

        # Generate subtest.
        # FIXME: Only generate the subtest if the previous test passed.
        for subtest in job.generateSubTests() :
          subtest.prepareTest()
          subtest.runTest(s)
          next_active_jobs.append(subtest)

        continue

      next_active_jobs.append(job)

    time.sleep(5)
    active_jobs = next_active_jobs[:]
    print len(active_jobs), "tests left"
    sys.stdout.flush()

  # Wait untill all HLS jobs finish
  #s.synchronize([ drmaa.Session.JOB_IDS_SESSION_ALL ], drmaa.Session.TIMEOUT_WAIT_FOREVER)

  #  #print 'Job: ' + str(retval.jobId) + ' finished with status ' + str(retval.hasExited)

  # Finialize the gridengine
  s.exit()

  for line in con.iterdump():
    print line

  #cur = con.cursor()
  # Report the experimental results
  #cur.execute('''select sim.name, sim.cycles, syn.fmax, syn.les, syn.mult9, syn.parameter, sim.parameter
  #             from simulation sim, synthesis syn
  #             where sim.name == syn.name and syn.parameter like sim.parameter''')

  #for row in cur.fetchall() :
  #  print row

  json.dump(fail_steps,
            open(os.path.join(args.config_bin_dir, 'failcases.json'), 'w'),
            indent = 2)

if __name__=='__main__':
    main()
