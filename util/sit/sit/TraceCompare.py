#!/usr/bin/env python

import sqlite3
import urllib2
import re
from difflib import SequenceMatcher

def generateInstructionTrace(DBScript, OpcodesToCompare = []) :
  conn = sqlite3.connect(':memory:')
  conn.executescript(urllib2.urlopen(DBScript).read())
  conn.commit()

  cur = conn.cursor()

  query = ('''select Instruction, OperandValue, RegisterName,
                     BB, SlotNum
            from InstTrace where Opcode in (%s)
            order by ActiveTime ASC, RegisterName ASC, Instruction ASC''' %
          ', '.join([ '"%s"' % s for s in OpcodesToCompare]))
  rows = cur.execute(query).fetchall()
  return zip(*[((OperandValue, RegisterName), (Instruction, BB, SlotNum)) for Instruction, OperandValue, RegisterName, BB, SlotNum in rows ])

def compareInstructionTrace(LHSDBScript, RHSDBScript, OpcodesToCompare = []) :
  LHSTrace = generateInstructionTrace(LHSDBScript, OpcodesToCompare)
  RHSTrace = generateInstructionTrace(RHSDBScript, OpcodesToCompare)
  s = SequenceMatcher(isjunk = None, a = LHSTrace[0], b = RHSTrace[0], autojunk = False)

  for tag, i1, i2, j1, j2 in s.get_opcodes():
    print ("%7s a[%d:%d] b[%d:%d]" % (tag, i1, i2, j1, j2))

if __name__ == "__main__" :
  CorrectTrace = r'''file:///E:/buildbot/shang-slave-43/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131029-163742-490778/pure_hw_sim/trace_database.sql'''
  BadTrace = r'''file:///E:/buildbot/shang-slave-41/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131029-164541-273147/pure_hw_sim/trace_database.sql'''

  compareInstructionTrace(CorrectTrace, BadTrace, ['phi'])
