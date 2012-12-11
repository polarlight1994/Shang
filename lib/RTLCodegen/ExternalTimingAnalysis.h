//=- ExternalTimingAnalysis.h - Timing Analysis By External Tools -*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the interface to enable timing analysis by external tools.
//
//===----------------------------------------------------------------------===//
#ifndef EXTERNAL_TIMING_ANALYSIS_H
#define EXTERNAL_TIMING_ANALYSIS_H

#include "TimingNetlist.h"

namespace llvm {
class MachineRegisterInfo;
class MachineInstr;
class raw_ostream;

namespace sys {
  class Path;
}

namespace yaml {
  class MappingNode;
}

class ExternalTimingAnalysis {
  TimingNetlist &TNL;
  typedef TimingNetlist::SrcInfoTy SrcInfoTy;
  typedef TimingNetlist::FaninIterator FaninIterator;
  typedef TimingNetlist::FanoutIterator FanoutIterator;

  // FIXME: Move these function to another class.
  // Write the wrapper of the netlist.
  void writeNetlistWrapper(raw_ostream &O) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, const sys::Path &NetlistPath,
                          const sys::Path &ExtractScript) const;

  void extractTimingForPair(raw_ostream &O, unsigned DstReg,
                            const VASTValue *Src) const;
  void extractTimingForPair(raw_ostream &O,
                            const VASTValue *Dst, unsigned DstReg,
                            const VASTValue *Src) const;

  void extractTimingToDst(raw_ostream &O, unsigned DstReg,
                          const SrcInfoTy &SrcInfo) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O,
                                   const sys::Path &ResultPath) const;
  // Read the JSON file written by the timing extraction script.
  bool readTimingAnalysisResult(const sys::Path &ResultPath);
  bool readPathDelay(yaml::MappingNode *N);

  explicit ExternalTimingAnalysis(TimingNetlist &TNL) : TNL(TNL) {}

  bool runExternalTimingAnalysis();
public:
  static bool runTimingAnalysis(TimingNetlist &TNL) {
    return ExternalTimingAnalysis(TNL).runExternalTimingAnalysis();
  }
};
}

#endif
