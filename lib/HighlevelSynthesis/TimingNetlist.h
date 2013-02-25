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
private:
  TNLDelay(uint16_t MSB_LLx1024, uint16_t LSB_LLx1024, bool)
    : MSB_LLx1024(MSB_LLx1024), LSB_LLx1024(LSB_LLx1024) {}

public:
  uint32_t MSB_LLx1024, LSB_LLx1024;

  TNLDelay() : MSB_LLx1024(0), LSB_LLx1024(0) {}
  TNLDelay(uint16_t MSB_LL, uint16_t LSB_LL)
    : MSB_LLx1024(MSB_LL * 1024), LSB_LLx1024(LSB_LL * 1024) {}

  TNLDelay(float delay)
    : MSB_LLx1024(ceil(delay/VFUs::LUTDelay) * 1024),
      LSB_LLx1024(ceil(delay/VFUs::LUTDelay) * 1024) {}

  TNLDelay operator + (TNLDelay RHS) const {
    return TNLDelay(MSB_LLx1024 + RHS.MSB_LLx1024, LSB_LLx1024 + LSB_LLx1024,
                    true);
  }

  static unsigned toInt(unsigned X) {
    return X == 0 ? 0 : (X - 1) / 1024 + 1;
  }

  unsigned getLSBLL() const { return toInt(LSB_LLx1024); }
  unsigned getMSBLL() const { return toInt(MSB_LLx1024); }
  unsigned getMaxLL() const { return std::max(getLSBLL(), getMSBLL()); }
  unsigned getMinLL() const { return std::min(getLSBLL(), getMSBLL()); }

  float getNormalizedDelay() const {
    return std::max(MSB_LLx1024, LSB_LLx1024) * VFUs::LUTDelay / 1024;
  }

  unsigned getNumCycles() const {
    return ceil(std::max(MSB_LLx1024, LSB_LLx1024) * VFUs::LUTDelay / 1024);
  }

  float getDelay() const {
    return std::max(MSB_LLx1024, LSB_LLx1024) * VFUs::LUTDelay * VFUs::Period
            / 1024;
  }

  static TNLDelay max(TNLDelay LHS, TNLDelay RHS) {
    // TODO: Extend the bit range and and max?
    return TNLDelay(std::max(LHS.MSB_LLx1024, RHS.MSB_LLx1024),
                    std::max(LHS.LSB_LLx1024, RHS.LSB_LLx1024),
                    true);
  }

  TNLDelay &scale(float RHS) {
    MSB_LLx1024 = ceil(MSB_LLx1024 / RHS);
    LSB_LLx1024 = ceil(LSB_LLx1024 / RHS);
  }

  TNLDelay &addLLParallel(unsigned MSB_LL, unsigned LSB_LL) {
    TNLDelay RHS(MSB_LL, LSB_LL);
    MSB_LLx1024 += RHS.MSB_LLx1024;
    LSB_LLx1024 += RHS.LSB_LLx1024;
    return *this;
  }

  TNLDelay &syncLL() {
    MSB_LLx1024 = LSB_LLx1024 = std::max(MSB_LLx1024, LSB_LLx1024);
    return *this;
  }

  TNLDelay &addLLWorst(unsigned MSB_LL, unsigned LSB_LL) {
    return addLLParallel(MSB_LL, LSB_LL).syncLL();
  }

  TNLDelay &addLLMSB2LSB(unsigned MSB_LL, unsigned LSB_LL, unsigned BitLL) {
    TNLDelay RHS(MSB_LL, LSB_LL);
    unsigned BitLLx1024 = BitLL * 1024;
    unsigned NewMSB_LLx1024 = RHS.MSB_LLx1024 + MSB_LLx1024;
    LSB_LLx1024 =  std::max(LSB_LLx1024 + BitLLx1024,
                            RHS.LSB_LLx1024 + MSB_LLx1024);
    MSB_LLx1024 = NewMSB_LLx1024;
    return *this;
  }

  TNLDelay &addLLLSB2MSB(unsigned MSB_LL, unsigned LSB_LL, unsigned BitLL) {
    TNLDelay RHS(MSB_LL, LSB_LL);
    unsigned BitLLx1024 = BitLL * 1024;
    unsigned NewLSB_LLx1024 = RHS.LSB_LLx1024 + LSB_LLx1024;
    MSB_LLx1024 =  std::max(MSB_LLx1024 + BitLLx1024,
                            RHS.MSB_LLx1024 + LSB_LLx1024);
    LSB_LLx1024 = NewLSB_LLx1024;
    return *this;
  }

  virtual void print(raw_ostream &OS) const;
  void dump() const;
};

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

  void buildTimingPath(VASTValue *Thu, VASTSeqValue *Dst, delay_type MUXDelay);
public:
  static char ID;

  TimingNetlist();
  ~TimingNetlist();

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
