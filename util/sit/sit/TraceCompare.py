#!/usr/bin/env python

import sqlite3
import urllib2
import re
from difflib import SequenceMatcher
from os import path

def generate_bb_filter(bbs = []) :
  if not bbs:
    return 'null is null'

  return "BB in (%s)" % ', '.join([ '"%s"' % s for s in bbs])

def generate_register_filter(Registers = []) :
  if not Registers:
    return 'null is null'

  return "RegisterName in (%s)" % ', '.join([ '"%s"' % s for s in Registers])

def generate_opcode_filter(Opcodes = []) :
  if not Opcodes:
    return 'null is null'

  return "Opcode in (%s)" % ', '.join([ '"%s"' % s for s in Opcodes])

def generate_register_value_filter(Values = []) :
  if not Values:
    return 'null is null'

  return "OperandValue in (%s)" % ', '.join([ '%s' % s for s in Values])

def build_reg_assign_matcher(LHSTrace, RHSTrace) :
  return SequenceMatcher(isjunk = None, a = LHSTrace[0], b = RHSTrace[0],
                         autojunk = False)

def build_instruction_matcher(LHSTrace, RHSTrace) :
  return SequenceMatcher(isjunk = None, a = LHSTrace[1], b = RHSTrace[1],
                         autojunk = False)

class TraceAnalyzer:
  def __init__(self, LHSDBScript, RHSDBScript = None, MatcherBuilder = None) :
    #Initialize the database connections
    self.lhs_conn = sqlite3.connect(':memory:')
    self.lhs_conn.executescript(urllib2.urlopen(LHSDBScript).read())
    self.lhs_conn.commit()

    if RHSDBScript :
      self.rhs_conn = sqlite3.connect(':memory:')
      self.rhs_conn.executescript(urllib2.urlopen(RHSDBScript).read())
      self.rhs_conn.commit()

    if MatcherBuilder :
      self.MatcherBuilder = MatcherBuilder

  def generateInstructionTrace(self, conn, Filter) :
    cur = conn.cursor()

    query = ('''select Instruction, OperandValue, RegisterName,
                       BB, SlotNum, ActiveTime
                from InstTrace where (%s)
                order by ActiveTime ASC, Instruction ASC, RegisterName ASC''' %
             Filter)
    rows = cur.execute(query).fetchall()
    return zip(*[((OperandValue, RegisterName), Instruction, BB, SlotNum, ActiveTime)
                 for Instruction, OperandValue, RegisterName, BB, SlotNum, ActiveTime in rows ])

  def generateMemoryLocationTrace(self, conn, AddrFilter, DataFilter) :
    cur = conn.cursor()

    select_addr = '(select * from InstTrace where (%s)) as Addr' % AddrFilter
    select_data = '(select * from InstTrace where (%s)) as Data' % DataFilter

    query = ('''select Addr.Instruction, Addr.OperandValue, Data.OperandValue,
                       Addr.RegisterName, Data.RegisterName,
                       Addr.BB, Addr.SlotNum, Addr.ActiveTime
                  from %s left join %s
                    on Addr.ActiveTime == Data.ActiveTime
                  order by Addr.ActiveTime ASC, Addr.Instruction ASC,
                           Addr.RegisterName ASC, Data.RegisterName ASC'''
             % (select_addr, select_data))
    rows = cur.execute(query).fetchall()
    return zip(*[((Addr, Data), Instruction, BB, SlotNum, ActiveTime)
                 for Instruction, Addr, Data, AddrName, DataName, BB, SlotNum, ActiveTime in rows ])

  def buildTraceMatcher(self, TraceBuilder) :
    LHSTrace = TraceBuilder(self.lhs_conn)
    RHSTrace = TraceBuilder(self.rhs_conn)
    return (LHSTrace, RHSTrace, self.MatcherBuilder(LHSTrace, RHSTrace))

  def printTraceDifferent(self, TraceBuilder) :
    LHSTrace, RHSTrace, SMatcher = self.buildTraceMatcher(TraceBuilder)
    for opcode, i1, i2, j1, j2 in SMatcher.get_opcodes() :
      print ("%7s lhs[%d:%d) rhs[%d:%d)" % (opcode, i1, i2, j1, j2))

      if   opcode == 'equal' :
        self.printEqual(zip(*[ l[i1:i2] for l in LHSTrace ]), (zip(*[ l[j1:j2] for l in RHSTrace ])))
      elif opcode == 'insert' :
        self.printInsert(zip(*[ l[j1:j2] for l in RHSTrace ]))
      elif opcode == 'delete' :
        self.printDelete(zip(*[ l[i1:i2] for l in LHSTrace ]))
      elif opcode == 'replace':
        self.handelReplace(zip(*[ l[i1:i2] for l in LHSTrace ]), (zip(*[ l[j1:j2] for l in RHSTrace ])))


  def printInstructionTraceDifferent(self, Filter) :
    self.printTraceDifferent(lambda conn : self.generateInstructionTrace(conn, Filter))

  def printMemoryLocationTraceDifferent(self, AddrFilter, DataFilter) :
    self.printTraceDifferent(lambda conn : self.generateMemoryLocationTrace(conn, AddrFilter, DataFilter))

  # From the Document of SequenceMatcher
  # 'equal'	a[i1:i2] == b[j1:j2] (the sub-sequences are equal).
  def printEqual(self, LHSTrace, RHSTrace) :
    for LHS, RHS in zip(LHSTrace, RHSTrace):
      print '<', LHS
      print '>', RHS

  # From the Document of SequenceMatcher
  # 'delete'	lhs[i1:i2] should be deleted. Note that j1 == j2 in this case.
  # LastElement is the last ElementBefore delete
  def printDelete(self, DeletedList) :
    for d in DeletedList :
      print '-', d

  # From the Document of SequenceMatcher
  # 'replace' lhs[i1:i2] should be replaced by rhs[j1:j2].
  def handelReplace(self, LHSTrace, RHSTrace) :
    for LHS in LHSTrace :
      print '-', LHS

    for RHS in RHSTrace :
      print '+', RHS
      
  # From the Document of SequenceMatcher
  # 'insert'  rhs[j1:j2] should be inserted at lhs[i1:i1]. Note that i1 == i2 in this case.
  def printInsert(self, InsertedList) :
    for d in InsertedList :
      print '+', d

  def generateSlotTraceInternal(self, conn, Filter = 'null is null') :
    cur = conn.cursor()

    query = ('''select BB, SlotNum, ActiveTime
                  from SlotTrace where (%s)
                  order by ActiveTime ASC, BB ASC, SlotNum ASC''' %
             Filter)
    rows = cur.execute(query).fetchall()
    return rows

  def generateSlotTrace(self, Filter = 'null is null') :
    return self.generateSlotTraceInternal(self.lhs_conn, Filter)

  def generateBBCyclesInternal(self, conn, Filter = 'null is null') :
    cur = conn.cursor()

    query = ('''select count(ActiveTime), BB
                  from SlotTrace where (%s)
                  group by BB
                  order by 1 DESC''' %
             Filter)
    rows = cur.execute(query).fetchall()
    return rows

  
  def generateBBCycles(self, Filter = 'null is null') :
    return self.generateBBCyclesInternal(self.lhs_conn, Filter)


def analyze_benchmarks_execution_trace(dburl) :
  #Initialize the database connections
  conn = sqlite3.connect(':memory:')
  conn.executescript(urllib2.urlopen(dburl).read())
  conn.commit()

  cur = conn.cursor()

  for name, parameter, cycles in cur.execute('''select name, parameter, cycles from simulation''').fetchall() :
    print name, cycles

    for rtl_output, in cur.execute('''select rtl_output from highlevelsynthesis where  name = '%s' and  parameter = '%s' ''' % (name, parameter)).fetchall() :
      rtl_output = rtl_output.replace('''/nfs/home/hongbin.zheng''', '''file:///E:''').replace('/', '\\')
      parent_dir = path.dirname(rtl_output)
      parent_dir = path.join(parent_dir, 'pure_hw_sim')
      sql_path = path.join(parent_dir, 'trace_database.sql')
      
      TA = TraceAnalyzer(sql_path)
      for s in TA.generateBBCycles() :
        print s, (float(s[0]) / float(cycles) * 100) 




if __name__ == "__main__" :
  #CorrectTrace = r'''file:///E:/buildbot/shang-slave/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131030-131814-171052/pure_hw_sim/trace_database.sql'''
  #BadTrace = r'''file:///E:/buildbot/shang-slave-43/LongTerm/build/shang-build/tools/shang/testsuite/benchmark/legup_chstone/aes/20131030-121730-295299/pure_hw_sim/trace_database.sql'''

  ## First of all, compare the trace of PHI instructions.
  #TA = TraceAnalyzer(CorrectTrace, BadTrace, build_reg_assign_matcher)
  #TA.printInstructionTraceDifferent(generate_opcode_filter(['phi']))
  #TA.printInstructionTraceDifferent(generate_bb_filter(['.lr.ph.i']))

  #TA = TraceAnalyzer(CorrectTrace, BadTrace, build_instruction_matcher)
  #TA.printMemoryLocationTraceDifferent('%s and %s' % (generate_register_filter(['mem6p0addr', 'mem6p1addr']),
  #                                      generate_register_value_filter([0])),
  #                                     generate_register_filter(['mem6p0wdata', 'mem6p1wdata']))

  #SlotTrace = r'''file:///E:\buildbot\shang-slave-41\LongTerm\build\shang-build\tools\shang\testsuite\benchmark\legup_chstone\aes\20131031-104625-492307\pure_hw_sim\trace_database.sql'''
  #TA = TraceAnalyzer(SlotTrace)
  #for s in TA.generateBBCycles() :
  #  print s

  SimulationDB = r'''http://192.168.1.253:8010/builders/LongTerm/builds/857/steps/custom%20target/logs/data/text'''
  analyze_benchmarks_execution_trace(SimulationDB)
  print 'a'


#if len(DeletedList) == 1 :
#  return

##Try to find out what happen in the BB before the behavior become different.
#BB = DeletedList[0][2]
#print 'comparing bb', BB
#filter = " BB = '%s' " % BB
#self.printInstructionTraceDifferent(filter)
