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
#ifndef SHANG_TIMING_NETLIST_H
#define SHANG_TIMING_NETLIST_H

#include "shang/VASTModulePass.h"

#include <map>
namespace llvm {
class VASTSeqValue;
class VASTValue;
class TimingEstimatorBase;

struct TNLDelay {
  float delay;
  uint16_t MSB_LL, LSB_LL;
  uint16_t PathSize;

  TNLDelay() : delay(0.0), MSB_LL(0), LSB_LL(0), PathSize(0) {}

  TNLDelay(float delay) : delay(delay), MSB_LL(0), LSB_LL(0), PathSize(0) {}

  TNLDelay operator + (double RHS) const {
    return TNLDelay(delay + RHS);
  }

  TNLDelay operator + (TNLDelay RHS) const {
    return TNLDelay(delay + RHS.delay);
  }

  TNLDelay operator / (float RHS) const {
    return TNLDelay(delay / RHS);
  }

  operator float() const {
    return delay;
  }

  static TNLDelay max(TNLDelay LHS, TNLDelay RHS) {
    if (LHS.delay > RHS.delay) return LHS;

    return RHS;
  }
};

/// Timinging Netlist - Annotate the timing information to the RTL netlist.
class TimingNetlist : public VASTModulePass {
public:
  typedef TNLDelay delay_type;
  typedef std::map<VASTValue*, delay_type> SrcDelayInfo;
  typedef SrcDelayInfo::value_type SrcEntryTy;
  typedef SrcDelayInfo::const_iterator src_iterator;
  typedef SrcDelayInfo::const_iterator const_src_iterator;

  typedef std::map<VASTValue*, SrcDelayInfo> PathDelayInfo;
  typedef PathDelayInfo::value_type PathTy;
  typedef PathDelayInfo::iterator path_iterator;
  typedef PathDelayInfo::const_iterator const_path_iterator;

private:
  // The path delay information.
  PathDelayInfo PathInfo;
  TimingEstimatorBase *Estimator;

  void buildTimingPath(VASTValue *Thu, VASTSeqValue *Dst, delay_type MUXDelay);
public:
  static char ID;

  TimingNetlist();

  delay_type getDelay(VASTValue *Src, VASTValue *Dst) const;
  delay_type getDelay(VASTValue *Src, VASTValue *Thu, VASTValue *Dst) const;

  // Iterate over the source node reachable to DstReg.
  src_iterator src_begin(VASTValue *Dst) const {
    const_path_iterator at = PathInfo.find(Dst);
    assert(at != PathInfo.end() && "DstReg not find!");
    return at->second.begin();
  }

  src_iterator src_end(VASTValue *Dst) const {
    const_path_iterator at = PathInfo.find(Dst);
    assert(at != PathInfo.end() && "DstReg not find!");
    return at->second.end();
  }

  bool src_empty(VASTValue *Dst) const {
    return !PathInfo.count(Dst);
  }

  const SrcDelayInfo *getSrcInfo(VASTValue *Dst) const {
    const_path_iterator at = PathInfo.find(Dst);
    return at != PathInfo.end() ? &at->second : 0;
  }

  path_iterator path_begin() { return PathInfo.begin(); }
  const_path_iterator path_begin() const { return PathInfo.begin(); }

  path_iterator path_end() { return PathInfo.end(); }
  const_path_iterator path_end() const { return PathInfo.end(); }

  void releaseMemory();
  bool runOnVASTModule(VASTModule &VM);
  void getAnalysisUsage(AnalysisUsage &AU) const;
  void print(raw_ostream &OS) const;

  void printPathsTo(raw_ostream &OS, VASTValue *Dst) const;
  void printPathsTo(raw_ostream &OS, const PathTy &Path) const;
};
}

#endif
