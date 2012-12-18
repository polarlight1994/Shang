//===- VRegAllocSimple.cpp - Simple Register Allocation ---------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a simple register allocation pass for Verilog target
// machine.
//
//===----------------------------------------------------------------------===//

#include "CompGraphTraits.h"
#include "CompGraph.h"
#include "CompGraphDOT.h"

#include "vtm/VerilogBackendMCTargetDesc.h"
#include "vtm/VFInfo.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/Passes.h"
#include "vtm/VInstrInfo.h"

//Dirty Hack:
#include "llvm/../../lib/CodeGen/VirtRegMap.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Function.h"
#include "llvm/PassAnalysisSupport.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/EdgeBundles.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/LiveStackAnalysis.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineLoopRanges.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#define DEBUG_TYPE "vtm-regalloc"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(LIMerged,
          "Number of live intervals merged in resource binding pass");
static cl::opt<bool> DisableFUSharing("vtm-disable-fu-sharing",
                                      cl::desc("Disable function unit sharing"),
                                      cl::init(false));

namespace {
struct VRASimple : public MachineFunctionPass {
  // Context.
  MachineFunction *MF;
  VFInfo *VFI;
  MachineRegisterInfo *MRI;
  const TargetInstrInfo *TII;
  VRegisterInfo *TRI;
  // Analysis
  VirtRegMap *VRM;
  LiveIntervals *LIS;

  typedef std::map<FuncUnitId, LiveInterval*> FU2PhysRegMapTy;
  FU2PhysRegMapTy FU2PhysRegMap;

  typedef const std::vector<unsigned> VRegVec;
  // Register/FU Compatibility Graph.
  typedef CompGraph<LiveInterval*, unsigned> LICGraph;
  typedef CompGraphNode<LiveInterval*> LICGraphNode;
  LICGraph RegLIG;

  VRASimple();
  void init(VirtRegMap &vrm, LiveIntervals &lis);

  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const;
  void releaseMemory();

  void assign(LiveInterval &VirtReg, unsigned PhysReg) {
    assert(!VRM->hasPhys(VirtReg.reg) && "Duplicate VirtReg assignment");
    VRM->assignVirt2Phys(VirtReg.reg, PhysReg);
  }

  LiveInterval *getInterval(unsigned RegNum) {
    if (!RegNum || MRI->reg_nodbg_empty(RegNum) || !LIS->hasInterval(RegNum))
      return 0;

    LiveInterval &LI = LIS->getInterval(RegNum);
    if (LI.empty()) {
      //LIS->removeInterval(RegNum);
      return 0;
    }

    return &LI;
  }

  unsigned getBitWidthOf(unsigned RegNum) {
    unsigned BitWidth = 0;
    assert(!MRI->reg_empty(RegNum) && "Register not is defined!");
    for (MachineRegisterInfo::def_iterator I = MRI->def_begin(RegNum);
         I != MachineRegisterInfo::def_end(); ++I) {
      MachineOperand &DefOp = I.getOperand();
      BitWidth = std::max(BitWidth, VInstrInfo::getBitWidth(DefOp));
    }

    assert(BitWidth && "Unexpected empty define register");
    return BitWidth;
  }

  // Run the functor on each definition of the register, and return true
  // if any functor return true (this means we meet some unexpected situation),
  // return false otherwise.
  template<class DefFctor>
  bool iterateUseDefChain(unsigned Reg, DefFctor &F) const {
    for (MachineRegisterInfo::reg_iterator I = MRI->reg_begin(Reg),
         E = MRI->reg_end(); I != E; ++I)
      if (F(I)) return true;

    return false;
  }

  void mergeLI(LiveInterval *FromLI, LiveInterval *ToLI,
               bool AllowOverlap = false);

  // Put all datapath op that using the corresponding register of the LI to
  // the user vector.
  typedef DenseMap<MachineInstr*, unsigned, VMachineInstrExpressionTrait>
    DatapathOpMap;
  void mergeIdenticalDatapath(LiveInterval *LI);
  typedef EquivalenceClasses<unsigned> EquRegClasses;
  void mergeEquLIs(EquRegClasses &LIs);

  // Compatibility Graph building.
  template<class CompEdgeWeight>
  bool reduceCompGraph(CompEdgeWeight &C);

  void bindCompGraph();

  void handleMemoryAccess(MachineInstr *MI);
  void handleBRAMAccess(MachineInstr *MI);
  void handleDstMux(MachineInstr *MI);
  unsigned allocateCalleeFNPorts(unsigned RegNum);
  void handleCalleeFN(MachineInstr *MI);
  void handleCopy(MachineInstr *MI);

  void handleCtrlOp(MachineInstr *MI);
  bool runOnMachineFunction(MachineFunction &F);

  void rewrite();
  void addMBBLiveIns(MachineFunction *MF);

  const char *getPassName() const {
    return "Verilog Backend Resource Binding Pass";
  }
};

struct WidthChecker {
  // The Maximum bit width of current register.
  unsigned CurMaxWidth;

  WidthChecker() : CurMaxWidth(0) {}

  void resetWidth() { CurMaxWidth = 0; }

  bool checkWidth(unsigned CurWidth) {
    //CurMaxWidth = std::max(CurMaxWidth, CurWidth);
    if (!CurMaxWidth)
      CurMaxWidth = CurWidth;
    else if (CurMaxWidth != CurWidth)
      // Do not merge regisrters with difference width.
      return false;

    return true;
  }

  unsigned getWidth() const { return CurMaxWidth; }
};

// Implement this with some hash function?
static uint64_t getRegKey(uint32_t Reg, uint32_t OpIdx) {
  union {
    uint64_t Data;
    struct {
      uint32_t RegNum;
      uint32_t OpIdx;
    } S;
  } U;

  U.S.RegNum = Reg;
  U.S.OpIdx = OpIdx;

  return U.Data;
}

//static uint64_t getRegKey(MachineOperand &MO, uint8_t OpIdx = 0) {
//  return getRegKey(MO.getReg(), OpIdx, MO.getSubReg());
//}

template<int NUMSRC>
struct FaninChecker {
  // TODO Explain sources and fanins
  // All copy source both fu of the edge.
  SmallSet<uint64_t, 4> MergedFanins[NUMSRC];
  unsigned Fanins[2][NUMSRC];
  unsigned CurSrc;
  unsigned ExtraCost;
  // TODO: Timing cost.
  // The more predicate value we have, the bigger latency will get to select
  // a value from the mux value.
  unsigned PredNum;
  SmallSet<uint64_t, 4> Preds;

  FaninChecker() {}

  void resetSrcs() {
    ExtraCost = 0;
    CurSrc = 0;
    for (unsigned i = 0; i < NUMSRC; ++i) {
      MergedFanins[i].clear();
      Fanins[0][i] = 0;
      Fanins[1][i] = 0;
    }
  }

  // Notify the source checker that we are switching to next source.
  void nextSrc() { ++CurSrc; }

  template<int N>
  void addFanin(MachineOperand &FanInOp) {
    if (FanInOp.isReg()) {
      // Igore the operand index.
      MergedFanins[N].insert(getRegKey(FanInOp.getReg(), 0));
      ++Fanins[CurSrc][N];
    } else
      ExtraCost += /*LUT Cost*/ VFUs::LUTCost;
  }

  template<int N>
  void removeSrcReg(unsigned Reg) {
    MergedFanins[N].erase(Reg);
  }

  template<int N>
  int getMergedSrcMuxSize() const {
    return MergedFanins[N].size();
  }

  template<int N>
  int getSrcNum(int SrcIndex) const {
    return Fanins[SrcIndex][N];
  }

  template<int N>
  int getSavedSrcMuxCost(int BitWidth){
    VFUMux *MUXDesc = getFUDesc<VFUMux>();
    return MUXDesc->getMuxCost(getSrcNum<N>(0), BitWidth)
           + MUXDesc->getMuxCost(getSrcNum<N>(1), BitWidth)
           - MUXDesc->getMuxCost(getMergedSrcMuxSize<N>(), BitWidth);
  }

  int getExtraCost() const { return ExtraCost; }
  //// Total cost.
  int getTotalSavedSrcMuxCost(int BitWidth);
  //int getTotalSrcMuxCost(int BitWidth);
  int getMaxMergedSrcMuxSize();
};

template<>
int FaninChecker<1>::getTotalSavedSrcMuxCost(int BitWidth){
  return getSavedSrcMuxCost<0>(BitWidth);
}

template<>
int FaninChecker<2>::getTotalSavedSrcMuxCost(int BitWidth){
  return getSavedSrcMuxCost<0>(BitWidth) + getSavedSrcMuxCost<1>(BitWidth);
}

template<>
int FaninChecker<1>::getMaxMergedSrcMuxSize() {
return getMergedSrcMuxSize<0>();
}

template<>
int FaninChecker<2>::getMaxMergedSrcMuxSize() {
return std::max(getMergedSrcMuxSize<0>(), getMergedSrcMuxSize<1>());
}

struct FanoutChecker {
  // All copy source both fu of the edge.
  SmallSet<uint64_t, 8> mergedFanouts;
  int NumFanouts;
  void resetFanouts() {
    mergedFanouts.clear();
    NumFanouts = 0;
  }

  unsigned addFanout(MachineRegisterInfo::reg_iterator I) {
    MachineInstr *MI = &*I;

    // Ignore the datapath at the moment.
    if (!VInstrInfo::isCtrlBundle(MI)) return 0;
    ++NumFanouts;
    if (MI->getNumOperands() == 0) {
      assert(MI->getOpcode() == VTM::VOpRet && "Unexpected empty operand op!");
      mergedFanouts.insert(getRegKey(1, -1));
      return 0;
    }

    MachineOperand &DefMO = MI->getOperand(0);
    if (!DefMO.isReg()) {
      if (MI->getOpcode() == VTM::VOpRetVal)
        mergedFanouts.insert(getRegKey(0, -2));

      return 0;
    }

    unsigned DstReg = DefMO.getReg();
    mergedFanouts.insert(getRegKey(DstReg, I.getOperandNo()));
    return DstReg;
  }

  int getNumMergedFanouts() {
    return mergedFanouts.size();
  }

  int getSavedFanoutsCost(int BitWidth) {
    VFUMux *MUXDesc = getFUDesc<VFUMux>();

    return MUXDesc->getMuxCost(NumFanouts, BitWidth) -
           MUXDesc->getMuxCost(getNumMergedFanouts(), BitWidth);
  }

};

template<class FUDescTy, int NUMSRC>
struct CompEdgeWeightBase : public FaninChecker<NUMSRC>, public FanoutChecker,
                            public WidthChecker {
  VRASimple *VRA;

  CompEdgeWeightBase(VRASimple *V) : VRA(V) {}

  void reset() {
    resetFanouts();
    FaninChecker<NUMSRC>::resetSrcs();
    resetWidth();
  }

  int computeWeight(unsigned FanInWidth, unsigned FanOutWidth) {
    VFUMux *MUXDesc = getFUDesc<VFUMux>();
    // Only merge the register if the mux size not exceed the max allowed size.
    if (FaninChecker<NUMSRC>::getMaxMergedSrcMuxSize() > int(MUXDesc->MaxAllowedMuxSize))
      return CompGraphWeights::HUGE_NEG_VAL;

    int Weight = 0;
    // We can save some register if we merge these two registers.
    unsigned Size = std::min(FanInWidth, 64u);
    Weight += /*FU Cost*/ getFUDesc<FUDescTy>()->lookupCost(Size);
    Weight += FaninChecker<NUMSRC>::getTotalSavedSrcMuxCost(FanInWidth);
    Weight += getSavedFanoutsCost(FanOutWidth);
    return Weight;
  }

  int computeWeight(int BitWidth) { return computeWeight(BitWidth, BitWidth); }
};

// Dirty Hack: Specialize for register.
template<>
int CompEdgeWeightBase<VFUDesc, 1>::computeWeight(unsigned FanInWidth,
                                                  unsigned FanOutWidth) {
  VFUMux *MUXDesc = getFUDesc<VFUMux>();
  // Only merge the register if the mux size not exceed the max allowed size.
  if (FaninChecker<1>::getMaxMergedSrcMuxSize() > int(MUXDesc->MaxAllowedMuxSize))
    return CompGraphWeights::HUGE_NEG_VAL;

  int Weight = 0;
  // We can save some register if we merge these two registers.
  Weight += /*FU Cost*/ FanInWidth * VFUs::RegCost;
  Weight += FaninChecker<1>::getTotalSavedSrcMuxCost(FanInWidth);

  return Weight;
}

// Weight computation functor for register Compatibility Graph.
struct CompRegEdgeWeight : public CompEdgeWeightBase<VFUDesc, 1> {
  // Context
  // Do not try to merge PHI copies, it is conditional and we cannot handle
  // them correctly at the moment.
  bool hasPHICopy;
  unsigned DstReg;

  typedef CompEdgeWeightBase<VFUDesc, 1> Base;
  CompRegEdgeWeight(VRASimple *V) : Base(V) {}

  void reset(unsigned DstR) {
    DstReg = DstR;
    hasPHICopy = false;
    Base::reset();
  }

  bool visitUse(MachineRegisterInfo::reg_iterator I) {
    unsigned DefReg = addFanout(I);
    if (I->getOpcode() == VTM::VOpMvPhi || I->getOpcode() == VTM::VOpSel) {
      // We cannot handle these ops correctly after their src and dst merged.
      if (DefReg == DstReg) return true;
    }

    return false;
  }

  bool visitDef(MachineInstr *MI) {
    switch (MI->getOpcode()) {
    case VTM::VOpMvPhi:
      // FIXME: Merging VOpMvPhi break adpcm_main_IMS_ASAP.
      return true;
    case VTM::VOpMvPipe:
    case VTM::VOpMove:
    case VTM::VOpMoveArg:
    case VTM::VOpReadFU:
      addFanin<0>(MI->getOperand(1));
      break;
    case VTM::VOpReadReturn:
      addFanin<0>(MI->getOperand(2));
      break;
    default:
#ifndef NDEBUG
      MI->dump();
      llvm_unreachable("Unexpected opcode in CompRegEdgeWeight!");
#endif
    }
    return false;
  }

  // Run on the definition of a register to collect information about the live
  // interval.
  bool operator()(MachineRegisterInfo::reg_iterator I) {
    MachineOperand &MO = I.getOperand();
    if (MO.isDef()) {
      // 1. Get the bit width information.
      if (!checkWidth(VInstrInfo::getBitWidth(I.getOperand())))
        return true;

      // 2. Analyze the definition op.
      return visitDef(&*I);
    }

    if (!MO.isImplicit()) return visitUse(I);

    return false;
  }

  // Run on the edge of the Compatibility Graph and return the weight of the
  // edge.
  int operator()(LiveInterval *Src, LiveInterval *Dst) {
    assert(Dst && Src && "Unexpected null li!");
    reset(Dst->reg);

    if (VRA->iterateUseDefChain(Src->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    // Go on check next source.
    nextSrc();

    if (VRA->iterateUseDefChain(Dst->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    if (hasPHICopy) return CompGraphWeights::HUGE_NEG_VAL;
    // Src register appear in the src of mux do not cost anything.
    removeSrcReg<0>(Src->reg);

    return Base::computeWeight(Base::getWidth());
  }
};
}

namespace llvm {
  // Specialized For liveInterval.
template<> struct CompGraphTraits<LiveInterval*> {
  static bool isEarlier(const LiveInterval *LHS, const LiveInterval *RHS) {
    assert(LHS && RHS && "Unexpected virtual node!");

    return *LHS < *RHS;
  }

  static bool compatible(const LiveInterval *LHS, const LiveInterval *RHS) {
    if (LHS == 0 || RHS == 0) return true;

    // A LiveInterval may has multiple live ranges for example:
    // LI0 = [0, 4), [32, 35)
    // Suppose we have another LiveInterval LI1 and LI2:
    // LI1 = [5, 9)
    // LI2 = [26, 33)
    // If we make LiveIntervals compatible if they are not overlap, we may
    // have a compatible graph for LI0, LI1 and LI2 like this:
    // LI0 -> LI1 -> LI2
    // The compatibility in a compatible graph suppose to be transitive,
    // but this not true in the above graph because LI0 is overlap with LI2.
    // This means two live interval may not mark as "compatible" even if they
    // are not overlap.

    // LHS and RHS is compatible if RHS end before LHS begin and vice versa.
    return LHS->beginIndex() >= RHS->endIndex()
            || RHS->beginIndex() >= LHS->endIndex();
  }

  static std::string getNodeLabel(const LiveInterval *LI) {
    std::string Str;
    raw_string_ostream ss(Str);
    ss << PrintReg(LI->reg);
    return ss.str();
  }
};
}

//===----------------------------------------------------------------------===//

FunctionPass *llvm::createSimpleRegisterAllocator() {
  return new VRASimple();
}

char VRASimple::ID = 0;

VRASimple::VRASimple() : MachineFunctionPass(ID) {
  initializeAdjustLIForBundlesPass(*PassRegistry::getPassRegistry());
  initializeLiveIntervalsPass(*PassRegistry::getPassRegistry());
  initializeSlotIndexesPass(*PassRegistry::getPassRegistry());
  initializeVirtRegMapPass(*PassRegistry::getPassRegistry());
  initializeBitLevelInfoPass(*PassRegistry::getPassRegistry());
}

void VRASimple::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AliasAnalysis>();
  AU.addPreserved<AliasAnalysis>();
  AU.addRequired<LiveIntervals>();
  AU.addRequiredID(AdjustLIForBundlesID);
  AU.addPreservedID(AdjustLIForBundlesID);
  AU.addPreserved<SlotIndexes>();
  AU.addRequired<VirtRegMap>();
  AU.addPreserved<VirtRegMap>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

void VRASimple::releaseMemory() {
  //RegAllocBase::releaseMemory();
  FU2PhysRegMap.clear();
  RegLIG.reset();
}

void VRASimple::init(VirtRegMap &vrm, LiveIntervals &lis) {
  TargetRegisterInfo *RegInfo
    = const_cast<TargetRegisterInfo*>(&vrm.getTargetRegInfo());
  TRI = reinterpret_cast<VRegisterInfo*>(RegInfo);
  MRI = &vrm.getRegInfo();
  VRM = &vrm;
  LIS = &lis;
  // Reset the physics register allocation information before register allocation.
  TRI->resetPhyRegAllocation();
  // FIXME: Init the PhysReg2LiveUnion right before we start to bind the physics
  // registers.
  // PhysReg2LiveUnion.init(UnionAllocator, MRI->getNumVirtRegs() + 1);
  // Cache an interferece query for each physical reg
  // Queries.reset(new LiveIntervalUnion::Query[PhysReg2LiveUnion.numRegs()]);
}

void VRASimple::handleMemoryAccess(MachineInstr *MI) {
  unsigned RegNo = MI->getOperand(0).getReg();
  assert(TargetRegisterInfo::isVirtualRegister(RegNo) && "Bad register number!");
  assert(MRI->getRegClass(RegNo) == &VTM::RINFRegClass && "Bad register class!");
  // Look up the corresponding LiveInterval.
  FuncUnitId FU = VInstrInfo::getPreboundFUId(MI);
  LiveInterval *&FULI = FU2PhysRegMap[FU];

  if (FULI == 0) {
    FULI = getInterval(RegNo);
    assign(*FULI, TRI->allocateFN(VTM::RINFRegClassID));
    return;
  }
  
  mergeLI(getInterval(RegNo), FULI, true);
}

void VRASimple::handleBRAMAccess(MachineInstr *MI) {
  unsigned RegNo = MI->getOperand(0).getReg();
  assert(TargetRegisterInfo::isVirtualRegister(RegNo) && "Bad register number!");
  assert(MRI->getRegClass(RegNo) == &VTM::RBRMRegClass && "Bad register class!");
  // Look up the corresponding LiveInterval.
  FuncUnitId FU = VInstrInfo::getPreboundFUId(MI);
  LiveInterval *&FULI = FU2PhysRegMap[FU];

  if (FULI == 0) {
    // Allocate the physical register according to the BRAM information.
    unsigned BRAMNum = FU.getFUNum();
    VFInfo::BRamInfo &Info = VFI->getBRamInfo(BRAMNum);
    unsigned &PhysReg = Info.PhyRegNum;
    unsigned BitWidth = Info.ElemSizeInBytes * 8;
    // Get the physical register number and update the blockram information.
    PhysReg = TRI->allocateFN(VTM::RBRMRegClassID, BitWidth);
    FULI = getInterval(RegNo);
    assign(*FULI, PhysReg);
    return;
  }
  
  mergeLI(getInterval(RegNo), FULI, true);
}

void VRASimple::handleDstMux(MachineInstr *MI) {
  unsigned RegNo = MI->getOperand(0).getReg();
  assert(TargetRegisterInfo::isVirtualRegister(RegNo) && "Bad register number!");
  assert(MRI->getRegClass(RegNo) == &VTM::RMUXRegClass && "Bad register class!");
  // Look up the corresponding LiveInterval.
  FuncUnitId FU = VInstrInfo::getPreboundFUId(MI);
  LiveInterval *&FULI = FU2PhysRegMap[FU];

  if (FULI == 0) {
    unsigned FNNum = FU.getFUNum();
    unsigned BitWidth = VInstrInfo::getBitWidth(MI->getOperand(0));
    unsigned PhysReg = TRI->allocateFN(VTM::RMUXRegClassID, BitWidth);
    FULI = getInterval(RegNo);
    assign(*FULI, PhysReg);
    return;
  }

  mergeLI(getInterval(RegNo), FULI, true);
}

unsigned VRASimple::allocateCalleeFNPorts(unsigned RegNum) {
  unsigned RetPortSize = 0;
  typedef MachineRegisterInfo::use_iterator use_it;
  for (use_it I = MRI->use_begin(RegNum); I != MRI->use_end(); ++I) {
    MachineInstr *MI = &*I;
    if (MI->getOpcode() == VTM::VOpReadFU ||
        MI->getOpcode() == VTM::VOpDisableFU)
      continue;

    assert(MI->getOpcode() == VTM::VOpReadReturn && "Unexpected callee user!");
    assert((RetPortSize == 0 ||
            RetPortSize == VInstrInfo::getBitWidth(MI->getOperand(0)))
            && "Return port has multiple size?");
    RetPortSize = VInstrInfo::getBitWidth(MI->getOperand(0));
  }

  return TRI->allocateFN(VTM::RCFNRegClassID, RetPortSize);
}


void VRASimple::handleCalleeFN(MachineInstr *MI) {
  unsigned RegNo = MI->getOperand(0).getReg();
  assert(TargetRegisterInfo::isVirtualRegister(RegNo) && "Bad register number!");
  assert(MRI->getRegClass(RegNo) == &VTM::RCFNRegClass && "Bad register class!");
  // Look up the corresponding LiveInterval.
  FuncUnitId FU = VInstrInfo::getPreboundFUId(MI);
  LiveInterval *&FULI = FU2PhysRegMap[FU];

  if (FULI == 0) {
    unsigned FNNum = FU.getFUNum();
    unsigned PhysReg = allocateCalleeFNPorts(RegNo);

    // Also allocate the enable port.
    TRI->getSubRegOf(PhysReg, 1, 0);
    if (PhysReg != FNNum)
      VFI->remapCallee(MI->getOperand(1).getSymbolName(), PhysReg);

    FULI = getInterval(RegNo);
    assign(*FULI, PhysReg);
    return;
  }

  mergeLI(getInterval(RegNo), FULI, true);
}

void VRASimple::handleCtrlOp(MachineInstr *MI) {
  unsigned Opcode = MI->getOpcode();
  switch (Opcode) {
  case VTM::VOpMemTrans:      handleMemoryAccess(MI); break;
  case VTM::VOpBRAMTrans:     handleBRAMAccess(MI); break;
  case VTM::VOpDstMux:        handleDstMux(MI); break;
  case VTM::VOpInternalCall:  handleCalleeFN(MI); break;
  case VTM::VOpReadFU:
    if (MI->getOperand(0).getReg() == 0)
      break;
    /* FALL THROUGH */
  case VTM::VOpMoveArg:
  case VTM::VOpMove:
  case VTM::VOpMvPhi:
  case VTM::VOpReadReturn:
    //Build the Compatibility Graphs
    RegLIG.GetOrCreateNode(getInterval(MI->getOperand(0).getReg()));
    break;
  }
}

bool VRASimple::runOnMachineFunction(MachineFunction &F) {
  MF = &F;
  VFI = F.getInfo<VFInfo>();

  init(getAnalysis<VirtRegMap>(), getAnalysis<LiveIntervals>());

  DEBUG(dbgs() << "Before simple register allocation:\n";F.dump());

  for (MachineFunction::iterator I = F.begin(), E =F.end(); I != E; ++I) {
    bool InCtrlBundle = false;
    typedef MachineBasicBlock::instr_iterator instr_it;

    for (instr_it II = I->instr_begin(), IE = I->instr_end(); II != IE; ++II) {
      if (II->getOpcode() == VTM::CtrlStart)
        InCtrlBundle = true;
      else if (II->getOpcode() == VTM::CtrlEnd)
        InCtrlBundle = false;
      else if (InCtrlBundle)
        handleCtrlOp(II);
    }
  }

  CompRegEdgeWeight RegWeight(this);

  bool SomethingBound = !DisableFUSharing;
  // Reduce the Compatibility Graphs
  while (SomethingBound) {
    DEBUG(dbgs() << "Going to reduce CompGraphs\n");
    SomethingBound = reduceCompGraph(RegWeight);
  }

  // Bind the Compatibility Graphs
  bindCompGraph();

  // Run rewriter
  LIS->addKillFlags();
  addMBBLiveIns(MF);
  rewrite();
  VRM->clearAllVirt();
  releaseMemory();

  DEBUG(dbgs() << "After simple register allocation:\n";
        //printVMF(dbgs(), F);
  );
  MRI->leaveSSA();
  MRI->invalidateLiveness();
  return true;
}

void VRASimple::rewrite() {
  for (MachineFunction::iterator MBBI = MF->begin(), MBBE = MF->end();
       MBBI != MBBE; ++MBBI) {
    DEBUG(MBBI->print(dbgs(), LIS->getSlotIndexes()));
    for (MachineBasicBlock::instr_iterator
           MII = MBBI->instr_begin(), MIE = MBBI->instr_end(); MII != MIE;) {
      MachineInstr *MI = MII;
      ++MII;

      for (MachineInstr::mop_iterator MOI = MI->operands_begin(),
           MOE = MI->operands_end(); MOI != MOE; ++MOI) {
        MachineOperand &MO = *MOI;

        // Make sure MRI knows about registers clobbered by regmasks.
        if (MO.isRegMask())
          MRI->addPhysRegsUsedFromRegMask(MO.getRegMask());

        if (!MO.isReg() || !TargetRegisterInfo::isVirtualRegister(MO.getReg()))
          continue;
        unsigned VirtReg = MO.getReg();
        unsigned PhysReg = VRM->getPhys(VirtReg);
        // assert(PhysReg != VirtRegMap::NO_PHYS_REG &&
        //        "Instruction uses unmapped VirtReg");
        if (PhysReg == VirtRegMap::NO_PHYS_REG) continue;
        //assert(!Reserved.test(PhysReg) && "Reserved register assignment");

        // Preserve semantics of sub-register operands.
        assert(!MO.getSubReg() && "Unexpected sub-regster!");
        // Rewrite. Note we could have used MachineOperand::substPhysReg(), but
        // we need the inlining here.
        MO.setReg(PhysReg);
      }

      DEBUG(dbgs() << "> " << *MI);

      // TODO: remove any identity copies.
    }
  }

  // Tell MRI about physical registers in use.
  for (unsigned Reg = 1, RegE = TRI->getNumRegs(); Reg != RegE; ++Reg)
    if (!MRI->reg_nodbg_empty(Reg))
      MRI->setPhysRegUsed(Reg);
}

void VRASimple::addMBBLiveIns(MachineFunction *MF) {
  typedef SmallVector<MachineBasicBlock*, 8> MBBVec;
  MBBVec liveInMBBs;
  MachineBasicBlock &entryMBB = *MF->begin();

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    unsigned PhysReg = VRM->getPhys(Reg);
    // Virtual register not bound.
    if (PhysReg == VirtRegMap::NO_PHYS_REG) continue;

    LiveInterval &LI = LIS->getInterval(Reg);

    for (LiveInterval::iterator RI = LI.begin(), RE = LI.end(); RI != RE; ++RI){
      // Find the set of basic blocks which this range is live into...
      liveInMBBs.clear();
      if (!LIS->findLiveInMBBs(RI->start, RI->end, liveInMBBs)) continue;

      // And add the physreg for this interval to their live-in sets.
      for (MBBVec::iterator I = liveInMBBs.begin(), E = liveInMBBs.end();
           I != E; ++I) {
        MachineBasicBlock *MBB = *I;
        if (MBB == &entryMBB || MBB->isLiveIn(PhysReg)) continue;

        MBB->addLiveIn(PhysReg);
      }
    }
  }
}

void VRASimple::mergeEquLIs(EquRegClasses &EquLIs) {
  // Merge the equivalence liveintervals.
  typedef EquRegClasses::iterator equ_li_it;
  typedef EquRegClasses::member_iterator mem_it;
  for (equ_li_it I = EquLIs.begin(), E = EquLIs.end(); I != E; ++I) {
    if (!I->isLeader()) continue;

    mem_it MI = EquLIs.member_begin(I);
    LiveInterval *LeaderLI = 0;

    while (MI != EquLIs.member_end()) {
      LiveInterval *CurLI = getInterval(*MI++);
      if (!CurLI) continue;

      if (LeaderLI)
        // FIXME: Why the identical datapath ops overlap?
        mergeLI(CurLI, LeaderLI, true);
      else
        LeaderLI = CurLI;
    }
  }
}

void VRASimple::mergeIdenticalDatapath(LiveInterval *LI) {
  typedef MachineRegisterInfo::use_iterator use_it;
  DatapathOpMap Users;
  EquRegClasses EquLIs;

  for (use_it I = MRI->use_begin(LI->reg), E = MRI->use_end(); I != E; ++I) {
    if (I.getOperand().isImplicit()) continue;

    MachineInstr *MI = &*I;
    if (VInstrInfo::isCtrlBundle(MI)) continue;

    // Datapath op should define and only define its result at operand 0.
    unsigned NewReg = MI->getOperand(0).getReg();
    // Ignore the dead datapath ops.
    if (NewReg == 0 || MRI->use_empty(NewReg)) continue;

    std::pair<DatapathOpMap::iterator, bool> p =
      Users.insert(std::make_pair(MI, NewReg));
    // Merge all identical datapath ops.
    if (!p.second && NewReg != p.first->second)
      EquLIs.unionSets(NewReg, p.first->second);
  }

  // Merge the equivalence liveintervals.
  mergeEquLIs(EquLIs);
}

void VRASimple::mergeLI(LiveInterval *FromLI, LiveInterval *ToLI,
                        bool AllowOverlap) {
  assert(FromLI != ToLI && "Why we merge the same LI?");
  assert((AllowOverlap || !ToLI->overlaps(*FromLI)) && "Cannot merge LI!");
  assert(getBitWidthOf(FromLI->reg) == getBitWidthOf(ToLI->reg)
         && "Cannot merge LIs difference with bit width!");
  // Merge two live intervals.
  while (ToLI->getNumValNums() > 1) {
    ToLI->MergeValueNumberInto(ToLI->getValNumInfo(1),
                               ToLI->getValNumInfo(0));
    ToLI->RenumberValues(*LIS);
  }

  ToLI->MergeRangesInAsValue(*FromLI, ToLI->getValNumInfo(0));
  MRI->replaceRegWith(FromLI->reg, ToLI->reg);
  ++LIMerged;
  // Merge the identical datapath ops.
  mergeIdenticalDatapath(ToLI);
}

template<class CompEdgeWeight>
bool VRASimple::reduceCompGraph(CompEdgeWeight &C) {
  SmallVector<LiveInterval*, 8> LongestPath, MergedLIs;
  RegLIG.updateEdgeWeight(C);

  bool AnyReduced = false;

  for (int PathWeight = RegLIG.findLongestPath(LongestPath, true);
       PathWeight > 0; PathWeight = RegLIG.findLongestPath(LongestPath, true)) {
    DEBUG(dbgs() << "// longest path in graph: {"
                 << TRI->getRegClass(RegLIG.ID)->getName() << "} weight:"
                 << PathWeight << "\n";
    for (unsigned i = 0; i < LongestPath.size(); ++i) {
      LiveInterval *LI = LongestPath[i];
      dbgs() << *LI << " bitwidth:" << getBitWidthOf(LI->reg) << '\n';
    });

    LiveInterval *RepLI = LongestPath.pop_back_val();

    // Merge the other LIs in the path to the first LI.
    while (!LongestPath.empty())
      mergeLI(LongestPath.pop_back_val(), RepLI);
    // Add the merged LI back to the graph later.
    MergedLIs.push_back(RepLI);
    AnyReduced = true;
  }

  // Re-add the merged LI to the graph.
  while (!MergedLIs.empty())
    RegLIG.GetOrCreateNode(MergedLIs.pop_back_val());

  return AnyReduced;
}

void VRASimple::bindCompGraph() {
  unsigned RC = RegLIG.ID;
  typedef LICGraph::iterator iterator;

  for (iterator I = RegLIG.begin(), E = RegLIG.end(); I != E; ++I) {
    LiveInterval *LI = (*I)->get();
    assign(*LI, TRI->allocatePhyReg(RC, getBitWidthOf(LI->reg)));
  }
}
