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
#include "MFDatapathContainer.h"
#include "ExternalTimingAnalysis.h"

#include "vtm/BitLevelDelayInfo.h"
#include "vtm/VerilogBackendMCTargetDesc.h"

#include "llvm/ADT/OwningPtr.h"
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
typedef BitLevelDelayInfo::delay_type delay_type;

const delay_type BitLevelDelayInfo::Delta = 0.00001;

char BitLevelDelayInfo::ID = 0;

BitLevelDelayInfo::BitLevelDelayInfo() : MachineFunctionPass(ID), MRI(0), TNL(0){
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
  return ceil(getCachedLatencyResult(MI));
}

unsigned BitLevelDelayInfo::getNumCPCeil(DepLatInfoTy::value_type v) {
  return ceil(v.second);
}

unsigned BitLevelDelayInfo::getNumCPFloor(DepLatInfoTy::value_type v) {
  return floor(v.second);
}

unsigned BitLevelDelayInfo::getChainedCPs(const MachineInstr *SrcInstr,
                                          unsigned DstOpcode) const {
  return ceil(getChainedDelay(SrcInstr, DstOpcode));
}

static void updateDelay(DepLatInfoTy &CurLatInfo, InstPtrTy Src, delay_type d) {
  // Latency from a control operation is simply the latency of the control
  // operation.
  // We may have dependency like:
  //  other op
  //    |   \
  //    |   other op
  //    |   /
  // current op
  // We should update the latency if we get a bigger latency.
  delay_type &OldDelay = CurLatInfo[Src];
  OldDelay = std::max(OldDelay, d);
}

static delay_type adjustChainedLatency(delay_type Latency, unsigned SrcOpcode,
                                       unsigned DstOpcode) {
  bool SrcWriteUntilFInish = VInstrInfo::isWriteUntilFinish(SrcOpcode);
  bool DstReadAtEmit = VInstrInfo::isReadAtEmit(DstOpcode);

  if (DstReadAtEmit && SrcWriteUntilFInish) {
    if (SrcOpcode == VTM::VOpMvPhi) {
      assert((DstOpcode == TargetOpcode::PHI || DstOpcode == VTM::VOpMvPhi
              || VInstrInfo::getDesc(DstOpcode).isTerminator())
             && "VOpMvPhi should only used by PHIs or terminators!!");
      // The latency from VOpMvPhi to PHI is exactly 0, because the VOpMvPhi is
      // simply identical to the PHI at next iteration.
      return delay_type(0);
    } else
      // If the edge is reg->reg, the result is ready after the clock edge, add
      // a delta to make sure DstInstr not schedule to the moment right at the
      // SrcInstr finish
      // Round up the latency.
      return ceil(Latency) + BitLevelDelayInfo::Delta;
  }

  // If the value is written to register, it has a delta latency
  if (SrcWriteUntilFInish) return Latency + BitLevelDelayInfo::Delta;

  // Chain the operations if dst not read value at the edge of the clock.
  return std::max(delay_type(0), Latency - BitLevelDelayInfo::Delta);
}

delay_type BitLevelDelayInfo::computeAndCacheLatencyFor(const MachineInstr *MI){
  delay_type TotalLatency = delay_type(0);

  switch (MI->getOpcode()) {
  case VTM::VOpMemTrans:
    if (VInstrInfo::mayLoad(MI))
      TotalLatency = delay_type(getFUDesc<VFUMemBus>()->getReadLatency());
    else // Need 1 cycles to disable the memory bus.
      TotalLatency = delay_type(1.0);

    // Enable single-cycle chaining with VOpMemTrans.
    TotalLatency -= BitLevelDelayInfo::Delta;
    break;
  case VTM::VOpBRAMTrans:
    // Block RAM read/write is copy-like operations and we do not need to wait
    // it.
    // DIRTYHACK: Prevent other operation being chained with the block ram access
    // by setting it's latency to 1 cycles.
    if (VInstrInfo::mayLoad(MI)) {
      TotalLatency = delay_type(1);
      // Enable single-cycle chaining with VOpMemTrans.
      TotalLatency -= BitLevelDelayInfo::Delta;
    } else // No need to wait the write finish.
      TotalLatency = delay_type(0);
    break;
  case VTM::VOpInternalCall:
    // Perfrom the call need excatly 1 cycle.
    TotalLatency = delay_type(1.0);
    // Enable single-cycle chaining with VOpInternalCall.
    TotalLatency -= BitLevelDelayInfo::Delta;
    break;

  default:
    TotalLatency = delay_type(0);
    break;
  }

  // Remember the latency from all MI's dependence leaves.
  CachedLatencies.insert(std::make_pair(MI, TotalLatency));
  return TotalLatency;
}

delay_type BitLevelDelayInfo::getChainedDelay(const MachineInstr *SrcInstr,
                                              unsigned DstOpcode) const {
  // Compute the latency correspond to detail slot.
  delay_type latency = getCachedLatencyResult(SrcInstr);
  return adjustChainedLatency(latency, SrcInstr->getOpcode(), DstOpcode);
}

void BitLevelDelayInfo::addDelayForPath(unsigned SrcReg, const MachineInstr *MI,
                                        DepLatInfoTy &CurDelayInfo,
                                        delay_type PathDelay) {
  if (SrcReg == 0) {
    updateDelay(CurDelayInfo, MI->getParent(), PathDelay);
    return;
  }

  MachineInstr *SrcMI = MRI->getVRegDef(SrcReg);
  assert(SrcMI && "Virtual register use without define!");

  addDelayForPath(SrcMI, MI, CurDelayInfo, PathDelay);
}

static unsigned getRegNum(const VASTMachineOperand *MO) {
  if (MO->getMO().isReg()) return MO->getMO().getReg();

  return 0;
}

void BitLevelDelayInfo::addDelayForPath(const MachineInstr *SrcMI,
                                        const MachineInstr *MI,
                                        DepLatInfoTy &CurDelayInfo,
                                        delay_type PathDelay) {
  // Do we ignore phi as dependence? Also ignore self loop.
  assert(SrcMI != MI && "Unexpected self-loop!");

  unsigned SrcOpCode = SrcMI->getOpcode();

  // The delay of control operation is handle here.
  if (VInstrInfo::isControl(SrcOpCode)) {
    updateDelay(CurDelayInfo, SrcMI, PathDelay + getCachedLatencyResult(SrcMI));
    return;
  }

  assert(SrcMI->getOperand(0).isDef() && "Broken datapath MachineInstr!");
  VASTValue *Src = (*TNL)->lookupExpr(SrcMI->getOperand(0).getReg()).get();
  // The source expression tree maybe folded by constant folding, in this case
  // the path though does not exist.
  if (TNL->src_empty(Src)) {
    // Try to add delay from the input port of the netlist.
    if (VASTMachineOperand *VMO = dyn_cast<VASTMachineOperand>(Src)) {
      const MachineOperand &MO = VMO->getMO();
      if (MO.isReg()) {
        addDelayForPath(MO.getReg(), MI, CurDelayInfo, PathDelay);
        return;
      }
    }

    addDelayForPath(unsigned(0), MI, CurDelayInfo, PathDelay);
    return;
  }

  // Get the datapath delay from the TimingNetlist.
  typedef TimingNetlist::src_iterator it;
  for (it I = TNL->src_begin(Src), E = TNL->src_end(Src); I != E; ++I)
    addDelayForPath(getRegNum(I->first), MI, CurDelayInfo, PathDelay +  I->second);
}

void BitLevelDelayInfo::buildDelayMatrix(const MachineInstr *MI) {
  DepLatInfoTy &CurDelayInfo = LatencyMap[MI];

  // Compute the latency of MI.
  computeAndCacheLatencyFor(MI);

  unsigned Opcode = MI->getOpcode();
  if (Opcode == VTM::PHI) return;

  bool NoRegOperand = true;

  // Iterate from use to define, ignore the the incoming value of PHINodes.
  // Because the incoming value may be not visited yet.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI->getOperand(i);

    // Only care about a use register.
    if (!MO.isReg() || MO.isDef() || MO.getReg() == 0)
      continue;    

    MachineInstr *SrcMI = MRI->getVRegDef(MO.getReg());
    assert(SrcMI && "Virtual register use without define!");

    addDelayForPath(SrcMI, MI, CurDelayInfo);

    updateDelay(CurDelayInfo, SrcMI, 0);
    NoRegOperand = false;
  }

  // Make sure the datapath MachineInstr is connected to something.
  if (NoRegOperand && VInstrInfo::isDatapath(Opcode))
    updateDelay(CurDelayInfo, MI->getParent(), delay_type(0));

}

void BitLevelDelayInfo::buildExitMIInfo(const MachineInstr *SSnk,
                                        DepLatInfoTy &Info, MISetTy &ExitMIs) {
  typedef MISetTy::const_iterator exit_it;

  for (exit_it I = ExitMIs.begin(), E = ExitMIs.end(); I != E; ++I) {
    const MachineInstr *ExitMI = *I;

    addDelayForPath(ExitMI, SSnk, Info);
  }
}

bool BitLevelDelayInfo::runOnMachineFunction(MachineFunction &MF) {
  MRI = &MF.getRegInfo();

  assert(TNL == 0 && "Last TNL not release!");
  TNL = new TimingNetlist(MF.getFunction()->getName(), MRI);

  typedef MachineFunction::iterator iterator;
  typedef MachineBasicBlock::instr_iterator instr_iterator;
  for (iterator BI = MF.begin(), BE = MF.end(); BI != BE; ++BI)
    for (instr_iterator I = BI->instr_begin(), E = BI->instr_end(); I != E; ++I)
      TNL->addInstrToDatapath(I);

  ExternalTimingAnalysis::runTimingAnalysis(*TNL);

  for (iterator BI = MF.begin(), BE = MF.end(); BI != BE; ++BI)
    for (instr_iterator I = BI->instr_begin(), E = BI->instr_end(); I != E; ++I){
      MachineInstr *MI = I;
      buildDelayMatrix(MI);
    }

  return false;
}

void BitLevelDelayInfo::dump() const {
  print(dbgs(), 0);
}

void BitLevelDelayInfo::print(raw_ostream &O, const Module *M) const {
  O << "DELAY-ESTIMATION: #Logic-level per clock cycles: "
    << VFUs::ClockPeriod() << '\n';
  // The critical chain of the function.
  std::pair<InstPtrTy, delay_type> LongestChain, CriticalChain;

  typedef LatencyMapTy::const_iterator LatIt;
  for (LatIt I = LatencyMap.begin(), E = LatencyMap.end(); I != E; ++I) {
    const MachineInstr *DstMI = I->first;

    // Ignore the chain end with data-path operations.
    if (VInstrInfo::isDatapath(DstMI->getOpcode())) continue;

    typedef DepLatInfoTy::const_iterator SrcIt;
    for (SrcIt II = I->second.begin(), IE = I->second.end(); II != IE; ++II) {
      DepLatInfoTy::value_type CurChain = *II;

      printChainDelayInfo(O, "DELAY-ESTIMATION", CurChain, DstMI);
    }
  }

  printChainDelayInfo(O, "DELAY-ESTIMATION-LONGEST-CHAIN", LongestChain, 0);
  printChainDelayInfo(O, "DELAY-ESTIMATION-DELAY-CHAIN", CriticalChain, 0);
}

void BitLevelDelayInfo::printChainDelayInfo(raw_ostream & O,
                                            const std::string &Prefix,
                                            const DepLatInfoTy::value_type &Lat,
                                            const MachineInstr *DstMI) const {
  const InstPtrTy SrcMI = Lat.first;

  // Ignore the chain start from data-path operations.
  if (SrcMI.isMI() && VInstrInfo::isDatapath(SrcMI->getOpcode())) return;

  delay_type Delay = Lat.second;

  // Do not count the delay introduced by required control-steps.
  if (const MachineInstr *SrcCtrlMI = SrcMI)
    Delay -= getCachedLatencyResult(SrcCtrlMI);


  O << Prefix << ": From " << SrcMI.getOpaqueValue()
    << " to " << DstMI << "\nDELAY-ESTIMATION: Delay "
    << Delay << '\n';

  O << Prefix << "-JSON: { \"SRC\":\"" << SrcMI.getOpaqueValue()
    << "\", \"DST\":\"" << DstMI << "\", \"Delay\":"
    << Delay << "} \n";
}

void BitLevelDelayInfo::reset() {
  if (TNL) {
    delete TNL;
    TNL = 0;
  }
  
  LatencyMap.clear();
  clearCachedLatencies();
}
