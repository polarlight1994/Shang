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

#include "shang/FUInfo.h"
#include "shang/VASTModulePass.h"

#include <map>

namespace llvm {
class VASTSelector;
class VASTSeqValue;
class VASTValue;
class CachedSequashTable;

/// Timinging Netlist - Annotate the timing information to the RTL netlist.
class TimingNetlist : public VASTModulePass {
public:
  typedef float delay_type;
  // TODO: For each bitslice of the source, allocate a delay record!
  typedef std::map<unsigned, delay_type> SrcDelayInfo;
  typedef SrcDelayInfo::value_type SrcEntryTy;
  typedef SrcDelayInfo::const_iterator src_iterator;
  typedef SrcDelayInfo::const_iterator const_src_iterator;

  typedef std::map<unsigned, SrcDelayInfo> PathDelayInfo;
  typedef PathDelayInfo::value_type PathTy;
  typedef PathDelayInfo::iterator path_iterator;
  typedef PathDelayInfo::const_iterator const_path_iterator;

  typedef std::map<unsigned, SrcDelayInfo> FaninDelayInfo;
  typedef FaninDelayInfo::value_type FaninTy;
  typedef FaninDelayInfo::iterator fanin_iterator;
  typedef FaninDelayInfo::const_iterator const_fanin_iterator;

  delay_type getSelectorDelayImpl(unsigned NumFannins, VASTSelector *Sel) const;

  // The path delay information.
  PathDelayInfo PathInfo;
  FaninDelayInfo FaninInfo;

  void buildTimingPathTo(VASTValue *Thu, VASTSelector *Dst, delay_type MUXDelay);
  bool performExternalAnalysis(VASTModule &VM);

  CachedSequashTable *CachedSequash;
public: 
  static char ID;

  TimingNetlist();

  delay_type getDelay(VASTValue *Src, VASTSelector *Dst) const;
  delay_type getDelay(VASTValue *Src, VASTValue *Dst) const;
  delay_type getDelay(VASTValue *Src, VASTValue *Thu, VASTSelector *Dst) const;

  path_iterator path_begin() { return PathInfo.begin(); }
  const_path_iterator path_begin() const { return PathInfo.begin(); }

  path_iterator path_end() { return PathInfo.end(); }
  const_path_iterator path_end() const { return PathInfo.end(); }

  virtual void releaseMemory();
  virtual bool runOnVASTModule(VASTModule &VM);
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  void print(raw_ostream &OS) const;

  void printPathsTo(raw_ostream &OS, VASTValue *Dst) const;
  void printPathsTo(raw_ostream &OS, const PathTy &Path) const;

  void dumpPathsTo(VASTValue *Dst) const;
};
}

#endif
