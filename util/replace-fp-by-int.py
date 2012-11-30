#!/bin/python
import sys
import re
from optparse import OptionParser
from random import randint

parser = OptionParser()
parser.add_option("-s", "--source_file", dest="src",
                  help="Source file to perfrom the replacement", metavar="FILE")
                  

(options, args) = parser.parse_args()

def repl_fp(matchobj) :
  return str(randint(1, 65536))

#Open the file.
with open(options.src, "r") as src:
  # read the file line by line.
  for line in src :
    # Replace the floating point constaints by random integer.
    line = re.sub(r'([-+]?[0-9]*\.[0-9]+([E]?[-+]?[0-9]+)?)', repl_fp, line)
    sys.stdout.write(line)

src.closed
