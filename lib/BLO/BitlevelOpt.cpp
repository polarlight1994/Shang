//==----------- BitlevelOpt.cpp - Bit-level Optimization ----------*- C++ -*-=//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the BitLevelOpt pass.
// The BitLevelOpt pass perform the bit-level optimizations iteratively until
// the bit-level optimization do not optimize the Module any further.
//
//===----------------------------------------------------------------------===//

#include "BitlevelOpt.h"

#include "vast/Passes.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-bit-level-opt"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumIterations, "Number of bit-level optimization iteration");
STATISTIC(NodesReplaced,
          "Number of Nodes are replaced during the bit-level optimization");
STATISTIC(NodesReplacedByKnownBits,
          "Number of Nodes whose bits are all known "
          "during the bit-level optimization");

//===----------------------------------------------------------------------===//
DatapathBLO::DatapathBLO(DatapathContainer &Datapath)
  : MinimalExprBuilderContext(Datapath), Builder(*this) {}

DatapathBLO::~DatapathBLO() {}

void DatapathBLO::resetForNextIteration() {
  Visited.clear();
  Datapath.gc();
}

bool DatapathBLO::replaceIfNotEqual(VASTValPtr From, VASTValPtr To) {
  if (To == None || From == To)
    return false;

  replaceAllUseWith(From, To);
  ++NodesReplaced;

  // Now To is a optimized node, we will not optimize it again in the current
  // iteration.
  if (VASTExpr *Expr = dyn_cast<VASTExpr>(To.get()))
    Visited.insert(Expr);

  return true;
}


VASTValPtr DatapathBLO::eliminateConstantInvertFlag(VASTValPtr V) {
  if (V.isInverted())
    if (VASTConstPtr C = dyn_cast<VASTConstPtr>(V))
        return getConstant(C.getAPInt());

  return V;
}

VASTValPtr DatapathBLO::eliminateInvertFlag(VASTValPtr V) {
  // There is not invert flag to fold.
  if (!V.isInverted())
    return V;

  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);

  if (Expr == None)
    return eliminateConstantInvertFlag(V);

  VASTExpr::Opcode Opcode = Expr->getOpcode();
  switch (Opcode) {
    // Only propagate the invert flag across these expressions:
  case VASTExpr::dpBitExtract:
  case VASTExpr::dpBitCat:
  case VASTExpr::dpBitRepeat:
  case VASTExpr::dpKeep:
    break;
    // Else stop propagating the invert flag here. In fact, the invert
    // flag cost nothing in LUT-based FPGA. What we worry about is the
    // invert flag may confuse the bit-level optimization.
  default:
    return V;
  }

  typedef VASTOperandList::op_iterator op_iterator;
  SmallVector<VASTValPtr, 8> InvertedOperands;
  // Collect the possible retimed operands.
  for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
    VASTValPtr Op = *I;
    VASTValPtr InvertedOp = eliminateConstantInvertFlag(Op.invert());
    InvertedOperands.push_back(InvertedOp);
  }

  return Builder.copyExpr(Expr.get(), InvertedOperands);
}

VASTValPtr DatapathBLO::optimizeBitRepeat(VASTValPtr Pattern, unsigned Times) {
  Pattern = eliminateInvertFlag(Pattern);

  // This is not a repeat at all.
  if (Times == 1)
    return Pattern;

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(Pattern)) {
    // Repeat the constant bit pattern.
    if (C->getBitWidth() == 1) {
      return C.getBoolValue() ?
             getConstant(APInt::getAllOnesValue(Times)) :
             getConstant(APInt::getNullValue(Times));
    }
  }

  return Builder.buildBitRepeat(Pattern, Times);
}

VASTValPtr
DatapathBLO::optimizeBitExtract(VASTValPtr V, unsigned UB, unsigned LB) {
  V = eliminateInvertFlag(V);
  unsigned OperandSize = V->getBitWidth();
  // Not a sub bitslice.
  if (UB == OperandSize && LB == 0)
    return V;

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(V))
    return getConstant(C.getBitSlice(UB, LB));

  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);

  if (Expr == None)
    return Builder.buildBitExtractExpr(V, UB, LB);

  if (Expr->getOpcode() == VASTExpr::dpBitExtract){
    unsigned Offset = Expr->getLB();
    UB += Offset;
    LB += Offset;
    return optimizeBitExtract(Expr.getOperand(0), UB, LB);
  }

  if (Expr->getOpcode() == VASTExpr::dpBitCat) {
    // Collect the bitslices which fall into (UB, LB]
    SmallVector<VASTValPtr, 8> Ops;
    unsigned CurUB = Expr->getBitWidth(), CurLB = 0;
    unsigned LeadingBitsToLeft = 0, TailingBitsToTrim = 0;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValPtr CurBitSlice = Expr.getOperand(i);
      CurLB = CurUB - CurBitSlice->getBitWidth();
      // Not fall into (UB, LB] yet.
      if (CurLB >= UB) {
        CurUB = CurLB;
        continue;
      }
      // The entire range is visited.
      if (CurUB <= LB)
        break;
      // Now we have CurLB < UB and CurUB > LB.
      // Compute LeadingBitsToLeft if UB fall into [CurUB, CurLB), which imply
      // CurUB >= UB >= CurLB.
      if (CurUB >= UB)
        LeadingBitsToLeft = UB - CurLB;
      // Compute TailingBitsToTrim if LB fall into (CurUB, CurLB], which imply
      // CurUB >= LB >= CurLB.
      if (LB >= CurLB)
        TailingBitsToTrim = LB - CurLB;

      Ops.push_back(CurBitSlice);
      CurUB = CurLB;
    }

    // Trivial case: Only 1 bitslice in range.
    if (Ops.size() == 1)
      return optimizeBitExtract(Ops.back(), LeadingBitsToLeft, TailingBitsToTrim);

    Ops.front() = optimizeBitExtract(Ops.front(), LeadingBitsToLeft, 0);
    Ops.back() = optimizeBitExtract(Ops.back(), Ops.back()->getBitWidth(),
                                    TailingBitsToTrim);

    return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, UB - LB);
  }

  if (Expr->getOpcode() == VASTExpr::dpBitRepeat) {
    VASTValPtr Pattern = Expr.getOperand(0);
    // Simply repeat the pattern by the correct number.
    if (Pattern->getBitWidth() == 1)
      return optimizeBitRepeat(Pattern, UB - LB);
    // TODO: Build the correct pattern.
  }

  return Builder.buildBitExtractExpr(V, UB, LB);
}

static VASTExprPtr GetAsBitExtractExpr(VASTValPtr V) {
  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);
  if (Expr == None || !Expr->isSubWord())
    return None;

  return Expr;
}

VASTValPtr DatapathBLO::optimizeBitCatImpl(MutableArrayRef<VASTValPtr> Ops,
                                           unsigned BitWidth) {
  VASTConstPtr LastC = dyn_cast<VASTConstPtr>(Ops[0]);
  VASTExprPtr LastBitSlice = GetAsBitExtractExpr(Ops[0]);

  unsigned ActualOpPos = 1;

  // Merge the constant sequence.
  for (unsigned i = 1, e = Ops.size(); i < e; ++i) {
    VASTValPtr V = Ops[i];
    if (VASTConstPtr CurC = dyn_cast<VASTConstPtr>(V)) {
      if (LastC != None) {
        // Merge the constants.
        APInt HiVal = LastC.getAPInt(), LoVal = CurC.getAPInt();
        unsigned HiSizeInBits = LastC->getBitWidth(),
                 LoSizeInBits = CurC->getBitWidth();
        unsigned SizeInBits = LoSizeInBits + HiSizeInBits;
        APInt Val = LoVal.zextOrSelf(SizeInBits);
        Val |= HiVal.zextOrSelf(SizeInBits).shl(LoSizeInBits);
        Ops[ActualOpPos - 1] = (LastC = getConstant(Val)); // Modify back.
        continue;
      } else {
        LastC = CurC;
        Ops[ActualOpPos++] = V; //push_back.
        continue;
      }
    } else // Reset LastImm, since the current value is not immediate.
      LastC = None;

    if (VASTExprPtr CurBitSlice = GetAsBitExtractExpr(V)) {
      VASTValPtr CurBitSliceParent = CurBitSlice.getOperand(0);
      if (LastBitSlice && CurBitSliceParent == LastBitSlice.getOperand(0)
          && LastBitSlice->getLB() == CurBitSlice->getUB()) {
        VASTValPtr MergedBitSlice
          = optimizeBitExtract(CurBitSliceParent, LastBitSlice->getUB(),
                               CurBitSlice->getLB());
        Ops[ActualOpPos - 1] = MergedBitSlice; // Modify back.
        LastBitSlice = GetAsBitExtractExpr(MergedBitSlice);
        continue;
      } else {
        LastBitSlice = CurBitSlice;
        Ops[ActualOpPos++] = V; //push_back.
        continue;
      }
    } else
      LastBitSlice = 0;

    Ops[ActualOpPos++] = V; //push_back.
  }

  Ops = Ops.slice(0, ActualOpPos);
  if (Ops.size() == 1)
    return Ops.back();

#ifndef NDEBUG
  unsigned TotalBits = 0;
  for (unsigned i = 0, e = Ops.size(); i < e; ++i)
    TotalBits += Ops[i]->getBitWidth();
  if (TotalBits != BitWidth) {
    dbgs() << "Bad bitcat operands: \n";
    for (unsigned i = 0, e = Ops.size(); i < e; ++i)
      Ops[i]->dump();
    llvm_unreachable("Bitwidth not match!");
  }
#endif

  return Builder.buildBitCatExpr(Ops, BitWidth);
}

VASTValPtr DatapathBLO::optimizeAndImpl(MutableArrayRef<VASTValPtr> Ops,
                                        unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
   return Ops[0];

  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);

  VASTValPtr LastVal = None;
  unsigned ActualPos = 0;
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    VASTValPtr CurVal = Ops[i];
    if (CurVal == LastVal) {
      // A & A = A
      continue;
    } else if (CurVal.invert() == LastVal)
      // A & ~A => 0
      return getConstant(APInt::getNullValue(BitWidth));

    // Ignore the 1s
    if (VASTConstPtr C = dyn_cast<VASTConstPtr>(CurVal)) {
      if (C.isAllOnes()) {
        DEBUG(dbgs().indent(2) << "Discard the all ones value: " << C << '\n');
        continue;
      }
    }

    Ops[ActualPos++] = CurVal;
    LastVal = CurVal;
  }

  // If there is only 1 operand left, simply return the operand.
  if (ActualPos == 1)
   return LastVal;

  // Resize the operand vector so it only contains valid operands.
  Ops = Ops.slice(0, ActualPos);

  return Builder.buildAndExpr(Ops, BitWidth);
}

VASTValPtr DatapathBLO::optimizeReduction(VASTExpr::Opcode Opc, VASTValPtr Op) {
  Op = eliminateInvertFlag(Op);

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(Op)) {
    APInt Val = C.getAPInt();
    switch (Opc) {
    case VASTExpr::dpRAnd:
      // Only reduce to 1 if all bits are 1.
      if (Val.isAllOnesValue())
        return getConstant(true, 1);
      else
        return getConstant(false, 1);
    case VASTExpr::dpRXor:
      // Only reduce to 1 if there are odd 1s.
      if (Val.countPopulation() & 0x1)
        return getConstant(true, 1);
      else
        return getConstant(false, 1);
      break; // FIXME: Who knows how to evaluate this?
    default:  llvm_unreachable("Unexpected Reduction Node!");
    }
  }

  // Promote the reduction to the operands.
  if (VASTExpr *Expr = dyn_cast<VASTExpr>(Op)) {
    switch (Expr->getOpcode()) {
    default: break;
    case VASTExpr::dpBitCat: {
      SmallVector<VASTValPtr, 8> Ops;
      typedef VASTExpr::op_iterator it;
      for (it I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
        Ops.push_back(optimizeReduction(Opc, *I));

      switch (Opc) {
      case VASTExpr::dpRAnd: return Builder.buildAndExpr(Ops, 1);
      case VASTExpr::dpRXor: return Builder.buildXorExpr(Ops, 1);
      default:  llvm_unreachable("Unexpected Reduction Node!");
      }
    }
    }
  }

  return Builder.buildReduction(Opc, Op);
}

VASTValPtr DatapathBLO::optimizeKeep(VASTValPtr Op) {
  return Builder.buildKeep(eliminateInvertFlag(Op));
}

VASTValPtr DatapathBLO::optimizeAddImpl(MutableArrayRef<VASTValPtr>  Ops,
                                        unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
    return Ops[0];

  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);

  APInt C = APInt::getNullValue(BitWidth);

  VASTValPtr LastVal = None;
  unsigned ActualPos = 0;
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    VASTValPtr CurVal = Ops[i];
    if (CurVal == LastVal) {
      // A + A = A << 1
      VASTConstant *ShiftAmt = getConstant(1, Log2_32_Ceil(BitWidth));
      CurVal = optimizeShift(VASTExpr::dpShl, CurVal, ShiftAmt, BitWidth);
    } else if (CurVal.invert() == LastVal)
      // A + ~A => ~0, verified by
      // (declare-fun a () (_ BitVec 32))
      // (assert (not (= (bvadd a (bvnot a)) #xffffffff)))
      // (check-sat)
      // -> unsat
      CurVal = getConstant(APInt::getAllOnesValue(BitWidth));

    // Accumulate the constants.
    if (VASTConstPtr CurC = dyn_cast<VASTConstPtr>(CurVal)) {
      C += CurC.getAPInt().zextOrTrunc(BitWidth);
      continue;
    }

    Ops[ActualPos++] = CurVal;
    LastVal = CurVal;
  }

  // Add the accumulated constant to operand list.
  if (C.getBoolValue())
    Ops[ActualPos++] = getConstant(C);

  Ops = Ops.slice(0, ActualPos);

  // If there is only 1 operand left, simply return the operand.
  if (ActualPos == 1)
    return Ops[0];

  return Builder.buildAddExpr(Ops, BitWidth);
}

VASTValPtr DatapathBLO::optimizeShift(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                                      unsigned BitWidth) {
  LHS = eliminateInvertFlag(LHS);
  RHS = eliminateInvertFlag(RHS);

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(RHS)) {
    unsigned ShiftAmount = C.getZExtValue();

    // If we not shift at all, simply return the operand.
    if (ShiftAmount == 0)
     return LHS;

    switch(Opc) {
    case VASTExpr::dpShl:{
      VASTValPtr PaddingBits = getConstant(0, ShiftAmount);
      LHS = optimizeBitExtract(LHS, LHS->getBitWidth() - ShiftAmount, 0);
      VASTValPtr Ops[] = { LHS, PaddingBits };
      return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, BitWidth);
    }
    case VASTExpr::dpSRL:{
      VASTValPtr PaddingBits = getConstant(0, ShiftAmount);
      LHS = optimizeBitExtract(LHS, LHS->getBitWidth(), ShiftAmount);
      VASTValPtr Ops[] = { PaddingBits, LHS };
      return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, BitWidth);
    }
    case VASTExpr::dpSRA:{
      VASTValPtr SignBits = optimizeBitRepeat(optimizeSignBit(LHS), ShiftAmount);
      LHS = optimizeBitExtract(LHS, LHS->getBitWidth(), ShiftAmount);
      VASTValPtr Ops[] = { SignBits, LHS };
      return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, BitWidth);
    }
    default: llvm_unreachable("Unexpected opcode!"); break;
    }
  }

  return Builder.buildShiftExpr(Opc, LHS, RHS, BitWidth);
}

void DatapathBLO::eliminateInvertFlag(MutableArrayRef<VASTValPtr> Ops) {
  for (unsigned i = 0; i < Ops.size(); ++i)
    Ops[i] = eliminateInvertFlag(Ops[i]);
}

VASTValPtr DatapathBLO::optimizeExpr(VASTExpr *Expr) {
  // Replace the expr by known bits if possible.
  VASTValPtr KnownBits = replaceKnownBits(Expr);
  if (KnownBits != Expr)
    return KnownBits;

  VASTExpr::Opcode Opcode = Expr->getOpcode();
  switch (Opcode) {
  case VASTExpr::dpAnd:
    return optimizeNAryExpr<VASTExpr::dpAnd>(Expr->getOperands(),
                                             Expr->getBitWidth());
  case VASTExpr::dpBitExtract: {
    VASTValPtr Op = Expr->getOperand(0);
    return optimizeBitExtract(Op, Expr->getUB(), Expr->getLB());
  }
  case VASTExpr::dpBitCat:
    return optimizeNAryExpr<VASTExpr::dpBitCat>(Expr->getOperands(),
                                                Expr->getBitWidth());
  case VASTExpr::dpBitRepeat: {
    unsigned Times = Expr->getRepeatTimes();

    VASTValPtr Pattern = Expr->getOperand(0);
    return optimizeBitRepeat(Pattern, Times);
  }
  case VASTExpr::dpAdd:
    return optimizeNAryExpr<VASTExpr::dpAdd>(Expr->getOperands(),
                                             Expr->getBitWidth());
  case VASTExpr::dpMul:
    return optimizeNAryExpr<VASTExpr::dpMul>(Expr->getOperands(),
                                             Expr->getBitWidth());
  case VASTExpr::dpRAnd:
  case VASTExpr::dpRXor:
    return optimizeReduction(Opcode, Expr->getOperand(0));
  case VASTExpr::dpKeep:
    return optimizeKeep(Expr->getOperand(0));
  case VASTExpr::dpShl:
  case VASTExpr::dpSRL:
  case VASTExpr::dpSRA:
    return optimizeShift(Opcode, Expr->getOperand(0), Expr->getOperand(1),
                         Expr->getBitWidth());
  // Yet to be implement:
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT:
  case VASTExpr::dpLUT:
    break;
  // Strange expressions that we cannot optimize.
  default: break;
  }

  return Expr;
}

VASTValPtr DatapathBLO::replaceKnownBits(VASTValPtr V) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V.get());
  if (Expr == NULL)
    return V;

  // Update the bitmask before we perform the optimization.
  Expr->evaluateMask();

  if (LLVM_LIKELY(!Expr->isAllBitKnown()))
    return V;

  // If all bit is known, simply return the constant to replace the expr.
  ++NodesReplacedByKnownBits;
  VASTValPtr C = getConstant(Expr->getKnownValue());
  // Do not forget the invert flag of the original expression.
  return eliminateConstantInvertFlag(C.invert(V.isInverted()));
}

bool DatapathBLO::optimizeAndReplace(VASTValPtr V) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V.get());

  if (Expr == NULL)
    return replaceIfNotEqual(V, eliminateConstantInvertFlag(V));

  // This expression had been optimized in the current iteration.
  if (Visited.count(Expr))
    return false;

  bool Replaced = false;

  std::set<VASTExpr*> LocalVisited;
  std::vector<std::pair<VASTHandle, unsigned> > VisitStack;

  VisitStack.push_back(std::make_pair(Expr, 0));

  while (!VisitStack.empty()) {
    VASTExpr *CurNode = VisitStack.back().first.getAsLValue<VASTExpr>();
    unsigned &Idx = VisitStack.back().second;

    // We have visited all children of current node.
    if (Idx == CurNode->size()) {
      VisitStack.pop_back();
      VASTValPtr NewVal = replaceKnownBits(optimizeExpr(CurNode));
      Replaced |= replaceIfNotEqual(CurNode, NewVal);
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTExpr *ChildExpr = CurNode->getOperand(Idx).getAsLValue<VASTExpr>();
    ++Idx;

    if (ChildExpr == NULL)
      continue;

    // No need to visit the same node twice
    if (!LocalVisited.insert(ChildExpr).second || Visited.count(ChildExpr))
      continue;

    VisitStack.push_back(std::make_pair(ChildExpr, 0));
  }

  return Replaced;
}


namespace {
struct BitlevelOpt : public VASTModulePass {
  static char ID;
  BitlevelOpt() : VASTModulePass(ID) {
    initializeBitlevelOptPass(*PassRegistry::getPassRegistry());
  }

  bool runSingleIteration(VASTModule &VM, DatapathBLO &BLO);

  bool runOnVASTModule(VASTModule &VM);


  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addPreservedID(PreSchedBindingID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addPreservedID(TimingDrivenSelectorSynthesisID);
    AU.addPreservedID(STGDistancesID);
  }
};
}

char BitlevelOpt::ID = 0;
char &vast::BitlevelOptID = BitlevelOpt::ID;
INITIALIZE_PASS(BitlevelOpt, "vast-bit-level-opt",
                "Perform the bit-level optimization.",
                false, true)

bool BitlevelOpt::runSingleIteration(VASTModule &VM, DatapathBLO &BLO) {
  bool Changed = false;

  typedef VASTModule::selector_iterator selector_iterator;

  for (selector_iterator I = VM.selector_begin(), E = VM.selector_end();
       I != E; ++I) {
    VASTSelector *Sel = I;

    if (Sel->isSelectorSynthesized()) {
      // Only optimize the guard and fanin
      BLO.optimizeAndReplace(Sel->getGuard());
      BLO.optimizeAndReplace(Sel->getFanin());
      continue;
    }

    typedef VASTSelector::const_iterator const_iterator;
    for (const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
      const VASTLatch &L = *I;
      BLO.optimizeAndReplace(L);
      BLO.optimizeAndReplace(L.getGuard());
    }
  }

  ++NumIterations;
  return Changed;
}

bool BitlevelOpt::runOnVASTModule(VASTModule &VM) {
  DatapathBLO BLO(VM);

  if (!runSingleIteration(VM, BLO))
    return false;

  while (runSingleIteration(VM, BLO))
    BLO.resetForNextIteration();

  return true;
}

Pass *vast::createBitlevelOptPass() {
  return new BitlevelOpt();
}
