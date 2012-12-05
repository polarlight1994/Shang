//===--- BitLevelDelayInfo.cpp - Bit-level delay estimator  -----*- C++ -*-===//
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
#include "vtm/BitLevelDelayInfo.h"
#include "vtm/VerilogBackendMCTargetDesc.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "detail-latency"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
DisableBLC("vtm-disable-blc",
          cl::desc("Disable bit-level chaining"),
          cl::init(false));

INITIALIZE_PASS_BEGIN(BitLevelDelayInfo, "detail-latency-info",
                      "Calculating the latency of instructions",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(MachineBasicBlockTopOrder)
INITIALIZE_PASS_END(BitLevelDelayInfo, "detail-latency-info",
                    "Calculating the latency of instructions",
                    false, true)

typedef BitLevelDelayInfo::DepLatInfoTy DepLatInfoTy;
typedef BitLevelDelayInfo::BDInfo BDInfo;

char BitLevelDelayInfo::ID = 0;
const unsigned BitLevelDelayInfo::LatencyScale = 256;
const unsigned BitLevelDelayInfo::LatencyDelta = 1;

static inline unsigned scaledCP(unsigned Num = 1) {
  return VFUs::ClockPeriod() * BitLevelDelayInfo::LatencyScale * Num;
}

static inline unsigned roundUpToScaledCPMultiple(unsigned Latency) {
  return ((Latency + scaledCP() - 1) / scaledCP()) * scaledCP();
}

static inline unsigned roundUpToCP(unsigned Latency) {
  return ((Latency + scaledCP() - 1) / scaledCP());
}

static inline unsigned roundDownToCP(unsigned Latency) {
  return (Latency / scaledCP());
}

static inline unsigned scaledLUTLatency() {
  return BitLevelDelayInfo::LatencyScale;
}

static inline unsigned ensureElementalLatency(unsigned Latency) {
  if (Latency > BitLevelDelayInfo::LatencyDelta)
    return std::max(Latency, scaledLUTLatency());

  return Latency;
}

static inline unsigned scaleUp(unsigned NumLogicLevels) {
  return NumLogicLevels * BitLevelDelayInfo::LatencyScale;
}

static inline unsigned scaleToLogicLevels(unsigned Delay) {
  return (Delay + BitLevelDelayInfo::LatencyScale - 1)
          / BitLevelDelayInfo::LatencyScale;
}

static inline unsigned scaledDetalLatency(const MachineInstr *MI) {
  return scaleUp(VInstrInfo::getNumLogicLevels(MI));
}

// Ensure all latency are not smaller than the elemental latency,
// i.e. the latency of a single LUT.
static inline BDInfo ensureElementalLatency(BDInfo L) {
  return BDInfo(ensureElementalLatency(L.MSBDelay),
                ensureElementalLatency(L.LSBDelay));
}

BitLevelDelayInfo::BitLevelDelayInfo() : MachineFunctionPass(ID), MRI(0) {
  initializeBitLevelDelayInfoPass(*PassRegistry::getPassRegistry());
}

Pass *llvm::createBitLevelDelayInfoPass() {
  return new BitLevelDelayInfo();
}

void BitLevelDelayInfo::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
  AU.addRequiredID(MachineBasicBlockTopOrderID);
  AU.setPreservesAll();
}

unsigned BitLevelDelayInfo::getStepsToFinish(const MachineInstr *MI) const {
  return roundUpToCP(getMaxLatency(MI));
}

unsigned BitLevelDelayInfo::getNumCPCeil(DepLatInfoTy::value_type v) {
  return roundUpToCP(v.second.getCriticalDelay());
}

unsigned BitLevelDelayInfo::getNumCPFloor(DepLatInfoTy::value_type v) {
  return roundDownToCP(v.second.getMinDelay());
}

unsigned BitLevelDelayInfo::getChainedCPs(const MachineInstr *SrcInstr,
                                          const MachineInstr *DstInstr) const {
  return roundUpToCP(getChainedLatency(SrcInstr, DstInstr));
}

static void updateLatency(DepLatInfoTy &CurLatInfo, InstPtrTy Src,
                          BDInfo Latency) {
  unsigned MSBLatency = Latency.MSBDelay, LSBLatency = Latency.LSBDelay;
  // Latency from a control operation is simply the latency of the control
  // operation.
  // We may have dependency like:
  //  other op
  //    |   \
  //    |   other op
  //    |   /
  // current op
  // We should update the latency if we get a bigger latency.
  DepLatInfoTy::mapped_type &V = CurLatInfo[Src];
  unsigned &OldLSBLatency = V.LSBDelay;
  OldLSBLatency = std::max(OldLSBLatency, LSBLatency);
  //assert(LSBLatency <= MSBLatency && "Broken latency pair!");
  unsigned &OldMSBLatency = V.MSBDelay;
  OldMSBLatency = std::max(OldMSBLatency, MSBLatency);
}

static
BDInfo getMSB2LSBLatency(BDInfo SrcLatency, BDInfo Inc, unsigned BitInc) {
  unsigned SrcMSBLatency = SrcLatency.MSBDelay,
           SrcLSBLatency = SrcLatency.LSBDelay;
  unsigned MSBInc = Inc.MSBDelay, LSBInc = Inc.LSBDelay;

  unsigned MSBLatency = MSBInc + SrcMSBLatency;
  unsigned LSBLatency = std::max(BitInc + SrcLSBLatency,
                                 LSBInc + SrcMSBLatency);
  return BDInfo(MSBLatency, LSBLatency);
}

static
BDInfo getCmpLatency(BDInfo SrcLatency, BDInfo Inc, unsigned BitInc) {
  BDInfo LatInfo = getMSB2LSBLatency(SrcLatency, Inc, BitInc);
  // We need to get the worst delay because the cmps only have 1 bit output.
  unsigned WorstLat = std::max(LatInfo.MSBDelay, LatInfo.LSBDelay);
  return BDInfo(WorstLat);
}

static
BDInfo getLSB2MSBLatency(BDInfo SrcLatency, BDInfo Inc, unsigned BitInc) {
  unsigned SrcMSBLatency = SrcLatency.MSBDelay,
           SrcLSBLatency = SrcLatency.LSBDelay;
  unsigned MSBInc = Inc.MSBDelay, LSBInc = Inc.LSBDelay;

  unsigned MSBLatency = std::max(MSBInc + SrcLSBLatency,
                                 BitInc + SrcMSBLatency);
  unsigned LSBLatency = LSBInc + SrcLSBLatency;
  return BDInfo(MSBLatency, LSBLatency);
}

static
BDInfo getWorstLatency(BDInfo SrcLatency, BDInfo Inc, unsigned /*BitInc*/) {
  unsigned SrcMSBLatency = SrcLatency.MSBDelay,
           SrcLSBLatency = SrcLatency.LSBDelay;
  unsigned MSBInc = Inc.MSBDelay, LSBInc = Inc.LSBDelay;

  unsigned MSBLatency = MSBInc + SrcMSBLatency;
  unsigned LSBLatency = LSBInc + SrcLSBLatency;
  unsigned WorstLatency = std::max(MSBLatency, LSBLatency);
  return BDInfo(WorstLatency);
}

static
BDInfo getParallelLatency(BDInfo SrcLatency, BDInfo Inc, unsigned /*BitInc*/) {
  unsigned SrcMSBLatency = SrcLatency.MSBDelay,
           SrcLSBLatency = SrcLatency.LSBDelay;
  unsigned MSBInc = Inc.MSBDelay, LSBInc = Inc.LSBDelay;
  unsigned MSBLatency = MSBInc + SrcMSBLatency;
  unsigned LSBLatency = LSBInc + SrcLSBLatency;

  return BDInfo(MSBLatency, LSBLatency);
}

template<typename FuncTy>
static void accumulateDatapathLatency(DepLatInfoTy &CurLatInfo,
                                      const DepLatInfoTy *SrcLatInfo,
                                      BDInfo Inc, unsigned BitInc, FuncTy F) {
  typedef DepLatInfoTy::const_iterator src_it;
  // Compute minimal delay for all possible pathes.
  for (src_it I = SrcLatInfo->begin(), E = SrcLatInfo->end(); I != E; ++I) {
    DEBUG(dbgs() << "DELAY-ESTIMATION-DEBUG: SRC " << I->first.getOpaqueValue()
           << " Original-delay: " << scaleToLogicLevels(I->second.MSBDelay)
           << ", " << scaleToLogicLevels(I->second.LSBDelay) << " Inc: "
           << scaleToLogicLevels(Inc.MSBDelay) << ", "
           << scaleToLogicLevels(Inc.LSBDelay) << " BitInc: "
           << scaleToLogicLevels(BitInc));
    updateLatency(CurLatInfo, I->first, F(I->second, Inc, BitInc));
    DEBUG(dbgs() << " After accumulate: "
      << scaleToLogicLevels(CurLatInfo.find(I->first)->second.MSBDelay) << ", "
      << scaleToLogicLevels(CurLatInfo.find(I->first)->second.LSBDelay) << '\n');
  }
}

static bool NeedExtraStepToLatchResult(const MachineInstr *MI,
                                       const MachineRegisterInfo &MRI,
                                       unsigned Latency) {
  if (MI->getNumOperands() == 0) return false;

  const MachineOperand &MO = MI->getOperand(0);
  if (!MO.isReg() || !MO.isDef()) return false;

  assert(MO.getReg() && "Broken instruction defining register 0!");
  return Latency != 0.0f && VInstrInfo::isWriteUntilFinish(MI->getOpcode())
         && !MRI.use_empty(MO.getReg());
}

static BDInfo getBitSliceLatency(unsigned OperandSize,
                                    unsigned UB, unsigned LB,
                                    BDInfo SrcLatency) {
  unsigned SrcMSBLatency = SrcLatency.MSBDelay,
           SrcLSBLatency = SrcLatency.LSBDelay;
  assert(OperandSize && "Unexpected zero size operand!");
  // Time difference between MSB and LSB.
  // unsigned MSB2LSBDelta = SrcMSBLatency - SrcLSBLatency;
  // unsigned DeltaPerBit = MSB2LSBDelta / OperandSize;
  // (UB * SrcMSBLatency) / OperandSize - (UB * SrcLSBLatency) / OperandSize
  // => DeltaPerBit * UB
  // (LB * SrcMSBLatency) / OperandSize - (LB * SrcLSBLatency) / OperandSize
  // => DeltaPerBit * LB
  // Compute the latency of LSB/MSB by assuming the latency is increasing linear
  unsigned MSBLatency = SrcLSBLatency + (UB * SrcMSBLatency) / OperandSize
                                      - (UB * SrcLSBLatency) / OperandSize,
           LSBLatency = SrcLSBLatency + (LB * SrcMSBLatency) / OperandSize
                                      - (LB * SrcLSBLatency) / OperandSize;
  return BDInfo(MSBLatency, LSBLatency);
}

static unsigned adjustChainedLatency(unsigned Latency, unsigned SrcOpcode,
                                  unsigned DstOpcode) {
  bool SrcWriteUntilFInish = VInstrInfo::isWriteUntilFinish(SrcOpcode);
  bool DstReadAtEmit = VInstrInfo::isReadAtEmit(DstOpcode);

  const unsigned Delta = BitLevelDelayInfo::LatencyDelta;

  if (DstReadAtEmit && SrcWriteUntilFInish) {
    if (SrcOpcode == VTM::VOpMvPhi) {
      assert((DstOpcode == TargetOpcode::PHI || DstOpcode == VTM::VOpMvPhi
              || VInstrInfo::getDesc(DstOpcode).isTerminator())
             && "VOpMvPhi should only used by PHIs or terminators!!");
      // The latency from VOpMvPhi to PHI is exactly 0, because the VOpMvPhi is
      // simply identical to the PHI at next iteration.
      return 0;
    } else
      // If the edge is reg->reg, the result is ready after the clock edge, add
      // a delta to make sure DstInstr not schedule to the moment right at the
      // SrcInstr finish
      // Round up the latency.
      return roundUpToScaledCPMultiple(Latency) + Delta;
  }

  // If the value is written to register, it has a delta latency
  if (SrcWriteUntilFInish) return Latency + Delta;

  // Chain the operations if dst not read value at the edge of the clock.
  return std::max(0, int(Latency) - int(Delta));
}

void BitLevelDelayInfo::buildLatenciesToCopy(const MachineInstr *MI,
                                             DepLatInfoTy &Info) {
  buildDepLatInfo<false>(MI, Info, 0, 0.0, VTM::VOpReadFU);
}

unsigned BitLevelDelayInfo::computeAndCacheLatencyFor(const MachineInstr *MI) {
  unsigned TotalLatency = 0;

  if (MI->getOpcode() == VTM::VOpBitSlice && MI->getOperand(1).isReg()) {
    unsigned SrcReg = MI->getOperand(1).getReg();
    MachineInstr *BitSliceSrc = MRI->getVRegDef(SrcReg);
    assert(BitSliceSrc && "The source MachineInstr for BitSlice not found!");
    // Update SrcMSBLatency and SrcLSBLatency according to the upper bound
    // and the lower bound of the bitslice.
    unsigned UB = MI->getOperand(2).getImm(), LB = MI->getOperand(3).getImm();
    // Create the entry for the bitslice, the latency of the bitslice is the
    // same as the scaled BitSliceSrc.
    BDInfo Lat = getLatencyToDst<false>(BitSliceSrc, VTM::VOpMove, UB, LB);
    TotalLatency = Lat.getCriticalDelay();
  } else
    TotalLatency = scaledDetalLatency(MI);

  // Remember the latency from all MI's dependence leaves.
  CachedLatencies.insert(std::make_pair(MI, TotalLatency));
  return TotalLatency;
}

bool BitLevelDelayInfo::isAddOrMult(const MachineInstr *MI) {
  switch (MI->getOpcode()) {
  default: break;
  case VTM::VOpAdd_c:
  case VTM::VOpMultLoHi_c:
  case VTM::VOpMult_c:
  case VTM::VOpAdd:
  case VTM::VOpMult:
  case VTM::VOpMultLoHi:
    return true;

  case VTM::VOpBitSlice:
    // Forward the latency from the source of the bitslice, and increase the
    // MSBLatency and LSBLatency according to the upper bound and lowerbound
    // of the bitslice.
    if (MI->getOperand(1).isReg()) {
      unsigned SrcReg = MI->getOperand(1).getReg();
      MachineInstr *BitSliceSrc = MRI->getVRegDef(SrcReg);
      return isAddOrMult(BitSliceSrc);
    }
  }

  return false;
}

template<bool IsCtrlDep>
BDInfo BitLevelDelayInfo::getLatencyToDst(const MachineInstr *SrcMI,
                                          unsigned DstOpcode,
                                          unsigned UB, unsigned LB) {
  unsigned CriticalDelay = getCachedLatencyResult(SrcMI);
  BDInfo Result = BDInfo(CriticalDelay);
  if (!IsCtrlDep || NeedExtraStepToLatchResult(SrcMI, *MRI, CriticalDelay)) {
    Result.LSBDelay = Result.MSBDelay
      = adjustChainedLatency(CriticalDelay, SrcMI->getOpcode(), DstOpcode);
    // If we are only reading the lower part of the result of SrcMI, and the
    // LSB of the result of SrcMI are available before SrcMI completely finish,
    // we can read the subword before SrcMI finish.
    if (UB && isAddOrMult(SrcMI)) {
      unsigned SrcSize = VInstrInfo::getBitWidth(SrcMI->getOperand(0));
      // Scale up the latency by SrcSize.
      Result.LSBDelay = std::max<int>(int(Result.MSBDelay) -
                                      int(scaledLUTLatency() * (SrcSize - 1)),
                                      scaledLUTLatency());
      // DirtyHack: Ignore the invert flag.
      if (SrcSize != 1 && UB != 3) {
        assert(UB <= SrcSize && UB > LB  && "Bad bitslice!");
        Result = getBitSliceLatency(SrcSize, UB, LB, Result);
      }

    }
  } else
    // IsCtrlDep
    Result = BDInfo(std::max(0, int(CriticalDelay) - int(LatencyDelta)));

  // Force synchronize the bit-delays if bit-level chaining is disabled.
  if (DisableBLC)
    Result = BDInfo(std::max(Result.MSBDelay, Result.LSBDelay));

  return Result;
}

template<bool IsCtrlDep>
void BitLevelDelayInfo::buildDepLatInfo(const MachineInstr *SrcMI,
                                        DepLatInfoTy &CurLatInfo,
                                        unsigned UB, unsigned LB,
                                        unsigned DstOpcode){
  const DepLatInfoTy *SrcLatInfo = getDepLatInfo(SrcMI);
  assert(SrcLatInfo && "SrcMI not visited yet?");

  BDInfo SrcLatency = getLatencyToDst<IsCtrlDep>(SrcMI, DstOpcode, UB, LB);

  // Try to compute the per-bit latency.
  unsigned BitLatency = 0;
  if (unsigned Size = UB - LB) {
    BitLatency = abs(int(SrcLatency.MSBDelay/Size)
                     - int(SrcLatency.LSBDelay/Size));
    BitLatency = ensureElementalLatency(BitLatency);
  }

  SrcLatency = ensureElementalLatency(SrcLatency);
  unsigned Opcode = SrcMI->getOpcode();
  bool isCtrl = VInstrInfo::isControl(SrcMI->getOpcode());

  switch (Opcode) {
  default:
    updateLatency(CurLatInfo, SrcMI, SrcLatency);
    if (!isCtrl)
      accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcLatency, BitLatency,
                                getWorstLatency);
    return;
    // Result bits are computed from LSB to MSB.
  case VTM::VOpAdd_c:
  case VTM::VOpMultLoHi_c:
  case VTM::VOpMult_c:
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcLatency, BitLatency,
                              getLSB2MSBLatency);
    /* FALL THOUGH */
  case VTM::VOpAdd:
  case VTM::VOpMult:
  case VTM::VOpMultLoHi:
    updateLatency(CurLatInfo, SrcMI, SrcLatency);
    return;
  case VTM::VOpBitSlice:
    // Forward the latency from the source of the bitslice, and increase the
    // MSBLatency and LSBLatency according to the upper bound and lowerbound
    // of the bitslice.
    if (SrcMI->getOperand(1).isReg()) {
      unsigned SrcReg = SrcMI->getOperand(1).getReg();
      MachineInstr *BitSliceSrc = MRI->getVRegDef(SrcReg);
      assert(BitSliceSrc && "The source MachineInstr for BitSlice not found!");
      // Update SrcMSBLatency and SrcLSBLatency according to the upper bound
      // and the lower bound of the bitslice.
      UB = SrcMI->getOperand(2).getImm();
      LB = SrcMI->getOperand(3).getImm();
      // Create the entry for the bitslice, the latency of the bitslice is the
      // same as the scaled BitSliceSrc.
      SrcLatency = getLatencyToDst<IsCtrlDep>(BitSliceSrc, DstOpcode, UB, LB);
      SrcLatency = ensureElementalLatency(SrcLatency);
      updateLatency(CurLatInfo, SrcMI, SrcLatency);

      buildDepLatInfo<IsCtrlDep>(BitSliceSrc, CurLatInfo, UB, LB, DstOpcode);
      return;
    }
    /* FALL THOUGH */
  // Each bits are compute independently.
  case VTM::VOpSel:
  case VTM::VOpLUT:
  case VTM::VOpAnd:
  case VTM::VOpOr:
  case VTM::VOpXor:
  case VTM::VOpNot:
  case VTM::VOpBitCat:
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcLatency, BitLatency,
                              getParallelLatency);
    updateLatency(CurLatInfo, SrcMI, SrcLatency);
    return;
  case VTM::VOpICmp_c:
    // The result of ICmp is propagating from MSB to LSB.
    SrcLatency.MSBDelay = BitLatency;
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcLatency, BitLatency,
                              getCmpLatency);
    /* FALL THOUGH */
  case VTM::VOpICmp:
    // Result bits are computed from MSB to LSB.
    updateLatency(CurLatInfo, SrcMI, SrcLatency);
    return;
  }
}

void BitLevelDelayInfo::visitAllUses(const MachineInstr *MI,
                                     DepLatInfoTy &CurLatInfo) {
  unsigned Opcode = MI->getOpcode();
  // Iterate from use to define, ignore the the incoming value of PHINodes.
  // Because the incoming value may be not visited yet.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI->getOperand(i);

    // Only care about a use register.
    if (!MO.isReg() || MO.isDef() || MO.getReg() == 0)
      continue;

    unsigned SrcReg = MO.getReg();
    MachineInstr *SrcMI = MRI->getVRegDef(SrcReg);
    assert(SrcMI && "Virtual register use without define!");

    // Do we ignore phi as dependence? Also ignore self loop.
    assert(SrcMI != MI && "Unexpected self-loop!");

    unsigned OpSize = VInstrInfo::getBitWidth(MO);

    if (Opcode == VTM::VOpBitSlice) {
      // Directly copy the dependencies latency information, because when
      // calculating latency, we treat it as the alias of SrcMI, exepct the
      // latencies are scaled according to the lower bound and upper bound of
      // the bitslice.
      CurLatInfo = *getDepLatInfo(SrcMI);
      // The latency of SrcMI is included into the latency of the bitslice.
      // Hence we need to set the latency of SrcMI to 0.0f to avoid accumulating
      // it more than once.
      CurLatInfo[SrcMI] = BDInfo(0, 0);
      continue;
    }

    buildDepLatInfo<false>(SrcMI, CurLatInfo, OpSize, 0, Opcode);
  }
}


const BitLevelDelayInfo::DepLatInfoTy &
BitLevelDelayInfo::addInstrInternal(const MachineInstr *MI,
                                    DepLatInfoTy &CurLatInfo) {
  const MachineBasicBlock *CurMBB = MI->getParent();

  if (!MI->isPHI()) visitAllUses(MI, CurLatInfo);

  // Compute the latency of MI.
  unsigned Latency = computeAndCacheLatencyFor(MI);
  bool IsControl = VInstrInfo::isControl(MI->getOpcode());

  // We will not get any latency information if a datapath operation do not
  // depends any control operation in the same BB.
  if (CurLatInfo.empty() && (!IsControl || MI->isPHI())) {
    Latency = std::max(Latency, LatencyDelta);
    CurLatInfo.insert(std::make_pair(CurMBB, BDInfo(Latency, Latency)));
  }

  return CurLatInfo;
}

void BitLevelDelayInfo::buildExitMIInfo(const MachineInstr *ExitMI,
                                        DepLatInfoTy &Info,
                                        MISetTy &MIsToWait, MISetTy &MIsToRead){
  typedef MISetTy::const_iterator exit_it;
  // Exiting directly, no need to read the result fore fore exting.
  for (exit_it I = MIsToWait.begin(), E = MIsToWait.end(); I != E; ++I)
    buildDepLatInfo<true>(*I, Info, 0, 0, ExitMI->getOpcode());

  // Exiting via data-path operation, the value need to be read before exiting.
  for (exit_it I = MIsToRead.begin(), E = MIsToRead.end(); I != E; ++I)
    buildDepLatInfo<false>(*I, Info, 0, 0, ExitMI->getOpcode());
}

unsigned BitLevelDelayInfo::getChainedLatency(const MachineInstr *SrcInstr,
                                              const MachineInstr *DstInstr) const{
  // Compute the latency correspond to detail slot.
  unsigned latency = getMaxLatency(SrcInstr);
  return adjustChainedLatency(latency, SrcInstr->getOpcode(),
                              DstInstr->getOpcode());
}

bool BitLevelDelayInfo::runOnMachineFunction(MachineFunction &MF) {
  MRI = &MF.getRegInfo();

  typedef MachineFunction::iterator iterator;
  typedef MachineBasicBlock::instr_iterator instr_iterator;
  for (iterator BI = MF.begin(), BE = MF.end(); BI != BE; ++BI)
    for (instr_iterator I = BI->instr_begin(), E = BI->instr_end(); I != E; ++I)
      addInstrInternal(I,  LatencyMap[I]);

  return false;
}

void BitLevelDelayInfo::dump() const {
  print(dbgs(), 0);
}

void BitLevelDelayInfo::print(raw_ostream &O, const Module *M) const {
  O << "DELAY-ESTIMATION: #Logic-level per clock cycles: "
    << VFUs::ClockPeriod() << '\n';
  // The critical chain of the function.
  std::pair<InstPtrTy, BDInfo> LongestChain, CriticalChain;
  unsigned CriticalLL = 0;

  typedef LatencyMapTy::const_iterator LatIt;
  for (LatIt I = LatencyMap.begin(), E = LatencyMap.end(); I != E; ++I) {
    const MachineInstr *DstMI = I->first;

    // Ignore the chain end with data-path operations.
    if (VInstrInfo::isDatapath(DstMI->getOpcode())) continue;

    typedef DepLatInfoTy::const_iterator SrcIt;
    for (SrcIt II = I->second.begin(), IE = I->second.end(); II != IE; ++II) {
      DepLatInfoTy::value_type CurChain = *II;

      printChainDelayInfo(O, "DELAY-ESTIMATION", CurChain, DstMI);

      if (CurChain.second.getCriticalDelay()
          > LongestChain.second.getCriticalDelay())
        LongestChain = CurChain;

      if (unsigned Latency = CurChain.second.getCriticalDelay()) {
        unsigned NumCycles = roundUpToCP(Latency);
        unsigned PerCyclesLL = Latency / NumCycles;

        if (CriticalLL < PerCyclesLL)  {
          CriticalLL = PerCyclesLL;
          CriticalChain = CurChain;
        }
      }
    }
  }

  printChainDelayInfo(O, "DELAY-ESTIMATION-LONGEST-CHAIN", LongestChain, 0);
  printChainDelayInfo(O, "DELAY-ESTIMATION-DELAY-CHAIN", CriticalChain, 0);
  O << "DELAY-ESTIMATION-FMAX: " << double(scaledCP()) / double(CriticalLL)
    << '\n';
}

void BitLevelDelayInfo::printChainDelayInfo(raw_ostream & O,
                                            const std::string &Prefix,
                                            const DepLatInfoTy::value_type &Lat,
                                            const MachineInstr *DstMI) {
  const InstPtrTy SrcMI = Lat.first;

  // Ignore the chain start from data-path operations.
  if (SrcMI.isMI() && VInstrInfo::isDatapath(SrcMI->getOpcode())) return;
  unsigned MSBDelay = Lat.second.MSBDelay,
           LSBDelay = Lat.second.LSBDelay;

  // Do not count the delay introduced by required control-steps.
  if (const MachineInstr *SrcCtrlMI = SrcMI) {
    MSBDelay -= scaledDetalLatency(SrcCtrlMI);
    LSBDelay -= scaledDetalLatency(SrcCtrlMI);
  }

  O << Prefix << ": From " << SrcMI.getOpaqueValue()
    << " to " << DstMI << "\nDELAY-ESTIMATION: MSB-Delay "
    << scaleToLogicLevels(Lat.second.MSBDelay)
    << "\n" << Prefix << ": LSB-Delay "
    << scaleToLogicLevels(Lat.second.LSBDelay)
    << "\n" << Prefix << ": MAX-Delay "
    << scaleToLogicLevels(Lat.second.getCriticalDelay()) << '\n';

  O << Prefix << "-JSON: { \"SRC\":\"" << SrcMI.getOpaqueValue()
    << "\", \"DST\":\"" << DstMI << "\", \"MSB\":"
    << scaleToLogicLevels(Lat.second.MSBDelay)
    << ", \"LSB\":" << scaleToLogicLevels(Lat.second.LSBDelay)
    << ", \"MAX\":"
    << scaleToLogicLevels(Lat.second.getCriticalDelay()) << "} \n";
}
