#!/usr/bin/env python

"""
sit - Shang Integrated Tester.

"""

import math, os, platform, random, re, sys, time, threading, traceback

import drmaa
from jinja2 import Environment, FileSystemLoader, Template

from logparser import SimLogParser
from teststeps import HLSStep


def ParseOptions() :
  import argparse
  parser = argparse.ArgumentParser(description='The Shang Integrated Tester')
  parser.add_argument("--mode", type=str, choices=["trivial"], help="the mode of sit", required=True)
  parser.add_argument("--tests", type=str, help="tests to run", required=True)
  parser.add_argument("--tests_base", type=str, help="base dir of the test suit (to locate the config templates)", required=True)
  parser.add_argument("--ptr_size", type=int, help="pointer size in bits", required=True)
  parser.add_argument("--shang", type=str, help="path to shang executable", required=True)
  parser.add_argument("--llc", type=str, help="path to llc executable")
  parser.add_argument("--lli", type=str, help="path to lli executable")
  parser.add_argument("--verilator", type=str, help="path to verilator executable")
  parser.add_argument("--systemc", type=str, help="path to systemc folder")

  return parser.parse_args()

def buildHLSConfig(test_name, dst_dir_base, test_config, template_env) :
  # Fork the cofiguration
  local_config = test_config.copy()

  #for period in range(5, 21) :
  #TODO: Scan the fmax.
  local_config['fmax'] = 100.0 #1000.0 / float(period)

  # Create the local folder for the current test.
  from datetime import datetime
  dst_dir = os.path.join(dst_dir_base, test_name, datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
  os.makedirs(dst_dir)
  #print "Created folder: ", dst_dir

  local_config['test_binary_root'] = dst_dir

  yield local_config

def runHLS(session, shang, hls_config) :
  # Create the HLS job.
  jt = session.createJobTemplate()
  jt.remoteCommand = 'timeout'
  jt.args = ['60s', shang, hls_config]
  #Set up the correct working directory and the output path
  jt.workingDirectory = os.path.dirname(hls_config)
  jt.outputPath = ':' + os.path.join(os.path.dirname(hls_config), 'hls.output')
  jt.joinFiles=True
  #Set up the environment variables
  #jt.env = ...

  jobid = session.runJob(jt)
  session.deleteJobTemplate(jt)

  return jobid

def runHybridSimulation(session, hls_base, hls_jid, hls_config, template_env) :
  # Create the hybrid simulation job.
  jt = session.createJobTemplate()
  workingDirectory = os.path.join(hls_base, 'hybrid_sim')
  os.makedirs(workingDirectory)

  #Generate the simulate script
  hls_config['bybrid_sim_root'] = workingDirectory
  sim_script = os.path.join(workingDirectory, 'hybrid_sim.sge')
  template_env.get_template('hybrid_sim.sge.in').stream(hls_config).dump(sim_script)

  jt.remoteCommand = 'bash'
  jt.args = [ sim_script ]
  #Set up the correct working directory and the output path
  jt.workingDirectory = workingDirectory
  hybrid_sim_log = os.path.join(workingDirectory, 'hybrid_sim.output')
  jt.outputPath = ':' + hybrid_sim_log
  jt.joinFiles=True
  # Wait untill HLS finished.
  jt.nativeSpecification = "-hold_jid %s" % hls_jid
  #Set up the environment variables
  jt.environment = {'VERILATOR_ROOT': os.path.dirname(os.path.dirname(hls_config['verilator'])) }

  jobid = session.runJob(jt)
  session.deleteJobTemplate(jt)

  #TODO: Wait until the simulation finish and parse the output.
  return SimLogParser(hls_config['test_name'], hybrid_sim_log, jobid)



def main(builtinParameters = {}):
  args = ParseOptions()

  print "Starting the Shang Integrated Tester in", args.mode, "mode..."

  # Get the synthesis configuration templates.
  env = Environment(loader=FileSystemLoader(args.tests_base))
  env.filters['joinpath'] = lambda list: os.path.join(*list)

  # Initialize the gridengine
  s = drmaa.Session()
  s.initialize()
  active_jobs = []


  #Global dict for the common configurations
  global_config = { #"test_name": test_name,
                    #"hardware_function": test_name,
                    #"test_file" : test_path,
                    "config_dir" : args.tests_base,
                    "ptr_size" : args.ptr_size,
                    "llc" : args.llc,
                    "lli" : args.lli,
                    "shang" : args.shang,
                    "verilator" : args.verilator,
                    "systemc" : args.systemc }

  for test_path in args.tests.split() :
    basedir = os.path.dirname(test_path)
    test_file = os.path.basename(test_path)
    test_name = os.path.splitext(test_file)[0]

    hls_config = global_config.copy()

    # TODO: Provide the keyword constructor
    hls_step = HLSStep(hls_config)
    hls_step.test_name = test_name
    hls_step.hardware_function = test_name
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
        else :
          print "Test", job.test_name, "passed"

        job.dumplog()

        # Generate subtest.
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
