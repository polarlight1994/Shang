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
class VASTSeqValue;
class VASTValue;
class BitlevelDelayEsitmator;

struct TNLDelay {

public:
  float MSB, LSB;
  VASTValue *From;

  TNLDelay() : MSB(0), LSB(0), From(0) {}
  TNLDelay(uint32_t MSB_LL, uint32_t LSB_LL)
    : MSB(MSB_LL * VFUs::LUTDelay), LSB(LSB_LL * VFUs::LUTDelay), From(0) {}

  TNLDelay(float MSB, float LSB) : MSB(MSB), LSB(LSB), From(0) {}

  float getLSB() const { return LSB; }
  float getMSB() const { return MSB; }
  float getMinDelay() const { return std::min(getLSB(), getMSB()); }

  float getNormalizedDelay() const {
    return std::max(MSB, LSB);
  }

  unsigned getNumCycles() const {
    return ceil(getNormalizedDelay());
  }

  float getDelay() const {
    return getNormalizedDelay() * VFUs::Period;
  }

  static TNLDelay max(TNLDelay LHS, TNLDelay RHS) {
    // TODO: Extend the bit range and and max?
    return TNLDelay(std::max(LHS.MSB, RHS.MSB),
                    std::max(LHS.LSB, RHS.LSB));
  }

  TNLDelay &scale(float RHS) {
    MSB = ceil(MSB / RHS);
    LSB = ceil(LSB / RHS);
    return *this;
  }

  TNLDelay &addLLParallel(const TNLDelay &RHS) {
    MSB += RHS.MSB;
    LSB += RHS.LSB;
    return *this;
  }

  TNLDelay &addLLParallel(float MSB, float LSB) {
    TNLDelay RHS(MSB, LSB);
    this->MSB += RHS.MSB;
    this->LSB += RHS.LSB;
    return *this;
  }

  TNLDelay &syncLL() {
    MSB = LSB = std::max(MSB, LSB);
    return *this;
  }

  TNLDelay &addLLWorst(float MSB, float LSB) {
    return addLLParallel(MSB, LSB).syncLL();
  }

  TNLDelay &addLLMSB2LSB(float MSB, float LSB, float Bit) {
    TNLDelay RHS(MSB, LSB);
    float NewMSB = RHS.MSB + this->MSB;
    this->LSB =  std::max(this->LSB + Bit, RHS.LSB + this->MSB);
    this->MSB = NewMSB;
    return *this;
  }

  TNLDelay &addLLLSB2MSB(float MSB, float LSB, float Bit) {
    TNLDelay RHS(MSB, LSB);
    float NewLSB = RHS.LSB + this->LSB;
    this->MSB =  std::max(this->MSB + Bit, RHS.MSB + this->LSB);
    this->LSB = NewLSB;
    return *this;
  }

  void print(raw_ostream &OS) const;
  void dump() const;
};

inline raw_ostream &operator<<(raw_ostream &OS, const TNLDelay &D) {
  D.print(OS);
  return OS;
}

/// Timinging Netlist - Annotate the timing information to the RTL netlist.
class TimingNetlist : public VASTModulePass {
public:
  typedef TNLDelay delay_type;
  // TODO: For each bitslice of the source, allocate a delay record!
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
  BitlevelDelayEsitmator *Estimator;

  void buildTimingPathToReg(VASTValue *Thu, VASTSeqValue *Dst, delay_type MUXDelay);
public: 
  static char ID;

  TimingNetlist();
  ~TimingNetlist();

  TNLDelay getMuxDelay(unsigned Fanins, VASTSeqValue *SVal = 0) const;

  delay_type getDelay(VASTValue *Src, VASTValue *Dst) const;
  delay_type getDelay(VASTValue *Src, VASTValue *Thu, VASTValue *Dst) const;

  float getNormalizedDelay(VASTValue *Src, VASTValue *Dst) const {
    return getDelay(Src, Dst).getNormalizedDelay();
  }

  float getNormalizedDelay(VASTValue *Src, VASTValue *Thu, VASTValue *Dst) const {
    return getDelay(Src, Thu, Dst).getNormalizedDelay();
  }

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

  const TNLDelay *getDelayOrNull(VASTValue *Src, VASTValue *Dst) const {
    const SrcDelayInfo *Srcs = getSrcInfo(Dst);
    if (Srcs == 0) return 0;

    src_iterator path_start_from = Srcs->find(Src);
    if (path_start_from == Srcs->end()) return 0;

    return &path_start_from->second;
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

  void dumpPathsTo(VASTValue *Dst) const;
};
}

#endif
