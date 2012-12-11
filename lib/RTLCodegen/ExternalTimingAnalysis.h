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

namespace yaml {
  class MappingNode;
}

class TimingNetlist : public MFDatapathContainer {
  typedef std::map<VASTMachineOperand*, double> SrcInfoTy;
  typedef std::map<unsigned, SrcInfoTy> PathInfoTy;
  PathInfoTy PathInfo;

  // Create a path from Src to DstReg.
  void createDelayEntry(unsigned DstReg, VASTMachineOperand *Src);

  // Compute the delay to DstReg through SrcReg.
  void computeDelayFromSrc(unsigned DstReg, unsigned SrcReg);

  // FIXME: Move these function to another class.
  // Write the wrapper of the netlist.
  void writeNetlistWrapper(raw_ostream &O, const Twine &Name) const;

  VASTMachineOperand *getSrcPtr(unsigned Reg) const;
  VASTWire *getDstPtr(unsigned Reg) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, const Twine &Name,
                          const sys::Path &NetlistPath,
                          const sys::Path &TimingExtractionScript) const;

  void extractTimingForPair(raw_ostream &O, unsigned DstReg,
                            const VASTValue *Src) const;
  void extractTimingForPair(raw_ostream &O,
                            const VASTValue *Dst, unsigned DstReg,
                            const VASTValue *Src) const;

  void extractTimingToDst(raw_ostream &O, unsigned DstReg,
                          const SrcInfoTy &SrcInfo) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O, const Twine &Name,
                                   const sys::Path &ResultPath) const;
  // Read the JSON file written by the timing extraction script.
  bool readTimingAnalysisResult(const sys::Path &ResultPath);
  bool readPathDelay(yaml::MappingNode *N);
public:

  // Iterate over the source node reachable to DstReg.
  typedef SrcInfoTy::const_iterator src_iterator;
  src_iterator src_begin(unsigned DstReg) const {
    PathInfoTy::const_iterator at = PathInfo.find(DstReg);
    assert(at != PathInfo.end() && "DstReg not find!");
    return at->second.begin();
  }

  src_iterator src_end(unsigned DstReg) const {
    PathInfoTy::const_iterator at = PathInfo.find(DstReg);
    assert(at != PathInfo.end() && "DstReg not find!");
    return at->second.end();
  }

  bool src_empty(unsigned DstReg) const {
    return !PathInfo.count(DstReg);
  }

  TimingNetlist() {}

  void addInstrToDatapath(MachineInstr *MI);

  bool runExternalTimingAnalysis(const Twine &Name);
  void runInternalTimingAnalysis();
};
}

#endif
