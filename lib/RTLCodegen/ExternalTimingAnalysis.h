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

#include "MFDatapathContainer.h"

namespace llvm {
class MachineRegisterInfo;
class MachineInstr;
class raw_ostream;

namespace sys {
  class Path;
}

class TimingNetlist : public MFDatapathContainer {
  // Write the wrapper of the netlist.
  void writeNetlistWrapper(raw_ostream &O, const Twine &Name) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, const Twine &Name,
                          const sys::Path &NetlistPath,
                          const sys::Path &TimingExtractionScript) const;

  void extractTimingForPair(raw_ostream &O, const VASTNamedValue *From,
                            const VASTNamedValue *To, unsigned DstReg) const;

  void extractTimingForTree(raw_ostream &O, const VASTWire *W,
                            unsigned DstReg) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O, const Twine &Name) const;
public:
  TimingNetlist() {}

  void addInstrToDatapath(MachineInstr *MI);

  bool runExternalTimingAnalysis(const Twine &Name);
};
}

#endif
