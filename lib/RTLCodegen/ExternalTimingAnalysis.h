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
  void writeNetlistWrapper(raw_ostream &O, const Twine &Name);

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, const Twine &Name,
                          const sys::Path &NetlistPath);
public:
  TimingNetlist() {}

  void addInstrToDatapath(MachineInstr *MI);

  void runExternalTimingAnalysis(const Twine &Name);
};
}

#endif
