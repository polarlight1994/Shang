#!/bin/python
import sys
import json

#period = 10
resolution = 19

max_distribution = {}
mim_distribution = {}
num_paths = 0;
total_max_delay = 0.0

maxlb = 0

print "Parsing Delay-estimation statistics ..."
# Read the stdin for the data.
for line in sys.stdin:
  # Only handle the specific entires.
  if (not line.startswith("DELAY-ESTIMATION-JSON:")) :
    continue
  # chainDelay should lools like this:
  #{u'SRC': u'0xad2f41c', u'DST': u'0xb68373c', u'LSB': 0.082799999999999999, u'MSB': 0.082799999999999999, u'MAX': 0.082799999999999999}
  chainDelay = json.loads(line[len("DELAY-ESTIMATION-JSON:"):])

  # Get the slowest bit delay.
  total_max_delay = total_max_delay + chainDelay["MAX"];

  max_delay = int(chainDelay["MAX"])
  range_lb = max_delay / resolution;
  max_distribution[range_lb] =  max_distribution.get(range_lb, 0) + 1
  maxlb = max(range_lb, maxlb)

  # Get the fastest bit delay.
  min_delay = int(min(chainDelay["MSB"], chainDelay["LSB"]))
  range_lb = min_delay / resolution;
  mim_distribution[range_lb] =  mim_distribution.get(range_lb, 0) + 1
  maxlb = max(range_lb, maxlb)

  print "Built %d entires: max %d min %d" % (maxlb, max_delay, min_delay)
  num_paths = num_paths + 1
#print distribution

print "Built %d entires" % maxlb

for lb in range(0, maxlb + 1):
  max_count = max_distribution.get(lb, 0)
  min_count = mim_distribution.get(lb, 0)
  print "[%dll,%dll)\t%d\t%f\t%d\t%f" % (lb * resolution, (lb + 1) *  resolution, max_count, float(max_count)/float(num_paths), min_count, float(min_count)/float(num_paths))

print "NumPath\t%d\tTotalMaxDelay\t%d" %(num_paths, total_max_delay)