#!/usr/bin/env python

import sqlite3
import urllib2
import re
from difflib import SequenceMatcher



def generate_opcode_filter(OpcodesToCompare = []) :
  if not OpcodesToCompare:
    return 'null is null'

  return "Opcode in (%s)" % ', '.join([ '"%s"' % s for s in OpcodesToCompare])

class BugLocator:
  def __init__(self, LHSDBScript, RHSDBScript) :
    #Initialize the database connections
    self.lhs_conn = sqlite3.connect(':memory:')
    self.lhs_conn.executescript(urllib2.urlopen(LHSDBScript).read())
    self.lhs_conn.commit()
    self.rhs_conn = sqlite3.connect(':memory:')
    self.rhs_conn.executescript(urllib2.urlopen(RHSDBScript).read())
    self.rhs_conn.commit()


  def generateInstructionTrace(self, conn, Filter) :
    cur = conn.cursor()

    query = ('''select Instruction, OperandValue, RegisterName,
                       BB, SlotNum, ActiveTime
                from InstTrace where (%s)
                order by ActiveTime ASC, RegisterName ASC, Instruction ASC''' %
             Filter)
    rows = cur.execute(query).fetchall()
    return zip(*[((OperandValue, RegisterName), Instruction, BB, SlotNum, ActiveTime)
                 for Instruction, OperandValue, RegisterName, BB, SlotNum, ActiveTime in rows ])


  def compareInstructionTrace(self, Filter) :
    LHSTrace = self.generateInstructionTrace(self.lhs_conn, Filter)
    RHSTrace = self.generateInstructionTrace(self.rhs_conn, Filter)
    return (LHSTrace, RHSTrace,
            SequenceMatcher(isjunk = None, a = LHSTrace[0], b = RHSTrace[0],
                            autojunk = False))

  def recusivelyLocateBug(self, Filter) :
    LHSTrace, RHSTrace, SMatcher = BL.compareInstructionTrace(Filter)
    for opcode, i1, i2, j1, j2 in SMatcher.get_opcodes() :
      print ("%7s lhs[%d:%d) rhs[%d:%d)" % (opcode, i1, i2, j1, j2))

      if   opcode == 'equal' :
        self.handleEqual(zip(*[ l[i1:i2] for l in LHSTrace ]), (zip(*[ l[j1:j2] for l in RHSTrace ])))
      elif opcode == 'delete' :
        self.handleDelete(zip(*[ l[i1:i2] for l in LHSTrace ]), [ l[j1 - 1] for l in RHSTrace ])
      elif opcode == 'replace':
        self.handelReplace(zip(*[ l[i1:i2] for l in LHSTrace ]), (zip(*[ l[j1:j2] for l in RHSTrace ])))

  # From the Document of SequenceMatcher
  # 'equal'	a[i1:i2] == b[j1:j2] (the sub-sequences are equal).
  def handleEqual(self, LHSTrace, RHSTrace) :
    for LHS, RHS in zip(LHSTrace, RHSTrace):
      print '<', LHS
      print '>', RHS

  # From the Document of SequenceMatcher
  # 'delete'	lhs[i1:i2] should be deleted. Note that j1 == j2 in this case.
  # LastElement is the last ElementBefore delete
  def handleDelete(self, DeletedList, RHSBeforeDelete) :
    print ' ', RHSBeforeDelete
    for d in DeletedList :
      print '-', d

    if len(DeletedList) == 1 :
      return

    #Try to find out what happen in the BB before the behavior become different.
    BB = RHSBeforeDelete[2]
    print 'comparing bb', BB
    filter = " BB = '%s' " % BB
    self.recusivelyLocateBug(filter)

  # From the Document of SequenceMatcher
  # 'replace' lhs[i1:i2] should be replaced by rhs[j1:j2].
  def handelReplace(self, LHSTrace, RHSTrace) :
    for LHS in LHSTrace :
      print '-', LHS

    for RHS in RHSTrace :
      print '+', RHS

if __name__ == "__main__" :
  CorrectTrace = r'''file:///E:/buildbot/shang-slave/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131030-131814-171052/pure_hw_sim/trace_database.sql'''
  BadTrace = r'''file:///E:/buildbot/shang-slave-43/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131030-121730-295299/pure_hw_sim/trace_database.sql'''

  # First of all, compare the trace of PHI instructions.
  BL = BugLocator(CorrectTrace, BadTrace)
  BL.recusivelyLocateBug(generate_opcode_filter(['phi']))
  print 'a'
