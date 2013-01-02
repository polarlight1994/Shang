//===-- VSelectionDAGInfo.cpp - VTM SelectionDAG Info ---------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the VSelectionDAGInfo class.
//
//===----------------------------------------------------------------------===//

#include "VTargetMachine.h"
#include "llvm/LLVMContext.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-selectiondag-info"
#include "llvm/Support/Debug.h"

using namespace llvm;
cl::opt<bool> EnableMemSCM("vtm-enable-memscm",
                           cl::init(true), cl::Hidden);

VSelectionDAGInfo::VSelectionDAGInfo(const VTargetMachine &TM)
  : TargetSelectionDAGInfo(TM) {
}

VSelectionDAGInfo::~VSelectionDAGInfo() {
}

SDValue
VSelectionDAGInfo::EmitTargetCodeForMemset(SelectionDAG &DAG, DebugLoc dl,
                                           SDValue Chain,
                                           SDValue Op1, SDValue Op2,
                                           SDValue Op3, unsigned Align,
                                           bool isVolatile,
                                           MachinePointerInfo DstPtrInfo) const{
  llvm_unreachable("Function is NOT implemented yet!");
  return SDValue();
}

SDValue
VSelectionDAGInfo::EmitTargetCodeForMemcpy(SelectionDAG &DAG, DebugLoc dl,
                                           SDValue Chain,
                                           SDValue Op1, SDValue Op2,
                                           SDValue Op3, unsigned Align,
                                           bool isVolatile, bool AlwaysInline,
                                           MachinePointerInfo DstPtrInfo,
                                           MachinePointerInfo SrcPtrInfo) const{
  llvm_unreachable("Function is NOT implemented yet!");
  return SDValue();
}

SDValue
VSelectionDAGInfo::EmitTargetCodeForMemmove(SelectionDAG &DAG, DebugLoc dl,
                                            SDValue Chain,
                                            SDValue Op1, SDValue Op2,
                                            SDValue Op3, unsigned Align, bool isVolatile,
                                            MachinePointerInfo DstPtrInfo,
                                            MachinePointerInfo SrcPtrInfo)const{
  llvm_unreachable("Function is NOT implemented yet!");
  return SDValue();
}
