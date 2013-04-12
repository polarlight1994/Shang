#!/usr/bin/env python

class LogParserBase :
  logpath = ''
  jobid = 0

  def __init__(self, logpath, jobid) :
    self.logpath = logpath
    self.jobid = jobid

class SimLogParser(LogParserBase):

  def __init__(self, logpath, jobid) :
    LogParserBase.__init__(self, logpath, jobid)

  def parse(self) :
    correct = False

    print 'Parsing', self.logpath
    with open(self.logpath,"r") as logfile:
      for line in logfile:
        if ("incorrect!" in line) :
          correct = False
        elif ("correct!" in line) :
          correct = True
    logfile.closed

    if correct :
      print 'Correct!'
