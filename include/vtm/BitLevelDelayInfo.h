//====----- BitLevelDelayInfo.h - Bit-level delay estimator -----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Compute the detail ctrlop to ctrlop latency (in cycle ratio) information.
//
//===----------------------------------------------------------------------===//
#include "vtm/VInstrInfo.h"
#include "vtm/Passes.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm{
class TimingNetlist;

struct InstPtrTy : public PointerUnion<MachineInstr*, MachineBasicBlock*> {
  typedef PointerUnion<MachineInstr*, MachineBasicBlock*> Base;

  bool isMI() const { return is<MachineInstr*>(); }
  bool isMBB() const { return is<MachineBasicBlock*>(); }

  MachineInstr* dyn_cast_mi() const {
    return dyn_cast<MachineInstr*>();
  }

  MachineBasicBlock* dyn_cast_mbb() const {
    return dyn_cast<MachineBasicBlock*>();
  }

  MachineInstr* get_mi() const {
    return get<MachineInstr*>();
  }

  MachineBasicBlock* get_mbb() const {
    return get<MachineBasicBlock*>();
  }

  operator MachineInstr*() const {
    return dyn_cast_mi();
  }

  MachineInstr* operator ->() const {
    return get_mi();
  }

  operator MachineBasicBlock*() const {
    return dyn_cast_mbb();
  }

  MachineBasicBlock *getParent() const {
    return isMBB() ? get_mbb() : get_mi()->getParent();
  }

  bool operator< (InstPtrTy RHS) const {
    return getOpaqueValue() < RHS.getOpaqueValue();
  }

  // Dirty Hack: Accept constant pointer and perform const_cast. We should get
  // rid of this.
  /*implicit*/ InstPtrTy(const MachineInstr *MI)
    : Base(const_cast<MachineInstr*>(MI)) {
      assert(MI && "Unexpected null pointer!");
  }

  /*implicit*/ InstPtrTy(const MachineBasicBlock *MBB)
    : Base(const_cast<MachineBasicBlock*>(MBB)) {
      assert(MBB && "Unexpected null pointer!");
  }

  InstPtrTy() : Base(static_cast<MachineInstr*>(0)) {}
};

class BitLevelDelayInfo : public MachineFunctionPass {
public:
  static char ID;
  // Remember the normalized delay from source.
  typedef double delay_type;
  typedef std::map<InstPtrTy, delay_type> DepLatInfoTy;

  static unsigned getNumCPCeil(DepLatInfoTy::value_type v);
  static unsigned getNumCPFloor(DepLatInfoTy::value_type v);

  MachineRegisterInfo *MRI;

  const static delay_type Delta;
private:
  TimingNetlist *TNL;
  // Cache the computational delay for every instruction.
  typedef std::map<const MachineInstr*, double> CachedLatMapTy;
  CachedLatMapTy CachedLatencies;

  delay_type computeAndCacheLatencyFor(const MachineInstr *MI);
  delay_type getCachedLatencyResult(const MachineInstr *MI) const {
    CachedLatMapTy::const_iterator at = CachedLatencies.find(MI);
    assert(at != CachedLatencies.end() && "Latency not calculated!");
    return at->second;
  }

  // The latency from all register source through the datapath to a given
  // wire/register define by a datapath/control op
  typedef std::map<const MachineInstr*, DepLatInfoTy> LatencyMapTy;

  LatencyMapTy LatencyMap;

  // Return the edge latency between SrcInstr and DstInstr considering chaining
  // effect.
  delay_type
  getChainedDelay(const MachineInstr *SrcInstr, unsigned DstOpcode) const;

  void printChainDelayInfo(raw_ostream & O, const std::string &Prefix,
                           const DepLatInfoTy::value_type &II,
                           const MachineInstr *DstMI) const;

  void buildDelayMatrix(const MachineInstr *MI);
  void addDelayForPath(unsigned SrcReg, const MachineInstr *MI,
                       DepLatInfoTy &CurDelayInfo,
                       delay_type PathDelay = delay_type(0)) ;
  void addDelayForPath(const MachineInstr *SrcMI, const MachineInstr *MI,
                       DepLatInfoTy &CurDelayInfo,
                       delay_type PathDelay = delay_type(0)) ;
public:
  BitLevelDelayInfo();
  const char *getPassName() const {
    return "Refine-CDFG";
  }

  unsigned getStepsToFinish(const MachineInstr *MI) const;

  unsigned getChainedCPs(const MachineInstr *SrcInstr, unsigned DstOpcode) const;

  // Get the source register and the corresponding latency to DstMI
  const DepLatInfoTy *getDepLatInfo(const MachineInstr *DstMI) const {
    LatencyMapTy::const_iterator at = LatencyMap.find(DstMI);
    return at == LatencyMap.end() ? 0 : &at->second;
  }

  typedef const std::set<const MachineInstr*> MISetTy;
  // All operation must finish before the BB exit, this function build the
  // information about the latency from instruction to the BB exit.
  void buildExitMIInfo(const MachineInstr *SSnk, DepLatInfoTy &Info,
                       MISetTy &ExitMIs);

  void addDummyLatencyEntry(const MachineInstr *MI) {
    CachedLatencies.insert(std::make_pair(MI, delay_type(0)));
  }

  void clearCachedLatencies() { CachedLatencies.clear(); }

  void reset();

  void getAnalysisUsage(AnalysisUsage &AU) const;

  bool runOnMachineFunction(MachineFunction &MF);

  void releaseMemory() { reset(); }

  // Delay estimation statistics.
  void print(raw_ostream &O, const Module *M) const;
  void dump() const;
};

}
