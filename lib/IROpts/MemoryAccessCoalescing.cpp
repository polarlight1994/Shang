//===-- MemoryAccessCoalescing.cpp - Coalesce Memory Accesses  --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the MemoryAccessCoalescing Pass, which fully
// utilize the bandwidth of memory bus to boost the speed performance of the
// design.
//
//===----------------------------------------------------------------------===//
#include "shang/Passes.h"
#include "shang/FUInfo.h"

#include "llvm/Pass.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-memory-access-coalecing"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(MemOpFused, "Number of memory operations fused");

namespace {
struct MemoryAccessCoalescing : public FunctionPass {
  static char ID;
  ScalarEvolution *SE;
  AliasAnalysis *AA;
  DataLayout *TD;

  MemoryAccessCoalescing() : FunctionPass(ID), SE(0), AA(0), TD(0) {
    initializeMemoryAccessCoalescingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<ScalarEvolution>();
    AU.addRequired<AliasAnalysis>();
    AU.addRequired<DataLayout>();
    AU.setPreservesCFG();
  }

  bool runOnFunction(Function &F) {
    bool changed = false;
    SE = &getAnalysis<ScalarEvolution>();
    AA = &getAnalysis<AliasAnalysis>();
    TD = &getAnalysis<DataLayout>();

    for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
      // We may scan the same BB more than once to fuse all compatible pairs.
      while (runOnBasicBlock(*I))
        changed = true;

    return changed;
  }

  bool runOnBasicBlock(BasicBlock &BB);
  Instruction *findFusingCandidate(BasicBlock::iterator I, BasicBlock &BB);

  typedef SmallPtrSet<Instruction*, 8> InstSetTy;
  bool canBeFused(Instruction *LHS, Instruction *RHS);
  bool hasMemDependency(Instruction *DstInst, Instruction *SrcInst);
  bool trackUsesOfSrc(InstSetTy &UseSet, Instruction *Src, Instruction *Dst);

  static bool isLoadStore(Instruction *I) {
    return I->getOpcode() == Instruction::Load
           || I->getOpcode() == Instruction::Store;
  }

  static Value *getPointerOperand(Instruction *I) {
    if (LoadInst *L = dyn_cast<LoadInst>(I))
      return L->getPointerOperand();

    if (StoreInst *S = dyn_cast<StoreInst>(I))
      return S->getPointerOperand();

    return 0;
  }

  static bool isWritingTheSameValue(Instruction *LHS, Instruction *RHS) {
    if (LHS->getOpcode() != RHS->getOpcode()) return false;

    if (StoreInst *LHSS = dyn_cast<StoreInst>(LHS)) {
      StoreInst *RHSS = cast<StoreInst>(RHS);
      return LHSS->getValueOperand() == RHSS->getValueOperand();
    }
    
    // Not store instruction, it must be load instruction, and "write" the same
    // Value.
    assert(isa<LoadInst>(LHS) && "Bad instruction type!");
    return true;
  }

  static unsigned getAccessAlignment(Instruction *I) {
    if (LoadInst *L = dyn_cast<LoadInst>(I))
      return L->getAlignment();

    if (StoreInst *S = dyn_cast<StoreInst>(I))
      return S->getAlignment();

    llvm_unreachable("Unexpected instruction type!");
    return 0;
  }

  static Type *getAccessType(Instruction *I) {
    Value *Ptr = getPointerOperand(I);
    assert(Ptr && "I is not memory access operand!");
    PointerType *PT = cast<PointerType>(Ptr->getType());
    return PT->getElementType();
  }

  unsigned getAccessSizeInBytes(Instruction *I) {
    return TD->getTypeStoreSize(getAccessType(I));
  }

  static Value *getStoredValue(Instruction *I) {
    if (StoreInst *S = dyn_cast<StoreInst>(I))
      return S->getValueOperand();

    return 0;
  }

  static Value *getLoadedValue(Instruction *I) {
    if (LoadInst *L = dyn_cast<LoadInst>(I))
      return L;

    return 0;
  }

  const SCEVConstant *getAddressDistant(Instruction *LHS, Instruction *RHS) {
    Value *LHSAddr = getPointerOperand(LHS), *RHSAddr = getPointerOperand(RHS);
    const SCEV *LHSSCEV = SE->getSCEV(LHSAddr), *RHSSCEV = SE->getSCEV(RHSAddr);

    return dyn_cast<SCEVConstant>(SE->getMinusSCEV(RHSSCEV, LHSSCEV));
  }

  int64_t getAddressDistantInt(Instruction *LHS, Instruction *RHS) {
    const SCEVConstant *DeltaSCEV = getAddressDistant(LHS, RHS);
    assert(DeltaSCEV && "Cannot calculate distance!");

    return DeltaSCEV->getValue()->getSExtValue();
  }

  // Return true if LHS and RHS have identical operands.
  bool isIdenticalLoadStore(Instruction *LHS, Instruction *RHS) {
    if (!isLoadStore(LHS) || LHS->getOpcode() != RHS->getOpcode()) return false;

    for (unsigned i = 0, e = LHS->getNumOperands(); i != e; ++i)
      if (LHS->getOperand(i) != RHS->getOperand(i))
        return false;

    return true;
  }

  void moveUsesAfter(Instruction *MergeFrom, Instruction *MergeTo,
                     InstSetTy &UseSet);
  void fuseInstruction(Instruction *From, Instruction *To);
};
}

char MemoryAccessCoalescing::ID = 0;

INITIALIZE_PASS_BEGIN(MemoryAccessCoalescing,
                      "shang-mem-coalesce", "Memory Coalescing", false, false)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
  INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(MemoryAccessCoalescing,
                    "shang-mem-coalesce", "Memory Coalescing", false, false)

 bool MemoryAccessCoalescing::runOnBasicBlock(BasicBlock &BB) {
  bool Changed = false;

  // 1. Collect the fusing candidates.
  for (BasicBlock::iterator I = BB.begin(), E = BB.end(); I != E; /*++I*/) {
    Instruction *Inst = I;
    // Only fuse the accesses via memory bus
    if (!isLoadStore(Inst))  {
      ++I;
      continue;
    }

    if (Instruction *FusingCandidate = findFusingCandidate(Inst, BB)) {
      // Fuse Inst into FusingCandidate.
      fuseInstruction(Inst, FusingCandidate);
      assert(Inst->use_empty() && FusingCandidate->use_empty()
             && "Replaced value is still used!");
      ++I;
      Inst->eraseFromParent();
      // I maybe pointing to FusingCandidate, increase I again to skip
      // FusingCandidate if it is the case.
      if (I == BasicBlock::iterator(FusingCandidate)) ++I;
      FusingCandidate->eraseFromParent();
      Changed = true;
      continue;
    }

    // Else simply move to next instruction.
    ++I;
  }

  // Eliminated the dead instructions if necessary.
  if (Changed) {
    SimplifyInstructionsInBlock(&BB, TD);
    BB.dump();
  }
  
  return Changed;
}

Instruction *MemoryAccessCoalescing::findFusingCandidate(BasicBlock::iterator It,
                                                         BasicBlock &BB) {
  Instruction *Inst = It;
  InstSetTy UseSet;

  for (BasicBlock::iterator I = llvm::next(It), E = BB.end(); I != E; ++I) {
    Instruction *LastInst = I;
    // If there is a dependence between Inst and LastInst, then we cannot fuse
    // them together.
    if (trackUsesOfSrc(UseSet, Inst, LastInst)) continue;

    // Only fuse VOpMemTranses together.
    if (!isLoadStore(LastInst)) continue;    

    // Return the fusing candidate.
    if (canBeFused(Inst, LastInst)) {
      // Move all instructions that using Inst after LastInst. So that we can
      // safely replace Inst by LastInst.
      moveUsesAfter(Inst, LastInst, UseSet);
      return LastInst;
    }
  }

  return 0;
}

bool MemoryAccessCoalescing::hasMemDependency(Instruction *DstInst,
                                              Instruction *SrcInst) {
  unsigned DstOpcode = DstInst->getOpcode();
  bool ForceDstDep = (DstOpcode == Instruction::Call);
  bool IsDstMemTrans = isLoadStore(DstInst);

  if (!IsDstMemTrans && !ForceDstDep) return false;

  bool DstMayWrite = DstInst->mayWriteToMemory();

  // Do not add loop to dependent graph.
  if (SrcInst == DstInst) return false;

  Value *DstPtr = getPointerOperand(DstInst);

  // Check if source will access memory.
  unsigned SrcOpcode = SrcInst->getOpcode();
  if (!isLoadStore(SrcInst) && SrcOpcode != Instruction::Call)
    return false;

  // Handle force dependency.
  if (ForceDstDep || SrcOpcode == Instruction::Call)
    return true;

  bool SrcMayWrite = SrcInst->mayWriteToMemory();

  // Ignore RAR dependency.
  if (!SrcMayWrite && ! DstMayWrite) return false;

  Value *SrcPtr = getPointerOperand(SrcInst);

  // Is DstInst depends on SrcInst?
  return !(AA->alias(SrcPtr, DstPtr) != AliasAnalysis::NoAlias);
}

bool MemoryAccessCoalescing::trackUsesOfSrc(InstSetTy &UseSet, Instruction *Src,
                                            Instruction *Dst) {
  assert(!isa<PHINode>(Src) && !isa<PHINode>(Dst) && "Unexpected PHI!");

  if (UseSet.count(Dst)) return true;

  // Check the memory dependence, note that we can move the identical accesses
  // across each other.
  if (hasMemDependency(Dst, Src) && !isIdenticalLoadStore(Dst, Src)) {
    // There is a memory dependency between Src and Dst.
    UseSet.insert(Dst);
    return true;
  }

  typedef InstSetTy::iterator iterator;
  for (iterator I = UseSet.begin(), E = UseSet.end(); I != E; ++I) {
    Instruction *SrcUser = *I;
    if (hasMemDependency(Dst, SrcUser)) {
      // There is a memory dependency between SrcUser and Dst, so we cannot
      // move SrcUser after Dst, and we need to model this as a use
      // relationship.
      UseSet.insert(Dst);
      return true;
    }      
  }

  // Iterate from use to define.
  typedef Instruction::op_iterator op_iterator;
  for (op_iterator I = Dst->op_begin(), E = Dst->op_end(); I != E; ++I) {
    Instruction *DepInst = dyn_cast<Instruction>(I);

    if (DepInst == 0 || isa<PHINode>(DepInst)) continue;

    // Ignore the Dep that is not in the same BB with Src.
    if (DepInst->getParent() != Src->getParent()) continue;

    if (DepInst == Src || UseSet.count(DepInst)) {
      // Src is (indirectly) used by Dst.
      UseSet.insert(Dst);
      return true;
    }
  }

  return false;
}

bool MemoryAccessCoalescing::canBeFused(Instruction *LHS, Instruction *RHS) {
  // Only fuse the memory accesses with the same access type.
  if (LHS->getOpcode() != RHS->getOpcode()) return false;

  const SCEVConstant *DeltaSCEV = getAddressDistant(LHS, RHS);

  // Cannot fuse two memory access with unknown distance.
  if (!DeltaSCEV) return false;

  int64_t Delta = DeltaSCEV->getValue()->getSExtValue();
  // Make sure LHS is in the lower address.
  if (Delta < 0) {
    Delta = -Delta;
    std::swap(LHS, RHS);
  }

  // Check if we can fuse the address of RHS into LHS
  uint64_t FusedWidth = std::max<unsigned>(getAccessSizeInBytes(LHS),
                                           Delta + getAccessSizeInBytes(RHS));
  // Do not generate unaligned memory access.
  if (FusedWidth > getAccessAlignment(LHS)) return false;

  if (LHS->mayWriteToMemory()) {
    // Cannot store with irregular byteenable at the moment.
    if (!isPowerOf2_64(FusedWidth)) return false;

    // For the stores, we must make sure the higher address is just next to
    // the lower address.
    if (Delta && uint64_t(Delta) != getAccessSizeInBytes(LHS)) return false;

    // LHS and RHS have the same address.
    if (Delta == 0 &&
      // Need to check if the two access are writing the same data, and writing
      // the same size.
        (!isWritingTheSameValue(LHS, RHS)))
        return false;
  }

  uint64_t BusWidth = getFUDesc<VFUMemBus>()->getDataWidth() / 8;

  // Don't exceed the width of data port of MemBus.
  if (FusedWidth > BusWidth) return false;

  return true;
}

void MemoryAccessCoalescing::moveUsesAfter(Instruction *MergeFrom,
                                           Instruction *MergeTo,
                                           InstSetTy &UseSet) {
  DEBUG(dbgs() << "Going to merge:\n" << *MergeFrom << "into\n" << *MergeTo);
  BasicBlock *BB = MergeFrom->getParent();

  typedef BasicBlock::iterator iterator;
  iterator L = MergeFrom, InsertPos = MergeTo;

  // Skip MergeFromMI.
  ++L;
  // Insert moved instruction after MergeToMI.
  ++InsertPos;

  while (L != iterator(MergeTo)) {
    assert(L != BB->end() && "Iterator pass the end of the list!");
    Instruction *MIToMove = L++;

    if (!UseSet.count(MIToMove)) continue;

    MIToMove->removeFromParent();
    BB->getInstList().insert(InsertPos, MIToMove);
  }
}

static Value *CreateMaskedValue(IRBuilder<> &Builder, Type *DstType,
                                Type *SrcType, Value *SrcValue, DataLayout *TD) {
  APInt MaskInt =  APInt::getLowBitsSet(TD->getTypeStoreSizeInBits(DstType),
                                        TD->getTypeAllocSizeInBits(SrcType));
  dbgs() << "Create mask " << MaskInt.toString(16, false) << " for " << *SrcType
         << " -> " << *DstType << '\n';
  Value *V = Builder.CreateZExtOrBitCast(SrcValue, DstType);
  return Builder.CreateAnd(V,  ConstantInt::get(DstType, MaskInt));
}

void MemoryAccessCoalescing::fuseInstruction(Instruction *From, Instruction *To) {
  assert(isLoadStore(From) && isLoadStore(To) 
         && "Unexpected type of Instruction to merge!!");
  assert(From->getOpcode() == To->getOpcode() && "Cannot mixing load and store!");

  // Get the new address, i.e. the lower address which has a bigger
  // alignment.
  Value *LowerLoadValue = getLoadedValue(From);
  Value *LowerAddr = getPointerOperand(From);
  Value *LowerStoredValue = getStoredValue(From);
  Type *LowerType = getAccessType(From);
  unsigned LowerSizeInBytes = getAccessSizeInBytes(From);

  Value *HigherLoadValue = getLoadedValue(To);
  Value *HigherAddr = getPointerOperand(To);
  Value *HigherStoredValue = getStoredValue(To);
  Type *HigherType = getAccessType(To);
  unsigned HigherSizeInBytes = getAccessSizeInBytes(To);

  int64_t Delta = getAddressDistantInt(To, From);
  // Make sure lower address is actually lower.
  if (Delta < 0) {
    Delta = - Delta;
    std::swap(LowerAddr, HigherAddr);
    std::swap(LowerStoredValue, HigherStoredValue);
    std::swap(LowerLoadValue, HigherLoadValue);
    std::swap(LowerSizeInBytes, HigherSizeInBytes);
    std::swap(LowerType, HigherType);
  }

  int64_t NewSizeInBytes = std::max<unsigned>(LowerSizeInBytes,
                                              Delta + HigherSizeInBytes);
  NewSizeInBytes = NextPowerOf2(NewSizeInBytes - 1);

  // Get the Byte enable.
  Type *NewType = IntegerType::get(From->getContext(), NewSizeInBytes * 8);
  assert(((LowerSizeInBytes + HigherSizeInBytes == NewSizeInBytes)
           || isa<LoadInst>(To))
         && "New Access writing extra bytes!");

  IRBuilder<> Builder(To);
  Value *NewAddr
    = Builder.CreatePointerCast(LowerAddr, PointerType::get(NewType, 0));
  // Build the new data to store by concatenating them together.
  if (StoreInst *S = dyn_cast<StoreInst>(To)) {
    // Get the Masked Values.
    Value *ExtendLowerData
      = CreateMaskedValue(Builder, NewType, LowerType, LowerStoredValue, TD);
    Value *ExtendHigherData
      = CreateMaskedValue(Builder, NewType, HigherType, HigherStoredValue, TD);
    // Shift the higher data to the right position.
    ExtendHigherData = Builder.CreateShl(ExtendHigherData, LowerSizeInBytes * 8);
    // Concat the values to build the new stored value.
    Value *NewStoredValue = Builder.CreateOr(ExtendHigherData, ExtendLowerData);
    S->getOperandUse(0).set(NewStoredValue);
    // Update the pointer.
    S->getOperandUse(1).set(NewAddr);
    S->setAlignment(NewSizeInBytes);
  }

  // Update the result registers.
  if (isa<LoadInst>(To)) {
    LoadInst *NewLoad = Builder.CreateLoad(NewAddr, false);
    // Update the pointer.
    NewLoad->setAlignment(NewSizeInBytes);
    // Extract the lower part and the higher part of the value.
    Value *ShiftedHigerValue
      = Builder.CreateLShr(NewLoad, LowerSizeInBytes * 8);
    Value *NewHigherValue
      = Builder.CreateZExtOrTrunc(ShiftedHigerValue, HigherType);

    Value *NewLowerValue = Builder.CreateZExtOrTrunc(NewLoad, LowerType);

    LowerLoadValue->replaceAllUsesWith(NewLowerValue);
    HigherLoadValue->replaceAllUsesWith(NewHigherValue);    
  }
  
  ++MemOpFused;
}

Pass *llvm::createMemoryAccessCoalescingPass() {
  return new MemoryAccessCoalescing();
}
