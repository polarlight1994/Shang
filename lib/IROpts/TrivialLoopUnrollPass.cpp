//===-- TrivialLoopUnroll.cpp - Loop unroller pass ------------------------===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass implements a simple loop unroller.  It works best when loops have
// been canonicalized by the -indvars pass, allowing it to determine the trip
// counts of loops easily.
//===----------------------------------------------------------------------===//

#include "vast/Passes.h"
#include "vast/DesignMetrics.h"
#include "vast/FUInfo.h"
#include "vast/LuaI.h"
#include "vast/Utilities.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-loop-unroll"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;
STATISTIC(NumUnrolled, "Number of loops unrolled");
STATISTIC(NumTailUnrolled, "Number of loops tail unrolled");

static cl::opt<unsigned>
ThresholdFactor("shang-unroll-threshold-factor",
                cl::desc("Factor to be multipied to the unroll threshold"),
                cl::init(1));
static cl::opt<unsigned>
TailUnrollThreshold("shang-unroll-epilog-threshold",
                    cl::desc("Threshold to perform tail unroll to ensure"
                             "  the tripcount is a mulitple of the unroll count"),
                    cl::init(128));

namespace vast {
int getLoopDepDist(const SCEV *SSAddr, const SCEV *SDAddr, bool SrcBeforeDest,
                   unsigned ElemSizeInByte, ScalarEvolution *SE) {
  // Use SCEV to compute the dependencies distance.
  const SCEV *Distance = SE->getMinusSCEV(SSAddr, SDAddr);
  // TODO: Get range.
  if (const SCEVConstant *C = dyn_cast<SCEVConstant>(Distance)) {
    int ItDistance = C->getValue()->getSExtValue();
    if (ItDistance >= 0)
      // The pointer distance is in Byte, but we need to get the distance in
      // Iteration.
      return getLoopDepDist(SrcBeforeDest, ItDistance / ElemSizeInByte);

    // No dependency.
    return -1;
  }

  return getLoopDepDist(SrcBeforeDest);
}

int getLoopDepDist(bool SrcBeforeDest, int Distance){
  if (!SrcBeforeDest && (Distance == 0)) Distance = 1;

  assert(Distance >= 0 && "Do not create a dependence with diff small than 0!");
  return Distance;
}
}

namespace {
class TrivialLoopUnroll : public LoopPass {
public:
  static char ID; // Pass ID, replacement for typeid
  TrivialLoopUnroll() : LoopPass(ID) {
    initializeTrivialLoopUnrollPass(*PassRegistry::getPassRegistry());
  }

  bool tailUnroll(Loop *L, ScalarEvolution *SE, unsigned Count);

  bool runOnLoop(Loop *L, LPPassManager &LPM);

  /// This transformation requires natural loop information & requires that
  /// loop preheaders be inserted into the CFG...
  ///
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<AliasAnalysis>();
    AU.addRequired<DataLayout>();
    AU.addRequired<LoopInfo>();
    AU.addPreserved<LoopInfo>();
    AU.addRequiredID(LoopSimplifyID);
    AU.addPreservedID(LoopSimplifyID);
    AU.addRequiredID(LCSSAID);
    AU.addPreservedID(LCSSAID);
    AU.addRequired<ScalarEvolution>();
    AU.addPreserved<ScalarEvolution>();
    // FIXME: Loop unroll requires LCSSA. And LCSSA requires dom info.
    // If loop unroll does not preserve dom info then LCSSA pass on next
    // loop will receive invalid dom info.
    // For now, recreate dom info, if loop is unrolled.
    AU.addPreserved<DominatorTree>();
  }
};

struct MemoryAccessAligner : public FunctionPass {
  static char ID;

  MemoryAccessAligner() : FunctionPass(ID) {
    initializeMemoryAccessAlignerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesCFG();
    AU.addRequired<ScalarEvolution>();
    AU.addPreserved<ScalarEvolution>();
  }

  bool runOnFunction(Function &F) {
    bool changed = false;
    // Only handle the allocas in entry block.
    BasicBlock &Entry = F.getEntryBlock();
    for (BasicBlock::iterator I = Entry.begin(), E = Entry.end(); I != E; ++I) {
      AllocaInst *AI = dyn_cast<AllocaInst>(I);

      if (AI == 0) continue;
      AI->setAlignment(std::max(8u, AI->getAlignment()));
    }

    ScalarEvolution &SE = getAnalysis<ScalarEvolution>();
    for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
      alignMemoryAccess(I, SE);

    return changed;
  }

  static void alignMemoryAccess(BasicBlock *BB, ScalarEvolution &SE) {
    for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
      if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
        const SCEV *Ptr = SE.getSCEV(LI->getPointerOperand());
        unsigned Align = (1 << SE.GetMinTrailingZeros(Ptr));
        if (Align > LI->getAlignment()) LI->setAlignment(Align);
      } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
        const SCEV *Ptr = SE.getSCEV(SI->getPointerOperand());
        unsigned Align = (1 << SE.GetMinTrailingZeros(Ptr));
        if (Align > SI->getAlignment()) SI->setAlignment(Align);
      }
    }
  }

  static void alignMemoryAccess(Loop *L, ScalarEvolution &SE) {
    typedef Loop::block_iterator block_iterator;
    for (block_iterator I = L->block_begin(), E = L->block_end(); I != E; ++I)
      alignMemoryAccess(*I, SE);
  }
};

// The dependency graph of the loop body.
class LoopDepGraph {
  LoopDepGraph(const LoopDepGraph &) LLVM_DELETED_FUNCTION;
  const LoopDepGraph&operator=(const LoopDepGraph &)LLVM_DELETED_FUNCTION;

public:
  typedef std::map<const Value*, unsigned> DepInfoTy;
  // The map that hold the dependency distance from the load instructions.
  // In contrast, the the dependency graph should only contains load and store.
  typedef std::map<const Value*, DepInfoTy> DepMapTy;
private:
  DepMapTy DepMap;

protected:
  // The current Loop.
  Loop *L;

  ScalarEvolution &SE;

  DataLayout *TD;

private:
  const Instruction *getAsNonTrivial(const Value *Src) {
    if (const Instruction *Inst = dyn_cast<Instruction>(Src)) {
      if (Inst->mayHaveSideEffects() || Inst->mayReadOrWriteMemory())
        return Inst;

      // The PHINode introducing the back-edge are also nontrivial.
      if (const PHINode *PN = dyn_cast<PHINode>(Inst))
        if (PN->getParent() == L->getHeader())
          return Inst;
    }

    return 0;
  }

  static bool insertDep(DepInfoTy &Dep, const Value *Src, unsigned Distance) {
    DepInfoTy::iterator at = Dep.find(Src);
    // Simply create the entry if it does not exist.
    if (at == Dep.end()) {
      Dep.insert(std::make_pair(Src, Distance));
      return true;
    }

    // Relax the distance in the DepInfo.
    unsigned &OldDistance = at->second;

    // No need to change the old distance.
    if (OldDistance <= Distance) return false;

    // Replace the distance by a shorter one.
    OldDistance = Distance;
    return true;
  }

  void insertDep(const Value *Src, const Value *Dst, unsigned Distance) {
    DepInfoTy &Deps = DepMap[Dst];
    assert(!Deps.empty() && "Unexpected empty dependence set!");
    insertDep(Deps, Src, Distance);
  }

  static void mergeDeps(DepInfoTy &To, const DepInfoTy &From) {
    typedef DepInfoTy::const_iterator iterator;
    for (iterator I = From.begin(), E = From.end(); I != E; ++I)
      insertDep(To, I->first, I->second);
  }

  void buildDep(const Instruction *Inst, unsigned Distance);
  void buildDepForBackEdges();
  void buildMemDep(ArrayRef<const Instruction*> MemOps, AliasAnalysis *AA);
  static
  AliasAnalysis::Location getLocation(const Instruction *I, AliasAnalysis *AA) {
    if (const LoadInst *LI = dyn_cast<LoadInst>(I))
      return AA->getLocation(LI);

    if (const StoreInst *SI = dyn_cast<StoreInst>(I))
      return AA->getLocation(SI);

    llvm_unreachable("Unexpected instruction!");
    return AliasAnalysis::Location();
  }

  int getDepDistance(const Instruction *Src, const Instruction *Dst,
                     AliasAnalysis *AA, bool SrcBeforeDst);

  void buildTransitiveClosure(ArrayRef<const Instruction*> MemOps);
public:

  LoopDepGraph(Loop *L, DataLayout *TD, ScalarEvolution &SE)
    : L(L), SE(SE), TD(TD) {}

  typedef DepMapTy::const_iterator iterator;

  iterator closure_begin() const { return DepMap.begin(); }
  iterator closure_end() const { return DepMap.end(); }

  // Build the dependency graph of the loop body, return true if success, false
  // otherwise.
  bool buildDepGraph(LoopInfo *LI, AliasAnalysis *AA, DesignMetrics *Metrics = 0);
};

class LoopMetrics : public DesignMetrics, public LoopDepGraph {
  // For the load/store, fusing the number of unrolled instance cause the memory
  // bandwidth saturated.
  typedef std::map<const Instruction*, unsigned> Inst2IntMap;
  Inst2IntMap SaturatedCounts;

  // The number of parallel iteration of the loop.
  unsigned NumParallelIt;

  unsigned getSaturateCount(Value *Val, Value *Ptr);
public:

  LoopMetrics(Loop *L, DataLayout *TD, ScalarEvolution &SE)
    : DesignMetrics(TD), LoopDepGraph(L, TD, SE), NumParallelIt(1) {}

  bool initialize(LoopInfo *LI, AliasAnalysis *AA);

  bool isUnrollAccaptable(unsigned Count, uint64_t UnrollThreshold,
                          uint64_t Alpha = 1, uint64_t Beta = 8,
                          uint64_t Gama = (2048 * 64)) const;
  unsigned exposedMemoryCoalescing(unsigned Count) const;
  unsigned getNumParallelIteration() const { return NumParallelIt; }
};
}

void LoopDepGraph::buildDep(const Instruction *Inst, unsigned Distance) {
  typedef Instruction::const_op_iterator op_iterator;
  DepInfoTy &DepInfo = DepMap[Inst];

  for (op_iterator I = Inst->op_begin(), E = Inst->op_end(); I != E; ++I) {
    const Value *Src = *I;
    // Simply insert the nontrivial instruction to DepInfo.
    if (const Instruction *Nontrivial = getAsNonTrivial(Src)) {
      // Ignore the Instruction which is outside the loop. If the instruction
      // is inside the loop, we should visited it as we are visiting the
      // instructions in topological order.
      if (DepMap.count(Nontrivial)) insertDep(DepInfo, Nontrivial, Distance);
      continue;
    }

    // If Src is not a leaf, we need to forward all its dependencies to the
    // current DepInfo.
    DepMapTy::iterator at = DepMap.find(Src);
    // Ignore the loop invariants.
    if (at == DepMap.end()) continue;

    // Merge the DepInfo.
    mergeDeps(DepInfo, at->second);
  }

  // Prevent loop when building dependency graph.
  DepInfo.erase(Inst);
  // Make the current node depends on the header of the Loop (i.e. the root of
  // the dependency graph) if it do not have any source.
  if (DepInfo.empty())  insertDep(DepInfo, L->getHeader(), 0);
}

bool LoopDepGraph::buildDepGraph(LoopInfo *LI, AliasAnalysis *AA,
                                 DesignMetrics *Metrics){
  LoopBlocksDFS DFS(L);
  DFS.perform(LI);

  std::vector<const Instruction*> Nontrivials, TrivialInsts;

  // Visit the blocks in top-order.
  typedef LoopBlocksDFS::RPOIterator top_iterator;
  typedef BasicBlock::iterator iterator;
  for (top_iterator I = DFS.beginRPO(), E = DFS.endRPO(); I != E; ++I) {
    BasicBlock *BB = *I;
    for (iterator BI = BB->begin(), BE = BB->end(); BI != BE; ++BI) {
      Instruction *Inst = BI;
      // Ignore the loops with CallSite in its body.
      if (isa<CallInst>(Inst) || isa<InvokeInst>(Inst)) return false;

      // Collect the nontrivial instructions, i.e. the load/store/call.
      if (const Instruction *Nontrivial = getAsNonTrivial(Inst))
        Nontrivials.push_back(Nontrivial);
      else
        TrivialInsts.push_back(Inst);

      // Build flow-dependences for the current Instruction, the iterate
      // distance of the dependences are 0.
      buildDep(Inst, 0);

      // Also estimate the implementation cost of the instruction is the
      // design metrics is available. We also need to ignore loop invariant.
      if (Metrics && !L->isLoopInvariant(Inst)) Metrics->visit(Inst);
    }
  }

  buildDepForBackEdges();

  // Remove the entry of the trivial instruction in the dependency map.
  while (!TrivialInsts.empty()) {
    DepMap.erase(TrivialInsts.back());
    TrivialInsts.pop_back();
  }

  buildMemDep(Nontrivials, AA);

  // Simply create the entry for the root of the dependencies graph.
  DepMap.insert(std::make_pair(L->getHeader(), DepInfoTy()));

  // Build the transitive closure of the dependency relation.
  // FIXME: Calculate the max parallel distance for each SCC.
  buildTransitiveClosure(Nontrivials);

  return true;
}

void LoopDepGraph::buildTransitiveClosure(ArrayRef<const Instruction*> MemOps) {
  bool Changed = true;
  while (Changed) {
    Changed = false;

    for (unsigned i = 0; i < MemOps.size(); ++i) {
      const Instruction *CurInst = MemOps[i];
      DepInfoTy &CurDep = DepMap[CurInst];
      assert(!CurDep.empty() && "Unexpected empty dependency map!");

      // Iterate over the source of the dependencies to
      // forward the dependencies of sources to current node.
      typedef DepInfoTy::iterator iterator;
      for (iterator I = CurDep.begin(), E = CurDep.end(); I != E; ++I) {
        const Value *Src = I->first;
        unsigned Distance = I->second;

        const DepInfoTy &SrcDep = DepMap[Src];
        assert((!SrcDep.empty() || isa<BasicBlock>(Src))
               && "Unexpected empty dependency map!");

        // Build the dependencies from (Source of Src) -> Src -> CurInst.
        typedef DepInfoTy::const_iterator const_iterator;
        for (const_iterator SI = SrcDep.begin(), SE = SrcDep.end();
             SI != SE; ++SI) {
          unsigned NewDistance = Distance + SI->second;
          Changed |= insertDep(CurDep, SI->first, NewDistance);
        }
      }
    }
  }
}

void LoopDepGraph::buildDepForBackEdges() {
  BasicBlock *Header = L->getHeader();

  typedef BasicBlock::iterator iterator;
  for (iterator I = Header->begin(); isa<PHINode>(I); ++I)
    // The distance of the back-edges are 1.
    buildDep(I, 1);
}

int LoopDepGraph::getDepDistance(const Instruction *Src, const Instruction *Dst,
                                 AliasAnalysis *AA, bool SrcBeforeDst) {
  // Ignore RAR dependencies.
  if (!Src->mayWriteToMemory() && !Dst->mayWriteToMemory())
    return -1;

  AliasAnalysis::Location SrcLoc = getLocation(Src, AA),
                          DstLoc = getLocation(Dst, AA);
  Value *SrcAddr = const_cast<Value*>(SrcLoc.Ptr),
        *DstAddr = const_cast<Value*>(DstLoc.Ptr);

  if (AA->isNoAlias(SrcLoc, DstLoc)) return -1;

  if (L->isLoopInvariant(SrcAddr) && L->isLoopInvariant(DstAddr))
    return getLoopDepDist(SrcBeforeDst);

  if (SrcLoc.Size == DstLoc.Size) {
    const SCEV *SAddrSCEV = SE.getSCEVAtScope(SrcAddr, L);
    const SCEV *DAddrSCEV = SE.getSCEVAtScope(DstAddr, L);
    return getLoopDepDist(SAddrSCEV, DAddrSCEV, SrcBeforeDst, SrcLoc.Size, &SE);
  }

  return getLoopDepDist(SrcBeforeDst);
}

void LoopDepGraph::buildMemDep(ArrayRef<const Instruction*> MemOps,
                               AliasAnalysis *AA) {
  for (unsigned i = 1; i < MemOps.size(); ++i) {
    const Instruction *Dst = MemOps[i];
    // Ignore the PHI's.
    if (isa<PHINode>(Dst))  continue;

    for (unsigned j = 0; j < i; ++j) {
      const Instruction *Src = MemOps[j];
      // Ignore the PHI's.
      if (isa<PHINode>(Src))  continue;

      int Distance = getDepDistance(Src, Dst, AA, true);
      if (Distance >= 0) insertDep(Src, Dst, Distance);

      Distance = getDepDistance(Dst, Src, AA, false);
      if (Distance >= 0) insertDep(Dst, Src, Distance);
    }
  }
}

unsigned LoopMetrics::getSaturateCount(Value *Val, Value *Ptr) {
  // Reject stores that are so large that they overflow an unsigned.
  uint64_t SizeInBits = TD->getTypeSizeInBits(Val->getType());
  if ((SizeInBits % 8 != 0) || !isInt<32>(SizeInBits)) return 1;

  // Check to see if the stride matches the size of the store.  If so, then we
  // know that every byte is touched in the loop.
  unsigned SizeInBytes = (unsigned)SizeInBits / 8;

  // The loop invariant load/store will be eliminated by dead load/store
  // elimination.
  if (L->isLoopInvariant(Ptr)) return UINT32_MAX;

  const SCEVAddRecExpr *PtrSCEV
    = dyn_cast<SCEVAddRecExpr>(SE.getSCEVAtScope(Ptr, L));
  // See if the pointer expression is an AddRec like {base,+,1} on the current
  // loop, which indicates a strided store.  If we have something else, it's a
  // random store we can't handle.
  if (PtrSCEV == 0 || PtrSCEV->getLoop() != L || !PtrSCEV->isAffine()) return 1;

  unsigned BaseAlignment = (1u << SE.GetMinTrailingZeros(PtrSCEV->getStart()));

  // Not benefit from load/store fusion if we do not have bigger alignment.
  if (BaseAlignment <= SizeInBytes) return 1;

  const SCEVConstant *Stride = dyn_cast<SCEVConstant>(PtrSCEV->getOperand(1));

  // Unknown stride, do not know to calculate the distance.
  if (Stride == 0) return 1;

  int64_t Stride64 = Stride->getValue()->getSExtValue();

  // Do not mess up with strange stride.
  if (Stride64 <=0) return 1;

  // The distance between pointer in successive iterations, in bytes.
  unsigned Distance = Stride64 * SizeInBytes;
  unsigned BusSizeInBits = LuaI::Get<VFUMemBus>()->getDataWidth();
  unsigned BusSizeInBytes = BusSizeInBits / 8;
  // Dual port RAM double the data bandwitdh!
  // BusSizeInBytes *= 2;

  // Get the number of instances causing the bandwidth of memory bus saturate,
  // based on the byte address.
  return std::max<unsigned>(BusSizeInBytes / Distance, 1);
}

bool LoopMetrics::initialize(LoopInfo *LI, AliasAnalysis *AA) {
  if (!buildDepGraph(LI, AA, this)) return false;

  NumParallelIt = UINT32_MAX;

  typedef LoopDepGraph::iterator iterator;
  DEBUG(dbgs() << "loops in dependency graph:\n");
  for (iterator I = closure_begin(), E = closure_end(); I != E; ++I) {
    if (const Instruction *Inst = dyn_cast<Instruction>(I->first)) {
      const DepInfoTy &Dep = I->second;
      DepInfoTy::const_iterator J = Dep.find(Inst);
      // The loop dose not exist by default, which means the distance is infinite.
      unsigned LoopDistance = UINT32_MAX;

      // A zero distance also means there is no loop
      if (J != Dep.end() && J->second != 0)
        LoopDistance = J->second;

      DEBUG(dbgs() << *Inst << " loop-distance: " << LoopDistance << '\n');
      NumParallelIt = std::min(NumParallelIt, LoopDistance);

      // Calculate the unroll count lead to a memory bus bandwidth saturate.
      if (const StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
        if (!SI->isSimple()) {
          // Cannot fuse strange instruction.
          SaturatedCounts.insert(std::make_pair(SI, 1));
          continue;
        }

        unsigned SaturatedCount
          = getSaturateCount(const_cast<Value*>(SI->getValueOperand()),
                             const_cast<Value*>(SI->getPointerOperand()));
        DEBUG(dbgs().indent(4) << "SaturatedCount: " << SaturatedCount << '\n');

        // The SaturatedCount make sense only if instances can be fused together.
        // That is there is no dependency between the instances.
        SaturatedCount = std::min(SaturatedCount, LoopDistance);
        SaturatedCounts.insert(std::make_pair(SI, SaturatedCount));
      }

      if (const LoadInst *LI = dyn_cast<LoadInst>(Inst)) {
        if (!LI->isSimple()) {
          // Cannot fuse strange instruction.
          SaturatedCounts.insert(std::make_pair(LI, 1));
          continue;
        }

        unsigned SaturatedCount
          = getSaturateCount(const_cast<LoadInst*>(LI),
                             const_cast<Value*>(LI->getPointerOperand()));
        DEBUG(dbgs().indent(4) << "SaturatedCount: " << SaturatedCount << '\n');

        // The SaturatedCount make sense only if instances can be fused together.
        // That is there is no dependency between the instances.
        SaturatedCount = std::min(SaturatedCount, LoopDistance);
        SaturatedCounts.insert(std::make_pair(LI, SaturatedCount));
      }
    }
  }

  return true;
}

unsigned LoopMetrics::exposedMemoryCoalescing(unsigned Count) const {
  unsigned OriginalAccesses = 0, UnrolledAccesses = 0;

  // Iterate over the load/store to calculate the related cost and benefit.
  typedef Inst2IntMap::const_iterator iterator;
  for (iterator I = SaturatedCounts.begin(), E = SaturatedCounts.end();
       I != E; ++I) {
    const Instruction *Inst = I->first;
    unsigned SaturedCount = I->second;

    OriginalAccesses += Count;
    if (isa<StoreInst>(Inst)) OriginalAccesses += Count;

    // The unrolled instances can be fused.
    UnrolledAccesses += Count / SaturedCount;
    if (isa<StoreInst>(Inst)) UnrolledAccesses += Count / SaturedCount;
  }

  assert(OriginalAccesses >= UnrolledAccesses && "Bad calculation!");
  return OriginalAccesses - UnrolledAccesses;
}

bool LoopMetrics::isUnrollAccaptable(unsigned Count, uint64_t UnrollThreshold,
                                     uint64_t Alpha, uint64_t Beta,
                                     uint64_t Gama) const {
  // Handle the trivial case trivially.
  if (Count == 1)
    return true;

  DesignMetrics::DesignCost Cost = getCost();
  uint64_t DataPathCost = Cost.getCostInc(Count, Alpha, 0, 0);
  // Datapath cost increment must be smaller than the threshold.
  if (DataPathCost > UnrollThreshold) return false;

  unsigned StepsElimnated = 0, NumDataFanIn = 0, NumAddressFanIn = 0;

  // Iterate over the load/store to calculate the related cost and benefit.
  typedef Inst2IntMap::const_iterator iterator;
  for (iterator I = SaturatedCounts.begin(), E = SaturatedCounts.end();
       I != E; ++I) {
    const Instruction *Inst = I->first;
    unsigned SaturedCount = I->second;

    // The unrolled instances can be fused.
    if (SaturedCount >= Count) {
      StepsElimnated += Count;
    }

    NumAddressFanIn += Count / SaturedCount;
    if (isa<StoreInst>(Inst)) NumDataFanIn += Count / SaturedCount;
  }

  // For the loop with out any load/stores we believe the loop is parallel.
  if (NumAddressFanIn == 0) StepsElimnated = Count;

  // Calculate the MUX cost increment.
  VFUMux *MUX = LuaI::Get<VFUMux>();
  VFUMemBus *MemBus = LuaI::Get<VFUMemBus>();
  unsigned AddrWidth = MemBus->getAddrWidth();
  unsigned DataWidth = MemBus->getDataWidth();

  int64_t AddrFaninCost
    = int64_t(MUX->getMuxCost(NumAddressFanIn, AddrWidth))
      - int64_t(MUX->getMuxCost(Cost.NumAddrBusFanin, AddrWidth));
  AddrFaninCost = std::max<int64_t>(0, AddrFaninCost);

  int64_t DataFaninCost
    = int64_t(MUX->getMuxCost(NumDataFanIn, DataWidth))
      - int64_t(MUX->getMuxCost(Cost.NumDataBusFanin, DataWidth));
  DataFaninCost = std::max<int64_t>(0, DataFaninCost);

  uint64_t MUXCost = Beta * uint64_t(AddrFaninCost + DataFaninCost);
  uint64_t IncreasedCost = DataPathCost + MUXCost;

  DEBUG(dbgs() << *L << ":\n" << "Count: " << Count
               << ", DataPathCost: " << DataPathCost
               << ", MUXCost: " << MUXCost
               << ", IncreasedCost: " << IncreasedCost
               << ", StepsElimnated: " << StepsElimnated
               << ", NumDataFanIn: " << NumDataFanIn
               << ", NumAddressFanIn: " << NumAddressFanIn
               << "\n");

  return IncreasedCost < Gama * StepsElimnated;
}

Pass *vast::createMemoryAccessAlignerPass() {
  return new MemoryAccessAligner();
}

char MemoryAccessAligner::ID = 0;
INITIALIZE_PASS_BEGIN(MemoryAccessAligner, "memory-access-aligner",
                      "Align the memory access", false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_END(MemoryAccessAligner, "memory-access-aligner",
                    "Align the memory access", false, false)

char TrivialLoopUnroll::ID = 0;
INITIALIZE_PASS_BEGIN(TrivialLoopUnroll, "trivial-loop-unroll",
                      "Unroll trivial loops", false, false)
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(LCSSA)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_END(TrivialLoopUnroll, "trivial-loop-unroll",
                    "Unroll trivial loops", false, false)

Pass *vast::createTrivialLoopUnrollPass() {
  return new TrivialLoopUnroll();
}

bool TrivialLoopUnroll::runOnLoop(Loop *L, LPPassManager &LPM) {
  // Only unroll the deepest loops in the loop nest.
  if (!L->empty()) return false;

  // Make sure the loop is in canonical form, and there is a single
  // exit block only.
  if (!L->isLoopSimplifyForm()) return false;

  LoopInfo *LI = &getAnalysis<LoopInfo>();
  ScalarEvolution *SE = &getAnalysis<ScalarEvolution>();

  BasicBlock *Header = L->getHeader();
  DEBUG(dbgs() << "Loop Unroll: F[" << Header->getParent()->getName()
        << "] Loop %" << Header->getName() << "\n");
  (void)Header;

  // Find trip count and trip multiple if count is not available
  unsigned TripCount = 0;
  unsigned TripMultiple = 1;
  // Find "latch trip count". UnrollLoop assumes that control cannot exit
  // via the loop latch on any iteration prior to TripCount. The loop may exit
  // early via an earlier branch.
  BasicBlock *LatchBlock = L->getLoopLatch();
  if (LatchBlock) {
    TripCount = SE->getSmallConstantTripCount(L, LatchBlock);
    TripMultiple = SE->getSmallConstantTripMultiple(L, LatchBlock);
  }
  // Use a default unroll-count if the user doesn't specify a value
  // and the trip count is a run-time value.  The default is different
  // for run-time or compile-time trip count loops.
  // Conservative heuristic: if we know the trip count, see if we can
  // completely unroll (subject to the threshold, checked below); otherwise
  // try to find greatest modulo of the trip count which is still under
  // threshold value.
  if (TripCount == 0)
    return false;

  unsigned Count = TripCount;

  LoopMetrics Metrics(L, &getAnalysis<DataLayout>(), *SE);
  if (!Metrics.initialize(LI, &getAnalysis<AliasAnalysis>())) {
    DEBUG(dbgs() << "  Not unrolling loop with strange instructions.\n");
    return false;
  }

  // Preform optimizetion on the patial datapath to avoid over estimating the
  // unrolling cost.
  Metrics.optimize();

  // FIXME: Read the threshold from the constraints script.
  uint64_t Threshold = 256000 * ThresholdFactor;

  if (TripCount != 1 && !Metrics.isUnrollAccaptable(Count, Threshold)) {
    DEBUG(dbgs() << "  Too large to fully unroll with count: " << Count
          << " because size >" << Threshold << "\n");
    if (TripCount) {
      // Search a feasible count by binary search.
      unsigned MaxCount = Count, MinCount = 1;

      while (MinCount <= MaxCount) {
        unsigned MidCount = MinCount + (MaxCount - MinCount) / 2;

        if (Metrics.isUnrollAccaptable(MidCount, Threshold)) {
          // MidCount is ok, try a bigger one.
          Count = MidCount;
          MinCount = MidCount + 1;
        } else
          // Else we had to try a smaller count.
          MaxCount = MidCount - 1;
      }

    }

    if (Count < 2) {
      DEBUG(dbgs() << "  could not unroll partially\n");
      return false;
    }
    DEBUG(dbgs() << "  partially unrolling with count: " << Count << "\n");
  }

  //assert(TripCount % Count == 0 && "Bad unroll count!");
  //assert(Metrics.isUnrollAccaptable(Count, Threshold) && "Bad unroll count!");
  if (TripCount % Count != 0) {
    // TODO: Estimate the cost of the tail unroll.
    if (TripCount > (TripCount % Count) * TailUnrollThreshold
        && Metrics.exposedMemoryCoalescing(Count)
        && tailUnroll(L, SE, Count)) {
      SE->forgetLoop(L);

      TripCount = SE->getSmallConstantTripCount(L, LatchBlock);
      TripMultiple = SE->getSmallConstantTripMultiple(L, LatchBlock);
      assert(TripCount % Count == 0 && "InsertEpilog should ensure TripCount % Count!");
    } else {
      // Reduce unroll count to be modulo of TripCount for partial unrolling
      while (Count != 0 && TripCount % Count != 0)
        --Count;
    }
  }

  if (Count < 2) {
    DEBUG(dbgs() << "  could not unroll partially\n");
    return false;
  }

  // Unroll the loop.
  if (!UnrollLoop(L, Count, TripCount, false, TripMultiple, LI, &LPM))
    return false;

#ifndef NDEBUG
  verifyFunction(*LatchBlock->getParent());
#endif

  ++NumUnrolled;

  return true;
}

bool TrivialLoopUnroll::tailUnroll(Loop *L, ScalarEvolution *SE,
                                   unsigned Count) {
  // Ensure there is a single exit block only.
  if (L->getNumBlocks() > 1 || L->getUniqueExitBlock() == 0) return false;

  // Check if we can hack the exiting condition.
  BasicBlock *Latch = L->getLoopLatch();
  assert(Latch && "Cannot get loop latch in 1 bb loop!");

  BranchInst *Br = dyn_cast<BranchInst>(Latch->getTerminator());

  // Do not mess up with the strange cases.
  if (Br == 0 || Br->isUnconditional()) return false;
  
  ICmpInst *ICmp = dyn_cast<ICmpInst>(Br->getCondition());

  // We can only handle the == comparision.
  if (ICmp == 0 || ICmp->getPredicate() != CmpInst::ICMP_EQ) return false;

  DEBUG(dbgs() << "tailUnroll  - Get exit guard:\t" << *ICmp << '\n');

  unsigned ConstIdx = 1;
  Instruction *IndVar = dyn_cast<Instruction>(ICmp->getOperand(0));

  if (IndVar == 0) {
    IndVar = dyn_cast<Instruction>(ICmp->getOperand(1));
    ConstIdx = 0;
  }

  if (IndVar == 0) return false;

  DEBUG(dbgs() << "tailUnroll - Get IndVar:\t" << *IndVar << '\n');

  Type *IndVarTy = IndVar->getType();
  Loop *ParentLoop = L->getParentLoop();

  // Build the map for the exit value.
  ValueToValueMapTy VMap;
  SmallVector<Instruction*, 8> Worklist, NewInsts;

  while (SE->getSmallConstantTripCount(L, Latch) % Count) {
    VMap.clear();
    Worklist.clear();

    const SCEV *IndVarExit = SE->getSCEVAtScope(IndVar, ParentLoop);
    assert(!isa<SCEVCouldNotCompute>(IndVarExit) && "Bad Exit value!");
    DEBUG(dbgs() << "tailUnroll - Get IndValExit:\t" << *IndVarExit << '\n');

    SCEVExpander Expander(*SE, "LoopEpilog");
    const SCEV *ExitMinusOne
      = SE->getAddExpr(IndVarExit, SE->getConstant(IndVarTy, -1));
    Value *ExitVal = Expander.expandCodeFor(ExitMinusOne, IndVarTy, ICmp);

    ICmp->getOperandUse(ConstIdx).set(ExitVal);

    // Force the ScalarEvaluation to recompute the loop.
    if (ParentLoop) SE->forgetLoop(ParentLoop);
    SE->forgetLoop(L);

    BasicBlock *ExitBlock = L->getExitBlock();
    assert(ExitBlock && "Cannot get exit block for 1 BB loop and exit br!");
    // Create the new block to duplicate the bodiy.
    BasicBlock *NewBB = BasicBlock::Create(SE->getContext(), "tail.unroll",
                                           Latch->getParent(), ExitBlock);

    Latch->getTerminator()->replaceUsesOfWith(ExitBlock, NewBB);
    // Do not forget to update the incoming value of the PHIs in the exit block!
    typedef BasicBlock::iterator iterator;
    for (iterator I = ExitBlock->begin(), E = ExitBlock->getFirstNonPHI();
         I != E; ++I) {
      PHINode *PN = cast<PHINode>(I);
  
      // Replace the incoming block of the PHI, be carefule with the PHI have
      // multiple identical incoming block.
      int Idx;
      while((Idx = PN->getBasicBlockIndex(Latch)) != -1)
        PN->setIncomingBlock(Idx, NewBB);
    }

    Instruction *NewInsertPt = BranchInst::Create(ExitBlock, NewBB);
    if (ParentLoop)
      ParentLoop->addBasicBlockToLoop(NewBB, getAnalysis<LoopInfo>().getBase());

    assert(NewInsts.empty() && "NewInst not empty!");

    // Clone the loop body but do not clone the terminator.
    for (iterator I = Latch->begin(), E = Latch->getTerminator(); I != E; ++I) {
      Instruction *Inst = I;
      Value *NewVal = 0;
  
      if (PHINode *PN = dyn_cast<PHINode>(I)) {
        Value *IncomingVal = PN->DoPHITranslation(Latch, Latch);
        if (const SCEVConstant *C
            = dyn_cast<SCEVConstant>(SE->getSCEVAtScope(IncomingVal, ParentLoop)))
          NewVal = C->getValue();
        else {
          // Create the PHINode to preserve LCSSA from, because the function
          // UnrollLoop require LCSSA from.
          PHINode *LCSSAPHI = PHINode::Create(PN->getType(), 1,
                                              PN->getName() + ".lcssa",
                                              NewInsertPt);
          // Never push the PHI to the NewInst vector for simplification,
          // otherwise we will break the LCSSA from!
          // NewInsts.push_back(LCSSAPHI);
          LCSSAPHI->addIncoming(IncomingVal, Latch);
          // Use the value come from the backedge for the epilog.
          NewVal = LCSSAPHI;
        }
      } else {
        Instruction *NewInst = Inst->clone();
        NewBB->getInstList().insert(NewInsertPt, NewInst);
        NewVal = NewInst;
        NewInsts.push_back(NewInst);
        // Update the operands according to the value map.
        RemapInstruction(NewInst, VMap, RF_IgnoreMissingEntries);
      }

      // Update the value map.
      VMap[Inst] = NewVal;
      assert(Worklist.empty() && "Worklist should be empty!");

      // Update the users outside the loop.
      typedef Instruction::use_iterator use_iterator;
      for (use_iterator UI = Inst->use_begin(), UE = Inst->use_end();
           UI != UE; ++UI) {
        Instruction *UseInst = cast<Instruction>(*UI);

        if (UseInst->getParent() == Latch || UseInst->getParent() == NewBB)
          continue;

        Worklist.push_back(UseInst);
      }

      while (!Worklist.empty())
        Worklist.pop_back_val()->replaceUsesOfWith(Inst, NewVal);
    }

    // Simplify the new instructions.
    while (!NewInsts.empty()) {
      Instruction *Inst = NewInsts.pop_back_val();

      if (isInstructionTriviallyDead(Inst)) {
        Inst->eraseFromParent();
        continue;
      }

      if (Value *V = SimplifyInstruction(Inst)) {
        Inst->replaceAllUsesWith(V);
        Inst->eraseFromParent();
      }
    }
  }

  // DIRTY HACK: Recalculate the dominator tree.
  if (DominatorTree *DT = getAnalysisIfAvailable<DominatorTree>())
    DT->runOnFunction(*Latch->getParent());

  ++NumTailUnrolled;

  return true;
}
