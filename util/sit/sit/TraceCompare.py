#!/usr/bin/env python

import sqlite3
import urllib2
import re
from difflib import Differ

def generateInstructionTrace(DBScript, OpcodesToCompare = []) :
  conn = sqlite3.connect(':memory:')
  f = urllib2.urlopen(DBScript)
  for l in f.readlines() :
    #print l
    conn.executescript(l)
  conn.commit()

  cur = conn.cursor()

  query = '''select Instruction, OperandValue, RegisterName
            from InstTrace where Opcode in (%s)
            order by ActiveTime ASC, RegisterName ASC, Instruction ASC''' % \
          ', '.join([ '"%s"' % s for s in OpcodesToCompare])
  rows = cur.execute(query).fetchall()
  return rows

def compareInstructionTrace(LHSDBScript, RHSDBScript, OpcodesToCompare = []) :
  LHSTrace = generateInstructionTrace(LHSDBScript, OpcodesToCompare)
  RHSTrace = generateInstructionTrace(RHSDBScript, OpcodesToCompare)
  for d in Differ().compare(LHSTrace, RHSTrace) :
    print d

if __name__ == "__main__" :
  CorrectTrace = r'''file:///E:/buildbot/shang-slave-43/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131029-163742-490778/pure_hw_sim/trace_database.sql'''
  BadTrace = r'''file:///E:/buildbot/shang-slave-41/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131029-164541-273147/pure_hw_sim/trace_database.sql'''

  compareInstructionTrace(CorrectTrace, BadTrace, ['phi'])
