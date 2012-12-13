//=----- TimingNetlist.h - The Netlist for Delay Estimation -------*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the timing netlist, which enable performing delay estimation
// on the RTL netlist. Please note that with this interface, we can perform
// timing estimation/analysis before and after scheduling/FU binding.
//
//===----------------------------------------------------------------------===//
#ifndef TIMING_NETLIST_H
#define TIMING_NETLIST_H

#include "MFDatapathContainer.h"

namespace llvm {
class MachineRegisterInfo;
// FIXME: Dont depends on MFDatapathContainer, otherwise we can only perform
// timing analysis on MachineFunction.
class TimingNetlist : public MFDatapathContainer {
public:
  typedef double delay_type;
  typedef std::map<VASTMachineOperand*, delay_type> SrcInfoTy;
  // FIXME: Represent the destination with VASTValue.
  typedef std::map<VASTValue*, SrcInfoTy> PathInfoTy;
  // The name of this netlist.
  const StringRef Name;
private:
  // The path delay information.
  PathInfoTy PathInfo;

  // Create a path from Src to DstReg.
  void createDelayEntry(VASTValue *Dst, VASTMachineOperand *Src);

  // Compute the delay to DstReg through SrcReg.
  void createPathFromSrc(VASTValue *Dst, VASTValue *Src);
public:
  TimingNetlist(const StringRef Name, MachineRegisterInfo *MRI)
    : MFDatapathContainer(MRI), Name(Name) {}
  // Add the instruction into the data-path.
  void addInstrToDatapath(MachineInstr *MI);

  // Annotate the delay for path Src -> Dst.
  void annotateDelay(VASTMachineOperand *Src, VASTValue *Dst, delay_type delay);

  delay_type getDelay(VASTMachineOperand *Src, VASTValue *Dst) const;

  // Iterate over the source node reachable to DstReg.
  typedef SrcInfoTy::const_iterator src_iterator;
  src_iterator src_begin(VASTValue *Dst) const {
    PathInfoTy::const_iterator at = PathInfo.find(Dst);
    assert(at != PathInfo.end() && "DstReg not find!");
    return at->second.begin();
  }

  src_iterator src_end(VASTValue *Dst) const {
    PathInfoTy::const_iterator at = PathInfo.find(Dst);
    assert(at != PathInfo.end() && "DstReg not find!");
    return at->second.end();
  }

  bool src_empty(VASTValue *Dst) const {
    return !PathInfo.count(Dst);
  }

  // Iterators for path iterating.
  typedef PathInfoTy::iterator       path_iterator;
  typedef PathInfoTy::const_iterator const_path_iterator;

  path_iterator path_begin() { return PathInfo.begin(); }
  const_path_iterator path_begin() const { return PathInfo.begin(); }

  path_iterator path_end() { return PathInfo.end(); }
  const_path_iterator path_end() const { return PathInfo.end(); }
};
}

#endif