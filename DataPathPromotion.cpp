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
#include "llvm/CodeGen/Passes.h"

using namespace llvm;
namespace {
struct DataPathPromotion : public MachineFunctionPass {
  // Mapping the PHI number to accutally register.
  std::map<unsigned, unsigned> PHIsMap;
  static char ID;

  DataPathPromotion() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) {
    for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I)
      for (MachineBasicBlock::instr_iterator II = I->instr_begin(),
           IE = I->instr_end(); II != IE; ++II) {
        MachineInstr *MI = II;   
        switch(MI->getOpcode()) {
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

char DataPathPromotion::ID = 0;

Pass *llvm::createDataPathPromotionPass() {
  return new DataPathPromotion();
}
