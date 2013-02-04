//===- DatapathCodeMotion.cpp - Move the data-path operations ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the VTM implementation of the DatapathCodeMotion pass.
//
//===----------------------------------------------------------------------===//

#include "shang/Utilities.h"

/*
namespace llvm {
bool hoistDatapathOp(MachineInstr *MI, MachineDominatorTree  *DT,
                     MachineRegisterInfo *MRI) {
  assert(VInstrInfo::isDatapath(MI->getOpcode()) && "Expect datapath operation!");

  MachineBasicBlock *MBBToHoist = DT->getRoot();
  MachineBasicBlock *CurMBB = MI->getParent();

  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI->getOperand(i);

    if (!MO.isReg() || MO.isDef() || !MO.getReg())  continue;

    MachineInstr *DefMI = MRI->getVRegDef(MO.getReg());
    assert(DefMI && "Not in SSA form!");

    MachineBasicBlock *DefMBB = DefMI->getParent();
    if (DT->dominates(MBBToHoist, DefMBB)) {
      MBBToHoist = DefMBB;
      continue;
    }

    assert(DT->dominates(DefMBB, MBBToHoist)
           && "Operands not in a path of the dominator tree!");
  }

  if (MBBToHoist == CurMBB) return false;

  MI->removeFromParent();

  MachineBasicBlock::instr_iterator IP = MBBToHoist->getFirstInstrTerminator();

  // Insert the imp_def before the PHI incoming copies.
  while (llvm::prior(IP)->getOpcode() == VTM::VOpMvPhi)
    --IP;

  MBBToHoist->insert(IP, MI);
  return true;
}

bool hoistDatapathOpInMBB(MachineBasicBlock *MBB, MachineDominatorTree *DT,
                          MachineRegisterInfo *MRI) {
  bool MadeChange = false;

  typedef MachineBasicBlock::instr_iterator instr_it;
  for (instr_it I = MBB->instr_begin(), E = MBB->instr_end(); I != E; / *++I* /) {
    MachineInstr *MI = I++;

    if (VInstrInfo::isDatapath(MI->getOpcode()))
      MadeChange |= hoistDatapathOp(MI, DT, MRI);
  }

  return MadeChange;
}

bool hoistDatapathOpInSuccs(MachineBasicBlock *MBB, MachineDominatorTree *DT,
                            MachineRegisterInfo *MRI) {
  bool MadeChange = false;
  typedef MachineBasicBlock::succ_iterator succ_iterator;
  for (succ_iterator I = MBB->succ_begin(), E = MBB->succ_end(); I != E; ++I)
    MadeChange |= hoistDatapathOpInMBB(MBB, DT, MRI);

  return MadeChange;
}

struct HoistDatapathPass : public MachineFunctionPass {
  static char ID;
  HoistDatapathPass() : MachineFunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesCFG();
    AU.addRequired<MachineDominatorTree>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) {
    bool Changed = false;
    MachineDominatorTree &DT = getAnalysis<MachineDominatorTree>();
    MachineRegisterInfo &MRI = MF.getRegInfo();

    for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I)
      Changed |= hoistDatapathOpInMBB(I, &DT, &MRI);

    return Changed;
  }
};

Pass *createHoistDatapathPass() {
  return new llvm::HoistDatapathPass();
}
}

char llvm::HoistDatapathPass::ID = 0;*/
