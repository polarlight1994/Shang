//===---------- VRegisterInfo.cpp - VTM Register Information ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the VTM implementation of the TargetRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#include "vtm/Passes.h"
#include "vtm/VerilogBackendMCTargetDesc.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VInstrInfo.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Type.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"

namespace llvm {
  extern const MCRegisterDesc VTMRegDesc[];
  extern const uint16_t VTMRegLists[];
  extern const uint16_t VTMRegDiffLists[];
  extern const char VTMRegStrings[];
  extern const uint16_t VTMRegUnitRoots[][2];
  extern const uint16_t VTMRegEncodingTable[];
}

using namespace llvm;

VRegisterInfo::VRegisterInfo() : VTMGenRegisterInfo(0) {
  // Dirty hack: Allocate enough physical register for the backend.
  InitMCRegisterInfo(VTMRegDesc, MaxPhyRegs, 0,
                     VTMMCRegisterClasses, 14,
                     VTMRegUnitRoots, 14,
                     VTMRegLists, VTMRegDiffLists, VTMRegStrings, NULL, 0,
                     VTMRegEncodingTable);
}

const TargetRegisterClass *
VRegisterInfo::getPointerRegClass(const MachineFunction &, unsigned) const {
  return &VTM::DRRegClass;
}


const uint16_t*
VRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  static const uint16_t CalleeSavedRegs[] = {0};
  return  CalleeSavedRegs;
}

BitVector
VRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  return Reserved;
}

void VRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                        int SPAdj,
                                        RegScavenger *RS /*= NULL*/ ) const {
}

void VRegisterInfo::emitPrologue(MachineFunction &MF) const {}

void VRegisterInfo::emitEpilogue(MachineFunction &MF,
                                 MachineBasicBlock &MBB) const {}

unsigned
VRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  llvm_unreachable("No return address register in VTM");
  return 0;
}

unsigned VRegisterInfo::getEHExceptionRegister() const {
  llvm_unreachable("What is the exception register");
  return 0;
}

unsigned VRegisterInfo::getEHHandlerRegister() const {
  llvm_unreachable("What is the exception handler register");
  return 0;
}

int VRegisterInfo::getDwarfRegNum(unsigned RegNum, bool isEH) const {
  llvm_unreachable("What is the dwarf register number");
  return -1;
}

int VRegisterInfo::getDwarfRegNumFull(unsigned RegNum, unsigned Flavour) const {
  llvm_unreachable("Unknown DWARF flavour");
  return -1;
}

bool VRegisterInfo::needsStackRealignment(const MachineFunction &) const {
  return false;
}


#define GET_REGINFO_TARGET_DESC
#include "VerilogBackendGenRegisterInfo.inc"

const TargetRegisterClass *
VRegisterInfo::getPointerRegClass(unsigned Kind) const {
  return &VTM::DRRegClass;
}

bool VRegisterInfo::IsWire(unsigned RegNo, const MachineRegisterInfo *MRI) {
  if (!TargetRegisterInfo::isVirtualRegister(RegNo))
    return false;

  const TargetRegisterClass *RC = MRI->getRegClass(RegNo);
  return RC != &VTM::DRRegClass;
}

unsigned VRegisterInfo::allocatePhyReg(unsigned RegClassID, unsigned Width) {
  unsigned RegNum = PhyRegs.size() + 1;
  PhyRegs.push_back(PhyRegInfo(RegClassID, RegNum, Width, 0));
  return RegNum;
}

unsigned VRegisterInfo::getSubRegOf(unsigned Parent, unsigned UB, unsigned LB) {
  PhyRegInfo Info = getPhyRegInfo(Parent);
  unsigned AliasSetId = Info.getAliasSetId();
  unsigned SubRegNum = PhyRegs.size() + 1;
  PhyRegInfo SubRegInfo = PhyRegInfo(Info.getRegClass(), AliasSetId, UB, LB);
  PhyRegs.push_back(SubRegInfo);
  // TODO: Check if the sub register exist?
  PhyRegAliasInfo[AliasSetId].insert(SubRegNum);
  return SubRegNum;
}

unsigned VRegisterInfo::allocateFN(unsigned FNClassID, unsigned Width /* = 0 */) {
  return allocatePhyReg(FNClassID, Width);
}

VRegisterInfo::PhyRegInfo VRegisterInfo::getPhyRegInfo(unsigned RegNum) const {
  return PhyRegs[RegNum - 1];
}

void VRegisterInfo::resetPhyRegAllocation() {
  PhyRegs.clear();
  PhyRegAliasInfo.clear();
}

unsigned VRegisterInfo::getSubReg(unsigned RegNo, unsigned Index) const {
  unsigned SubReg = RegNo + Index;
  assert(getPhyRegInfo(SubReg).getParentRegister() == RegNo
         && "Bad subreg index!");
  return SubReg;
}


const TargetRegisterClass *VRegisterInfo::getRepRegisterClass(unsigned Opcode) {
  if (VInstrInfo::isDatapath(Opcode)) return &VTM::WireRegClass;

  switch (Opcode) {
  default:                  return &VTM::DRRegClass;
  case VTM::VOpMemTrans:    return &VTM::RINFRegClass;
  case VTM::VOpInternalCall:return &VTM::RCFNRegClass;
  case VTM::VOpBRAMRead:
  case VTM::VOpBRAMWrite:   return &VTM::RBRMRegClass;
  case VTM::VOpDstMux:      return &VTM::RMUXRegClass;
  }

  return 0;
}
