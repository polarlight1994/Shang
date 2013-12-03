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
#include "vast/Passes.h"
#include "vast/FUInfo.h"
#include "vast/Utilities.h"

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

    DEBUG(dbgs() << "Run Memory Access Coalescer on " << F.getName() << '\n');

    for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
      // We may scan the same BB more than once to fuse all compatible pairs.
      while (runOnBasicBlock(*I))
        changed = true;

    return changed;
  }

  bool runOnBasicBlock(BasicBlock &BB);
  Instruction *findFusingCandidate(BasicBlock::iterator I, BasicBlock &BB);

  typedef SmallPtrSet<Instruction*, 8> InstSetTy;
  bool canBeFused(Instruction *LowerInst, Instruction *HigherInst);
  bool hasMemDependency(Instruction *DstInst, Instruction *SrcInst);
  bool trackUsesOfSrc(InstSetTy &UseSet, Instruction *Src, Instruction *Dst);

  static bool isVolatile(Instruction *I) {
    if (LoadInst *L = dyn_cast<LoadInst>(I))
      return L->isVolatile();

    if (StoreInst *S = dyn_cast<StoreInst>(I))
      return S->isVolatile();

    assert(isa<CallInst>(I) && "Unexpected instruction type!");

    return false;
  }

  AliasAnalysis::Location getPointerLocation(Instruction *I) const {
    if (LoadInst *LI = dyn_cast<LoadInst>(I))
      return AA->getLocation(LI);

    if (StoreInst *SI = dyn_cast<StoreInst>(I))
      return AA->getLocation(SI);

    llvm_unreachable("Unexpected instruction type!");
    return AliasAnalysis::Location();
  }

  bool isNoAlias(Instruction *Src, Instruction *Dst) const {
    return AA->isNoAlias(getPointerLocation(Src), getPointerLocation(Dst));
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

  const SCEVConstant *getAddressDistant(Instruction *LowerInst,
                                        Instruction *HigherInst) {
    Value *LowerAddr = getPointerOperand(LowerInst),
          *HigherAddr = getPointerOperand(HigherInst);
    const SCEV *LowerSCEV = SE->getSCEV(LowerAddr),
               *HigherSCEV = SE->getSCEV(HigherAddr);

    return dyn_cast<SCEVConstant>(SE->getMinusSCEV(HigherSCEV, LowerSCEV));
  }

  int64_t getAddressDistantInt(Instruction *LowerInst, Instruction *HigherInst) {
    const SCEVConstant *DeltaSCEV = getAddressDistant(LowerInst, HigherInst);
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
  void fuseInstruction(Instruction *LowerInst, Instruction *HigherInst);
};
}

char MemoryAccessCoalescing::ID = 0;

INITIALIZE_PASS_BEGIN(MemoryAccessCoalescing,
                      "shang-mem-coalesce", "Memory Coalescing", false, false)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
  INITIALIZE_PASS_DEPENDENCY(DependenceAnalysis)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(MemoryAccessCoalescing,
                    "shang-mem-coalesce", "Memory Coalescing", false, false)

 bool MemoryAccessCoalescing::runOnBasicBlock(BasicBlock &BB) {
  bool Changed = false;

  DEBUG(dbgs() << "Before memory access coalescing\n"; BB.dump(););

  // 1. Collect the fusing candidates.
  for (BasicBlock::iterator I = BB.begin(), E = BB.end(); I != E; /*++I*/) {
    Instruction *Inst = I;
    // Only fuse the accesses via memory bus
    if (!isLoadStore(Inst))  {
      ++I;
      continue;
    }

    if (Instruction *FusingCandidate = findFusingCandidate(Inst, BB)) {
      DEBUG(dbgs() << "Going to fuse\n\t" << *FusingCandidate << " into \n\t"
                   << *Inst << '\n');
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
    DEBUG(dbgs() << "BB Changed:\n"; BB.dump(););
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
  bool ForceDstDep = isCall(DstInst);

  if (!isLoadStore(DstInst) && !ForceDstDep) return false;

  bool DstMayWrite = DstInst->mayWriteToMemory() || isVolatile(DstInst);

  // Do not add loop to dependent graph.
  if (SrcInst == DstInst) return false;

  // Check if source will access memory.
  if (!isLoadStore(SrcInst) && !isCall(SrcInst))
    return false;

  // Handle force dependency.
  if (ForceDstDep || isCall(SrcInst)) return true;

  bool SrcMayWrite = SrcInst->mayWriteToMemory() || isVolatile(SrcInst);

  // Ignore RAR dependency.
  if (!SrcMayWrite && ! DstMayWrite) return false;

  // Is DstInst depends on SrcInst?
  return !isNoAlias(SrcInst, DstInst);
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

bool MemoryAccessCoalescing::canBeFused(Instruction *LowerInst,
                                        Instruction *HigherInst) {
  // Only fuse the memory accesses with the same access type.
  if (LowerInst->getOpcode() != HigherInst->getOpcode()) return false;

  // Do not fuse the Volatile load/stores.
  if (isVolatile(LowerInst) || isVolatile(HigherInst)) return false;

  const SCEVConstant *DistanceSCEV = getAddressDistant(LowerInst, HigherInst);
  // Cannot fuse two memory access with unknown distance.
  if (!DistanceSCEV) return false;

  int64_t AddrDistance = DistanceSCEV->getValue()->getSExtValue();

  DEBUG(dbgs() << "Get lower " << *LowerInst << '\n'
               << "    higher" << *HigherInst << '\n'
               << "    Address distance " << AddrDistance << '\n');

  // Make sure LHS is in the lower address.
  if (AddrDistance < 0) {
    AddrDistance = -AddrDistance;
    std::swap(LowerInst, HigherInst);
  }

  // Check if we can fuse the address of RHS into LHS
  uint64_t FusedWidth
    = std::max<unsigned>(getAccessSizeInBytes(LowerInst),
                         AddrDistance + getAccessSizeInBytes(HigherInst));
  // Do not generate unaligned memory access.
  if (FusedWidth > getAccessAlignment(LowerInst)) return false;

  if (LowerInst->mayWriteToMemory()) {
    // Cannot store with irregular byteenable at the moment.
    if (!isPowerOf2_64(FusedWidth)) return false;

    // For the stores, we must make sure the higher address is just next to
    // the lower address.
    if (AddrDistance &&
        uint64_t(AddrDistance) != getAccessSizeInBytes(LowerInst))
      return false;

    // LHS and RHS have the same address.
    if (AddrDistance == 0 &&
      // Need to check if the two access are writing the same data, and writing
      // the same size.
        (!isWritingTheSameValue(LowerInst, HigherInst)))
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
  DEBUG(dbgs() << "Create mask " << MaskInt.toString(16, false)
               << " for " << *SrcType << " -> " << *DstType << '\n');
  Value *V = Builder.CreateZExtOrBitCast(SrcValue, DstType);
  return Builder.CreateAnd(V,  ConstantInt::get(DstType, MaskInt));
}

void MemoryAccessCoalescing::fuseInstruction(Instruction *LowerInst,
                                             Instruction *HigherInst) {
  assert(isLoadStore(LowerInst) && isLoadStore(HigherInst)
         && "Unexpected type of Instruction to merge!!");
  assert(LowerInst->getOpcode() == HigherInst->getOpcode()
         && "Cannot mixing load and store!");

  // Get the new address, i.e. the lower address which has a bigger
  // alignment.
  Value *LowerLoadValue = getLoadedValue(LowerInst);
  Value *LowerAddr = getPointerOperand(LowerInst);
  Value *LowerStoredValue = getStoredValue(LowerInst);
  Type *LowerType = getAccessType(LowerInst);
  unsigned LowerSizeInBytes = getAccessSizeInBytes(LowerInst);
  unsigned LowerAlignment = getAccessAlignment(LowerInst);

  Value *HigherLoadValue = getLoadedValue(HigherInst);
  Value *HigherAddr = getPointerOperand(HigherInst);
  Value *HigherStoredValue = getStoredValue(HigherInst);
  Type *HigherType = getAccessType(HigherInst);
  unsigned HigherSizeInBytes = getAccessSizeInBytes(HigherInst);
  unsigned HigherAlignment = getAccessAlignment(HigherInst);

  int64_t AddrDistant = getAddressDistantInt(LowerInst, HigherInst);
  // Make sure lower address is actually lower.
  if (AddrDistant < 0) {
    AddrDistant = - AddrDistant;
    std::swap(LowerAddr, HigherAddr);
    std::swap(LowerStoredValue, HigherStoredValue);
    std::swap(LowerLoadValue, HigherLoadValue);
    std::swap(LowerSizeInBytes, HigherSizeInBytes);
    std::swap(LowerType, HigherType);
    std::swap(LowerAlignment, HigherAlignment);
  }

  int64_t NewSizeInBytes = std::max<unsigned>(LowerSizeInBytes,
                                              AddrDistant + HigherSizeInBytes);
  NewSizeInBytes = NextPowerOf2(NewSizeInBytes - 1);

  assert(NewSizeInBytes <= getFUDesc<VFUMemBus>()->getDataWidth() / 8
         && "Bad load/store size!");

  // Get the Byte enable.
  Type *NewType = IntegerType::get(LowerInst->getContext(), NewSizeInBytes * 8);
  assert(((LowerSizeInBytes + HigherSizeInBytes == NewSizeInBytes)
           || isa<LoadInst>(HigherInst))
         && "New Access writing extra bytes!");

  assert(LowerAlignment >= NewSizeInBytes && "Bad alignment of lower access!");

  IRBuilder<> Builder(HigherInst);
  Value *NewAddr
    = Builder.CreatePointerCast(LowerAddr, PointerType::get(NewType, 0));
  // Build the new data to store by concatenating them together.
  if (isa<StoreInst>(HigherInst)) {
    // Get the Masked Values.
    Value *ExtendLowerData
      = CreateMaskedValue(Builder, NewType, LowerType, LowerStoredValue, TD);
    Value *ExtendHigherData
      = CreateMaskedValue(Builder, NewType, HigherType, HigherStoredValue, TD);
    // Shift the higher data to the right position.
    ExtendHigherData = Builder.CreateShl(ExtendHigherData, LowerSizeInBytes * 8);
    // Concat the values to build the new stored value.
    Value *NewStoredValue = Builder.CreateOr(ExtendHigherData, ExtendLowerData);
    StoreInst *NewStore = Builder.CreateStore(NewStoredValue, NewAddr, false);
    NewStore->setAlignment(LowerAlignment);
  }

  // Update the result registers.
  if (isa<LoadInst>(HigherInst)) {
    LoadInst *NewLoad = Builder.CreateLoad(NewAddr, false);
    // Update the pointer.
    NewLoad->setAlignment(LowerAlignment);
    // Extract the lower part and the higher part of the value.
    Value *ShiftedHigerValue = Builder.CreateLShr(NewLoad, AddrDistant * 8);
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
