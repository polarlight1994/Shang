//==----------- BitlevelOpt.cpp - Bit-level Optimization ----------*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
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

//===--------------------------------------------------------------------===//
APInt BitMasks::getKnownBits() const {
  return KnownZeros | KnownOnes;
}

bool BitMasks::isSubSetOf(const BitMasks &RHS) const {
  assert(!(KnownOnes & RHS.KnownZeros)
        && !(KnownZeros & RHS.KnownOnes)
        && "Bit masks contradict!");

  APInt KnownBits = getKnownBits(), RHSKnownBits = RHS.getKnownBits();
  if (KnownBits == RHSKnownBits) return false;

  return (KnownBits | RHSKnownBits) == RHSKnownBits;
}

void BitMasks::dump() const {
  SmallString<128> Str;
  KnownZeros.toString(Str, 2, false, true);
  dbgs() << "Known Zeros\t" << Str << '\n';
  Str.clear();
  KnownOnes.toString(Str, 2, false, true);
  dbgs() << "Known Ones\t" << Str << '\n';
  Str.clear();
  getKnownBits().toString(Str, 2, false, true);
  dbgs() << "Known Bits\t" << Str << '\n';
}

//===----------------------------------------------------------------------===//
BitMasks BitMaskContext::calculateBitCatBitMask(VASTExpr *Expr) {
  unsigned CurUB = Expr->getBitWidth();
  unsigned ExprSize = Expr->getBitWidth();
  // Clear the mask.
  APInt KnownOnes = APInt::getNullValue(ExprSize),
    KnownZeros = APInt::getNullValue(ExprSize);

  // Concatenate the bit mask together.
  for (unsigned i = 0; i < Expr->size(); ++i) {
    VASTValPtr CurBitSlice = Expr->getOperand(i);
    unsigned CurSize = CurBitSlice->getBitWidth();
    unsigned CurLB = CurUB - CurSize;
    BitMasks CurMask = calculateBitMask(CurBitSlice);
    KnownZeros |= CurMask.KnownZeros.zextOrSelf(ExprSize).shl(CurLB);
    KnownOnes |= CurMask.KnownOnes.zextOrSelf(ExprSize).shl(CurLB);

    CurUB = CurLB;
  }

  return BitMasks(KnownZeros, KnownOnes);
}

BitMasks BitMaskContext::calculateConstantBitMask(VASTConstant *C) {
  return BitMasks(~C->getAPInt(), C->getAPInt());
}

BitMasks BitMaskContext::calculateAssignBitMask(VASTExpr *Expr) {
  unsigned UB = Expr->UB, LB = Expr->LB;
  BitMasks CurMask = calculateBitMask(Expr->getOperand(0));
  // Adjust the bitmask by LB.
  return BitMasks(VASTConstant::getBitSlice(CurMask.KnownZeros, UB, LB),
                  VASTConstant::getBitSlice(CurMask.KnownOnes, UB, LB));
}

BitMasks BitMaskContext::calculateAndBitMask(VASTExpr *Expr) {
  unsigned BitWidth = Expr->getBitWidth();
  // Assume all bits are 1s.
  BitMasks Mask(APInt::getNullValue(BitWidth),
                APInt::getAllOnesValue(BitWidth));

  for (unsigned i = 0; i < Expr->size(); ++i) {
    BitMasks OperandMask = calculateBitMask(Expr->getOperand(i));
    // The bit become zero if the same bit in any operand is zero.
    Mask.KnownZeros |= OperandMask.KnownZeros;
    // The bit is one only if the same bit in all operand are zeros.
    Mask.KnownOnes &= OperandMask.KnownOnes;
  }

  return Mask;
}

// The implementation of basic bit mark calucation.
BitMasks BitMaskContext::calculateBitMask(VASTValue *V) {
  BitMaskCacheTy::iterator I = BitMaskCache.find(V);
  // Return the cached version if possible.
  if (I != BitMaskCache.end()) {
    return I->second;
  }

  // Most simple case: Constant.
  if (VASTConstant *C = dyn_cast<VASTConstant>(V))
    return setBitMask(V, calculateConstantBitMask(C));

  VASTExpr *Expr = dyn_cast<VASTExpr>(V);
  if (!Expr)
    return BitMasks(V->getBitWidth());

  switch(Expr->getOpcode()) {
  default: break;
  case VASTExpr::dpBitCat:
    return setBitMask(V, calculateBitCatBitMask(Expr));
  case VASTExpr::dpAssign:
    return setBitMask(V, calculateAssignBitMask(Expr));
  case VASTExpr::dpAnd:
    return setBitMask(V, calculateAndBitMask(Expr));
  case VASTExpr::dpKeep:
    return setBitMask(V, calculateBitMask(Expr->getOperand(0)));
  }

  return BitMasks(Expr->getBitWidth());
}

BitMasks BitMaskContext::calculateBitMask(VASTValPtr V) {
  BitMasks Masks = calculateBitMask(V.get());

  // Flip the bitmask if the value is inverted.
  if (V.isInverted())
    return BitMasks(Masks.KnownOnes, Masks.KnownZeros);

  return Masks;
}

//===----------------------------------------------------------------------===//
DatapathBLO::DatapathBLO(DatapathContainer &Datapath)
  : MinimalExprBuilderContext(Datapath), Builder(*this) {}

DatapathBLO::~DatapathBLO() {}

void DatapathBLO::resetForNextIteration() {
  Visited.clear();
  Datapath.gc();
}

void DatapathBLO::deleteContenxt(VASTValue *V) {
  MinimalExprBuilderContext::deleteContenxt(V);
  BitMaskCache.erase(V);
}

bool DatapathBLO::replaceIfNotEqual(VASTValPtr From, VASTValPtr To) {
  if (From == To)
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

VASTValPtr DatapathBLO::propagateInvertFlag(VASTValPtr V) {
  // There is not invert flag to fold.
  if (!V.isInverted())
    return V;

  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);

  if (Expr == None)
    return eliminateConstantInvertFlag(V);

  VASTExpr::Opcode Opcode = Expr->getOpcode();
  switch (Opcode) {
    // Only propagate the invert flag across these expressions:
  case VASTExpr::dpAssign:
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

  return Builder.buildExpr(Opcode, InvertedOperands, Expr->UB, Expr->LB);
}

bool DatapathBLO::optimizeBitRepeat(VASTExpr *Expr) {
  VerifyOpcode<VASTExpr::dpBitRepeat>(Expr);

  VASTConstPtr C = cast<VASTConstPtr>(Expr->getOperand(1));
  unsigned Times = C.getZExtValue();

  VASTValPtr Pattern = Expr->getOperand(0);

  // This is not a repeat at all.
  if (Times == 1) {
    replaceIfNotEqual(Expr, Pattern);
    return true;
  }

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(Pattern)) {
    // Repeat the constant bit pattern.
    if (C->getBitWidth() == 1) {
      Pattern = C.getBoolValue() ?
                getConstant(APInt::getAllOnesValue(Times)) :
                getConstant(APInt::getNullValue(Times));
      replaceIfNotEqual(Expr, Pattern);
      return true;
    }
  }

  return false;
}

bool DatapathBLO::optimizeExpr(VASTExpr *Expr) {
  switch (Expr->getOpcode()) {
  case VASTExpr::dpBitRepeat:
    return optimizeBitRepeat(Expr);
  // Strange expressions that we cannot optimize.
  default: break;
  }
  return false;
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
  typedef VASTOperandList::op_iterator ChildIterator;
  std::vector<std::pair<VASTExpr*, ChildIterator> > VisitStack;

  VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *CurNode = VisitStack.back().first;
    ChildIterator &It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == CurNode->op_end()) {
      VisitStack.pop_back();
      Replaced |= optimizeExpr(Expr);
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTExpr *ChildExpr = It->getAsLValue<VASTExpr>();
    ++It;

    if (ChildExpr == NULL)
      continue;

    // No need to visit the same node twice
    if (!LocalVisited.insert(ChildExpr).second || Visited.count(ChildExpr))
      continue;

    VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
  }

  return Replaced;
}


namespace {
struct BitlevelOptPass : public VASTModulePass {
  static char ID;
  BitlevelOptPass() : VASTModulePass(ID) {
    //initializeBitlevelOptPassPass(*PassRegistry::getPassRegistry());
  }

  bool runSingleIteration(VASTModule &VM, DatapathBLO &BLO);

  bool runOnVASTModule(VASTModule &VM);


  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addPreservedID(PreSchedBindingID);
    AU.addPreservedID(ControlLogicSynthesisID);
  }
};
}

char BitlevelOptPass::ID = 0;

bool BitlevelOptPass::runSingleIteration(VASTModule &VM, DatapathBLO &BLO) {
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

bool BitlevelOptPass::runOnVASTModule(VASTModule &VM) {
  DatapathBLO BLO(VM);

  if (!runSingleIteration(VM, BLO))
    return false;

  while (runSingleIteration(VM, BLO))
    BLO.resetForNextIteration();

  return true;
}

Pass *llvm::createBitlevelOptPass() {
  return new BitlevelOptPass();
}
