#!/usr/bin/env python

"""
sit - Shang Integrated Tester.

"""

import math, os, platform, random, re, sys, time, threading, traceback

def ParseOptions() :
  import argparse
  parser = argparse.ArgumentParser(description='The Shang Integrated Tester')
  parser.add_argument("-m", "--mode", type=str, choices=["trivial"], help="the mode of sit", required=True)
  parser.add_argument("--tests", type=str, help="tests to run", required=True)
  #parser.add_argument("--configs", type=str, help="config files", required=True)

  return parser.parse_args()

def loadConfig(configs, dst_dir) :
  print "Load config"

def main(builtinParameters = {}):
  args = ParseOptions()

  print "Starting the Shang Integrated Tester in", args.mode, "mode..."

  for test_path in args.tests.split() :
    basedir = os.path.dirname(test_path)
    test_file = os.path.basename(test_path)
    print "Running", args.mode, "test in", basedir
    #Generate the synthesis configuration

if __name__=='__main__':
    main()
