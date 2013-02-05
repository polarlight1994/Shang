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

#include "TimingEstimator.h"

namespace llvm {
class VASTModule;
class raw_ostream;

namespace sys {
  class Path;
}

namespace yaml {
  class MappingNode;
}

class ExternalTimingAnalysis : public TimingEstimatorBase {
  VASTModule &VM;

  // FIXME: Move these function to another class.
  // Write the wrapper of the netlist.
  void writeNetlist(raw_ostream &O) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, const sys::Path &NetlistPath,
                          const sys::Path &ExtractScript) const;

  void extractTimingForPair(raw_ostream &O, const VASTSeqValue *Dst,
                            const VASTSeqValue *Src) const;

  void extractTimingToDst(raw_ostream &O, const VASTSeqValue *Dst) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O,
                                   const sys::Path &ResultPath) const;
  // Read the JSON file written by the timing extraction script.
  bool readTimingAnalysisResult(const sys::Path &ResultPath);
  bool readPathDelay(yaml::MappingNode *N);

public:
  explicit ExternalTimingAnalysis(VASTModule &VM) : VM(VM) {}

  bool runExternalTimingAnalysis();
};
}

#endif