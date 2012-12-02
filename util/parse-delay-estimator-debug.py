#!/bin/python
import sys
import json

print "Parsing Delay-estimation statistics ..."
# Read the stdin for the data.
for line in sys.stdin:
  # Only handle the specific entires.
  if (not line.startswith("DELAY-ESTIMATION-DEBUG:")) :
    continue

  if not "0xb94aa58" in line : continue

  print line
