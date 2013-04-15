#!/usr/bin/env python

"""
sit - Shang Integrated Tester.

"""

import math, os, platform, random, re, sys, time, threading, traceback

import drmaa
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
    from json import load
    basic_config.update(load(jsondata))

  print "Starting the Shang Integrated Tester in", args.mode, "mode..."

  # Initialize the gridengine
  s = drmaa.Session()
  s.initialize()
  active_jobs = []

  for test_path in args.tests.split() :
    basedir = os.path.dirname(test_path)
    test_file = os.path.basename(test_path)
    test_name = os.path.splitext(test_file)[0]

    # TODO: Provide the keyword constructor
    hls_step = HLSStep(basic_config)
    hls_step.test_name = test_name
    hls_step.hardware_function = test_name if args.mode == TestStep.HybridSim else 'main'
    hls_step.test_file = test_path
    hls_step.fmax = 100.0

    hls_step.prepareTest()
    hls_step.runTest(s)

    active_jobs.append(hls_step)


  # Examinate the status of the jobs
  while active_jobs :
    next_active_jobs = []
    for job in active_jobs:
      status = s.jobStatus(job.jobid)
      if status == drmaa.JobState.DONE or status == drmaa.JobState.FAILED:
        retval = s.wait(job.jobid, drmaa.Session.TIMEOUT_WAIT_FOREVER)
        if not retval.hasExited or retval.exitStatus != 0 :
          print "Test", job.test_name, "FAIL"
          job.dumplog()
        else :
          print "Test", job.test_name, "passed"
          job.parseResults()

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

if __name__=='__main__':
    main()
