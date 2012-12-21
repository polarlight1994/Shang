//====- DataPathPromotion.cpp - Perpare for RTL code generation -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the DataPathPromotion pass, promote the operation in 
// DataPath from ChainedOpc to ControlOpc according to the restraint.
//
//===----------------------------------------------------------------------===//

#include "vtm/VerilogBackendMCTargetDesc.h"
#include "vtm/Passes.h"
#include "vtm/VFInfo.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VInstrInfo.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;
namespace {
struct DataPathPromotion : public MachineFunctionPass {
  static char ID;

  DataPathPromotion() : MachineFunctionPass(ID) {}

  template<unsigned IDX>
  void insertRegForOperand(MachineInstr *MI, MachineRegisterInfo &MRI) {
    MachineOperand &MO = MI->getOperand(IDX);
    MachineOperand MOToBeCopied = MO;
    MOToBeCopied.clearParent();
    unsigned Reg = MRI.createVirtualRegister(&VTM::DRRegClass);
    MachineInstr *IP = MO.getParent();
    DebugLoc dl = IP->getDebugLoc();

    BuildMI(*IP->getParent(), IP, dl, VInstrInfo::getDesc(VTM::VOpMove))
      .addOperand(VInstrInfo::CreateReg(Reg, VInstrInfo::getBitWidth(MO), true))
      .addOperand(MOToBeCopied)
      // The predecate and trace information sould also be copied.
      .addOperand(*VInstrInfo::getPredOperand(MI))
      .addOperand(*VInstrInfo::getTraceOperand(MI));

    // Read the value from the copied register instead.
    MO.ChangeToRegister(Reg, false);
  }

  void promoteAdd(MachineInstr *MI, MachineRegisterInfo &MRI);

  template<VFUs::FUTypes T>
  void promoteBinOp(MachineInstr *MI, MachineRegisterInfo &MRI) {
    unsigned Size = VInstrInfo::getBitWidth(MI->getOperand(0));
    if (!getFUDesc(T)->shouldBeChained(Size)) {
      insertRegForOperand<1>(MI, MRI);
      insertRegForOperand<2>(MI, MRI);
    }
  }

  bool runOnMachineFunction(MachineFunction &MF) {
    MachineRegisterInfo &MRI = MF.getRegInfo();

    for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I)
      for (MachineBasicBlock::instr_iterator II = I->instr_begin(),
           IE = I->instr_end(); II != IE; ++II) {
        MachineInstr *MI = II;   
        switch(MI->getOpcode()) {
        case VTM::VOpAdd:
          promoteAdd(MI, MRI);
          break;
        case VTM::VOpSGE:
        case VTM::VOpSGT:
        case VTM::VOpUGE:
        case VTM::VOpUGT:
          promoteBinOp<VFUs::ICmp>(MI, MRI);
          break;
        case VTM::VOpMult:
        case VTM::VOpMultLoHi:
          promoteBinOp<VFUs::Mult>(MI, MRI);
          break;
        case VTM::VOpSHL:
        case VTM::VOpSRA:
        case VTM::VOpSRL:
          promoteBinOp<VFUs::Shift>(MI, MRI);
          break;
        default:
          break;
        }
      }    
    return true;
  }

  const char *getPassName() const {
    return "Data Path Promotion Pass";
  }
};
}

void DataPathPromotion::promoteAdd(MachineInstr *MI, MachineRegisterInfo &MRI) {
  unsigned Size = VInstrInfo::getBitWidth(MI->getOperand(0));
  Size = std::min<int>(Size - 1, 0);
  
  if (!getFUDesc(VFUs::AddSub)->shouldBeChained(Size)) {
    insertRegForOperand<1>(MI, MRI);
    insertRegForOperand<2>(MI, MRI);
    insertRegForOperand<3>(MI, MRI);
  }
}

char DataPathPromotion::ID = 0;

Pass *llvm::createDataPathPromotionPass() {
  return new DataPathPromotion();
}
