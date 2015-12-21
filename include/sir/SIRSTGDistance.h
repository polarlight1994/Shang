//---- SIRSTGDistance.h - Abstract Interface for Timing Analysis ---*- C++ -*-//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the SIRSTGDistance pass which compute the shortest path
// distance between the states in the State-Transition-Graph.
//
//===----------------------------------------------------------------------===//

#ifndef SIR_STG_DISTANCE_H
#define SIR_STG_DISTANCE_H

#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

namespace llvm {
class SIRSTGDistance : public SIRPass {
public:
  static char ID;
  std::map<unsigned, std::map<unsigned, unsigned> > DistanceMatrix;

  SIRSTGDistance() : SIRPass(ID) {
    initializeSIRSTGDistancePass(*PassRegistry::getPassRegistry());
  }
  
  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequiredID(SIRSchedulingID);
    AU.addRequiredID(SIRRegisterSynthesisForCodeGenID);
    AU.setPreservesAll();
  }

  unsigned getDistance(unsigned SrcSlotNum, unsigned DstSlotNum);
  bool updateDistance(unsigned Distance, unsigned SrcSlotNum, unsigned DstSlotNum);

  unsigned getIntervalFromSrc(SIRSlot *DefSlot, SIRSlot *ReadSlot);
  unsigned getIntervalFromSrc(SIRRegister *Reg, SIRSlot *ReadSlot);
  unsigned getIntervalFromSrc(SIRRegister *Reg, ArrayRef<SIRSlot *> ReadSlots);

  bool runOnSIR(SIR &SM);
};
}

#endif