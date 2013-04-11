#!/usr/bin/env python

"""
sit - Shang Integrated Tester.

"""

import math, os, platform, random, re, sys, time, threading, traceback

def ParseOptions() :
  import argparse
  parser = argparse.ArgumentParser(description='The Shang Integrated Tester')
  parser.add_argument("--mode", type=str, choices=["trivial"], help="the mode of sit", required=True)
  parser.add_argument("--tests", type=str, help="tests to run", required=True)
  parser.add_argument("--tests_base", type=str, help="base dir of the test suit (to locate the config templates)", required=True)
  #parser.add_argument("--configs", type=str, help="config files", required=True)

  return parser.parse_args()

def loadConfig(config_dir, dst_dir, test_config) :
  from jinja2 import Environment, FileSystemLoader

  env = Environment(loader=FileSystemLoader(config_dir))

  env.filters['joinpath'] = lambda list: os.path.join(*list)

  template= env.get_template('test_config.lua.in')
  print template.render(test_config)

def main(builtinParameters = {}):
  args = ParseOptions()

  print "Starting the Shang Integrated Tester in", args.mode, "mode..."

  for test_path in args.tests.split() :
    basedir = os.path.dirname(test_path)
    test_file = os.path.basename(test_path)
    test_name = os.path.splitext(test_file)[0]
    print "Running", args.mode, "test in", basedir, "for", test_name

    #Global dict for the common configurations
    test_config = { "SYN_FUNC": test_name,
                    "config_dir" : args.tests_base}

    #Generate the synthesis configuration
    loadConfig(args.tests_base, basedir, test_config.copy())

if __name__=='__main__':
    main()
