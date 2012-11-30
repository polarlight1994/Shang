#!/bin/python
from optparse import OptionParser
from random import randint
import sys

parser = OptionParser()
parser.add_option("-n", dest="n", type="int",
                  help="The size of the array to be generated", metavar="INT")
parser.add_option("-u", dest="ub", type="int", default=65536,
                  help="The upper bound of the values in the array")        
parser.add_option("-l", dest="lb", type="int", default=0,
                  help="The lower bound of the values in the array")            

(options, args) = parser.parse_args()

ub = options.ub
lb = options.lb

sys.stdout.write('{')
for x in range(0, options.n - 1):
  sys.stdout.write("%d, " % randint(lb, ub))
 
sys.stdout.write(str(randint(lb, ub)))
sys.stdout.write('};\n')
