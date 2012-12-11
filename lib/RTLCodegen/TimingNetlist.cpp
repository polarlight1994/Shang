//=--- TimingNetlist.cpp - The Netlist for Delay Estimation -------*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface timing netlist.
//
//===----------------------------------------------------------------------===//

#include "TimingNetlist.h"

#include "llvm/ADT/SetOperations.h"
#define DEBUG_TYPE "timing-netlist"
#include "llvm/Support/Debug.h"

using namespace llvm;

VASTMachineOperand *TimingNetlist::getSrcPtr(unsigned Reg) const {
  // Lookup the value from input of the netlist.
  return dyn_cast_or_null<VASTMachineOperand>((*this)->lookupExpr(Reg).get());
}

VASTWire *TimingNetlist::getDstPtr(unsigned Reg) const {
  // Lookup the value from the output list of the netlist first.
  return lookupFanout(Reg);
}

void TimingNetlist::annotateDelay(VASTMachineOperand *Src, unsigned ToReg,
                                  delay_type delay) {

  PathInfo[ToReg][Src] = delay / VFUs::Period;
}


void TimingNetlist::createDelayEntry(unsigned DstReg, VASTMachineOperand *Src) {
  assert(DstReg && "Unexpected NO_REGISTER!");
  assert(Src && "Bad pointer to Src!");

  PathInfo[DstReg][Src] = 0;
}

void TimingNetlist::computeDelayFromSrc(unsigned DstReg, unsigned SrcReg) {
  // Forward the Src terminator of the path from SrcReg.
  PathInfoTy::iterator at = PathInfo.find(SrcReg);

  // If SrcReg is a terminator of a path, create a path from SrcReg to DstReg.
  if (at == PathInfo.end()) {
    // Ignore the SrcReg that can be eliminated by constant folding.
    if (VASTMachineOperand *MO = getSrcPtr(SrcReg))
      createDelayEntry(DstReg, MO);

    return;
  }

  // Otherwise forward the source nodes reachable to SrcReg to DstReg.
  set_union(PathInfo[DstReg], at->second);
}

void TimingNetlist::addInstrToDatapath(MachineInstr *MI) {
  unsigned DefReg = 0;

  bool IsDatapath = VInstrInfo::isDatapath(MI->getOpcode());

  // Can use add the MachineInstr to the datapath?
  if (IsDatapath)
    buildDatapathOnly(MI);

  // Ignore the PHIs.
  if (!IsDatapath && MI->isPHI()) return;

  // Otherwise export the values used by this MachineInstr.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i){
    MachineOperand &MO = MI->getOperand(i);

    if (!MO.isReg()) {
      // Remember the external source.
      if (IsDatapath && !MO.isImm())
        createDelayEntry(DefReg, cast<VASTMachineOperand>(getAsOperandImpl(MO)));

      continue;
    }

    unsigned Reg = MO.getReg();

    if (Reg == 0) continue;
    
    if (MO.isDef()) {
      assert(DefReg == 0 && "Unexpected multi-defines!");
      DefReg = Reg;
      continue;
    }

    // Remember the paths.
    if (IsDatapath) {
      computeDelayFromSrc(DefReg, Reg);
    } else
      // Try to export the value.
      exportValue(Reg);
  }
}
