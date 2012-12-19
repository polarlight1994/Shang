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

void TimingNetlist::annotateDelay(VASTMachineOperand *Src, VASTValue *Dst,
                                  delay_type delay) {
  PathInfoTy::iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  SrcInfoTy::iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  path_start_from->second = delay / VFUs::Period;
}

TimingNetlist::delay_type TimingNetlist::getDelay(VASTMachineOperand *Src,
                                                  VASTValue *Dst) const {                                                    
  PathInfoTy::const_iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  SrcInfoTy::const_iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  return path_start_from->second;
}

void TimingNetlist::createDelayEntry(VASTValue *Dst, VASTMachineOperand *Src) {
  assert(Src && Dst && "Bad pointer!");

  PathInfo[Dst][Src] = 0;
}

void TimingNetlist::createPathFromSrc(VASTValue *Dst, VASTValue *Src) {
  assert(Dst != Src && "Unexpected cycle!");

  // Forward the Src terminator of the path from SrcReg.
  PathInfoTy::iterator at = PathInfo.find(Src);

  // If SrcReg is a terminator of a path, create a path from SrcReg to DstReg.
  if (at == PathInfo.end()) {
    // Ignore the SrcReg that can be eliminated by constant folding.
    if (VASTMachineOperand *MO = dyn_cast<VASTMachineOperand>(Src))
      createDelayEntry(Dst, MO);

    return;
  }

  // Otherwise forward the source nodes reachable to SrcReg to DstReg.
  set_union(PathInfo[Dst], at->second);
}

void TimingNetlist::addInstrToDatapath(MachineInstr *MI) {
  VASTValue *DatapathNode = 0;

  bool IsDatapath = VInstrInfo::isDatapath(MI->getOpcode());

  // Can use add the MachineInstr to the datapath?
  if (IsDatapath)
    DatapathNode = buildDatapathOnly(MI).get();

  // Ignore the PHIs.
  if (!IsDatapath && MI->isPHI()) return;

  // Otherwise export the values used by this MachineInstr.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i){
    MachineOperand &MO = MI->getOperand(i);

    if (!MO.isReg()) {
      // Remember the external source.
      if (IsDatapath && !MO.isImm()) {
        VASTMachineOperand *VMO = cast<VASTMachineOperand>(getAsOperandImpl(MO));
        createDelayEntry(DatapathNode, VMO);
      }

      continue;
    }

    unsigned Reg = MO.getReg();

    if (MO.isDef() || Reg == 0) continue;

    // Remember the paths.
    if (IsDatapath) {
      VASTValue *SrcVal = Builder.lookupExpr(Reg).get();
      assert((SrcVal != DatapathNode || MI->getOpcode() == VTM::VOpNot)
             && "Unexpected cycle!");
      if (SrcVal != DatapathNode) createPathFromSrc(DatapathNode, SrcVal);
    } else
      // Try to export the value.
      pinValue(Reg);
  }
}
