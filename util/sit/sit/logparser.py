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

    logfile = open(self.logpath,"r")
    for line in logfile:
      if ("incorrect!" in line) :
        correct = False
      elif ("correct!" in line) :
        correct = True

    if not correct :
      print self.logpath, '...Incorrect!'
      print logfile.read()

    logfile.closed
