#!/bin/python
import sys
import json

print "Parsing Delay-estimation statistics ..."
# Read the stdin for the data.
for line in sys.stdin:
  # Only handle the specific entires.
  if (not "DELAY-ESTIMATOR-JSON:" in line) :
    continue
  # print line
  chainDelay = json.loads(line[len("DELAY-ESTIMATOR-JSON:"):])
  error = chainDelay["BLACKBOX"] - chainDelay["ACCURATE"]
  error_rate = error / chainDelay["ACCURATE"]
  print line, error, ((error_rate) * 100), '%'
