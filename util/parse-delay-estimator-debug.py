#!/bin/python
import sys
import json

print "Parsing Delay-estimation statistics ..."
# Read the stdin for the data.
for line in sys.stdin:
  # Only handle the specific entires.
  if ("DELAY-ESTIMATION-VERIFY:" in line) :
    print line

  if ("DELAY-ESTIMATION-LONGEST-CHAIN-JSON:" in line) :
    print line
