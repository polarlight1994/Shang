#!/usr/bin/env python

"""
sit - Shang Integrated Tester.

"""

import math, os, platform, random, re, sys, time, threading, traceback
import drmaa

def ParseOptions() :
  import argparse
  parser = argparse.ArgumentParser(description='The Shang Integrated Tester')
  parser.add_argument("--mode", type=str, choices=["trivial"], help="the mode of sit", required=True)
  parser.add_argument("--tests", type=str, help="tests to run", required=True)
  parser.add_argument("--tests_base", type=str, help="base dir of the test suit (to locate the config templates)", required=True)
  parser.add_argument("--ptr_size", type=int, help="pointer size in bits", required=True)
  parser.add_argument("--shang", type=str, help="path to shang executable", required=True)

  return parser.parse_args()

def generateHLSConfig(test_name, dst_dir_base, test_config, template_env) :
  # Create the local folder for the current test.
  from datetime import datetime
  dst_dir = os.path.join(dst_dir_base, test_name, datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
  os.makedirs(dst_dir)

  local_config = test_config.copy()
  local_config['test_binary_root'] = dst_dir
  #TODO: Scan the fmax.
  local_config['fmax'] = 100.0

  synthesis_config = os.path.join(dst_dir, 'test_config.lua')
  template_env.get_template('test_config.lua.in').stream(local_config).dump(synthesis_config)
  yield synthesis_config

def runHLS(session, shang, hls_config) :
  # Create the HLS job.
  jt = session.createJobTemplate()
  jt.remoteCommand = shang
  jt.args = [hls_config]
  jt.joinFiles=True

  jobid = session.runJob(jt)
  session.deleteJobTemplate(jt)

  return jobid

def main(builtinParameters = {}):
  args = ParseOptions()

  print "Starting the Shang Integrated Tester in", args.mode, "mode..."

  # Get the synthesis configuration templates.
  from jinja2 import Environment, FileSystemLoader

  env = Environment(loader=FileSystemLoader(args.tests_base))
  env.filters['joinpath'] = lambda list: os.path.join(*list)

  # Initialize the gridengine
  s = drmaa.Session()
  s.initialize()
  hls_jobs = []

  for test_path in args.tests.split() :
    basedir = os.path.dirname(test_path)
    test_file = os.path.basename(test_path)
    test_name = os.path.splitext(test_file)[0]
    print "Running", args.mode, "test in", basedir, "for", test_name

    #Global dict for the common configurations
    test_config = { "hardware_function": test_name,
                    "test_file" : test_path,
                    "config_dir" : args.tests_base,
                    "ptr_size" : args.ptr_size}

    #Generate the synthesis configuration and run the test
    for hls_config in generateHLSConfig(test_name, basedir, test_config, env) :
      hls_jobs.append(runHLS(s, args.shang, hls_config))

  # Wait untill all HLS jobs finish
  s.synchronize(hls_jobs, drmaa.Session.TIMEOUT_WAIT_FOREVER)
  for curjob in hls_jobs:
    print 'Collecting job ' + curjob
    retval = s.wait(curjob, drmaa.Session.TIMEOUT_WAIT_FOREVER)
    print 'Job: ' + str(retval.jobId) + ' finished with status ' + str(retval.hasExited)

  # Finialize the gridengine
  s.exit()

if __name__=='__main__':
    main()
