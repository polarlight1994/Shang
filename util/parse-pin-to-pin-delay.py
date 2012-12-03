#!/bin/python
import sys
import json
import re
from xlwt import Workbook

def extract_number(line) :
    return int(re.search(r"\d+",line).group())

print "Parsing pin-to-pin delays ..."

DelayMatrix = Workbook()
Matrix = DelayMatrix.add_sheet('Matrix')

# Read the stdin for the data.
line = sys.stdin.readline()

while line :
  # Only handle the specific entires.
  if ("FU-DELAY-CHARACTERIZE" in line) :
    print line
    print extract_number(line)
    FromPin = extract_number(line)

    line = sys.stdin.readline()
    print line
    print extract_number(line)
    ToPin = extract_number(line)

    line = sys.stdin.readline()
    print line
    print extract_number(line)
    LL = extract_number(line)

    Matrix.write(FromPin, ToPin, LL) 
    
  line = sys.stdin.readline()
  
DelayMatrix.save("X.xls")
