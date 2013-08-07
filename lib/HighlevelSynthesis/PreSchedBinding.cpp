//===-- PreSchedBinding.cpp - Perform the Schedule Independent Binding ----===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface of PreSchedBinding pass. PreScheduleBinding
// perform the schedule independent binding.
//
//===----------------------------------------------------------------------===//

#include "PreSchedBinding.h"

#include "shang/Passes.h"
#include "shang/Strash.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "shang-live-variables"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct LiveInterval : public ilist_node<LiveInterval> {
  /// AliveSlots - Set of Slots at which this value is defined.  This is a bit
  /// set which uses the Slot number as an index.
  ///
  SparseBitVector<> Defs;

  /// AliveSlots - Set of Slots in which this value is alive completely
  /// through.  This is a bit set which uses the Slot number as an index.
  ///
  SparseBitVector<> Alives;

  /// Kills - Set of Slots which are the last use of this VASTSeqDef.
  ///
  SparseBitVector<> Kills;

  /// DefKill - The slot that the define is read, and the new define is
  /// available at the same time.
  ///
  SparseBitVector<> DefKills;

  std::set<VASTSeqOp*> KillOps, DefKillOps;

  typedef std::set<VASTSeqOp*>::iterator kill_iterator;
  kill_iterator kill_begin() const { return KillOps.begin(); }
  kill_iterator kill_end() const { return KillOps.end(); }

  void trimKillOps(const DenseMap<BasicBlock*, unsigned> &BBNumbers);
};

// Calculate the live interval in terms of basic block.
class BBLiveIntervals {
  DominatorTree &DT;
  iplist<LiveInterval> VarList;
  std::map<Instruction*, LiveInterval*> VarInfos;

  DenseMap<BasicBlock*, unsigned> BBNumbers;

  void buildBBNumbers(Function &F) {
    typedef Function::iterator iterator;
    unsigned Number = 0;
    for (iterator I = F.begin(), E = F.end(); I != E; ++I)
      BBNumbers[I] = ++Number;
  }

  void initializeDefSlot(Instruction *I, LiveInterval &LI);

  // Create the VarInfo for PHINodes.
  void createInstVarInfo(VASTModule &VM);
  void handleUse(Instruction *Def, VASTSeqOp *Op);
public:
  explicit BBLiveIntervals(DominatorTree &DT) : DT(DT) {}

  LiveInterval *getVarInfo(Instruction *Inst) const {
    std::map<Instruction*, LiveInterval*>::const_iterator I = VarInfos.find(Inst);
    assert(I != VarInfos.end() && "Corresponding liveInterval is not created yet?");
    return I->second;
  }

  void run(VASTModule &VM);

  void setUpInterval(PSBCompNode *N) {
    DataflowInst Inst = N->Inst;
    if (Inst.IsLauch()) {
      N->getDefs().set(BBNumbers.lookup(Inst->getParent()));
      N->getReachables().set(BBNumbers.lookup(Inst->getParent()));
      return;
    }

    LiveInterval *LI = getVarInfo(Inst);
    N->getDefs() |= LI->Defs;
    N->getDefs() |= LI->DefKills;

    N->getReachables() |= LI->Alives;
    N->getReachables() |= LI->Kills;

    N->setKillOps(LI->KillOps, LI->DefKillOps);
  }
};
}

static BasicBlock *GetParentBB(VASTSeqOp *Op) {
  VASTSlot *S = Op->getSlot();
  Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());

  BasicBlock *ParentBB = S->getParent();

  // We may need to adjust the parent slot to get the actual parent BB.
  if (Inst) {
    if (isa<PHINode>(Inst) || isa<BranchInst>(Inst) || isa<SwitchInst>(Inst)) {
      S = S->getParentGroup();
      if (BasicBlock *BB = S->getParent())
        ParentBB = BB;
    }

    assert((ParentBB == Inst->getParent() || isa<PHINode>(Inst)) &&
           "Parent not match!");
  }

  return ParentBB;
}

void LiveInterval::trimKillOps(const DenseMap<BasicBlock*, unsigned> &BBNumbers){
  std::vector<VASTSeqOp*> NotKills;

  for (kill_iterator I = kill_begin(), E = kill_end(); I != E; ++I) {
    VASTSeqOp *Op = *I;
    unsigned BBNum = BBNumbers.lookup(GetParentBB(Op));
    if (!Kills.test(BBNum))
      NotKills.push_back(Op);
  }

  while (!NotKills.empty()) {
    KillOps.erase(NotKills.back());
    NotKills.pop_back();
  }
}

void BBLiveIntervals::initializeDefSlot(Instruction *I, LiveInterval &LI) {
  if (PHINode *PN = dyn_cast<PHINode>(I)) {
    typedef PHINode::block_iterator iterator;
    for (iterator I = PN->block_begin(), E = PN->block_end(); I != E; ++I) {
      BasicBlock *BB = *I;
      LI.Defs.set(BBNumbers.lookup(BB));
      if (BB == PN->getParent())
        LI.DefKills.set(BBNumbers.lookup(BB));
      else
        LI.Kills.set(BBNumbers.lookup(BB));
    }

    return;
  }

  BasicBlock *BB = I->getParent();
  LI.Defs.set(BBNumbers.lookup(BB));
  LI.Kills.set(BBNumbers.lookup(BB));
}

void BBLiveIntervals::createInstVarInfo(VASTModule &VM) {
  // Also add the VarInfo for the static registers.
  typedef VASTModule::seqval_iterator iterator;

  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *V = I;

    if (V->isFUOutput() || V->isFUInput())
      continue;

    Instruction *Inst = dyn_cast_or_null<Instruction>(V->getLLVMValue());

    if (Inst == 0 || Inst->use_empty())
      continue;

    LiveInterval *&LI = VarInfos[Inst];
    assert(LI == 0 && "More than 1 SeqValue correspond to the same LLVM Value?");
    LI = new LiveInterval();
    VarList.push_back(LI);

    initializeDefSlot(Inst, *LI);
  }
}

void BBLiveIntervals::handleUse(Instruction *Def, VASTSeqOp *Op) {
  BasicBlock *BB = GetParentBB(Op);

  LiveInterval *LI = getVarInfo(Def);

  // Ignore the define block.
  if (Def->getParent() == BB) {
    LI->KillOps.insert(Op);
    return;
  }

  unsigned BBNum = BBNumbers.lookup(BB);
  // This variable is known alive at this slot, that means there is some even
  // later use. We do not need to do anything.
  if (LI->Alives.test(BBNum)) return;

  LI->Kills.set(BBNum);
  //assert(!VI->Defs.test(UseSlot->SlotNum));

  // The value not killed at define slots anymore.
  LI->Kills.intersectWithComplement(LI->Defs);

  if (LI->Defs.test(BBNum)) {
    LI->DefKills.set(BBNum);
    LI->DefKillOps.insert(Op);
  } else
    LI->KillOps.insert(Op);

  typedef pred_iterator ChildIt;
  std::vector<std::pair<BasicBlock*, ChildIt> > VisitStack;
  VisitStack.push_back(std::make_pair(BB, pred_begin(BB)));

  while (!VisitStack.empty()) {
    BasicBlock *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == pred_end(Node)) {
      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    BasicBlock *ChildNode = *It;
    ++VisitStack.back().second;
    unsigned ChildNodeNum = BBNumbers.lookup(ChildNode);

    // All uses of a (SSA) define should be within the dominator tree rooted on
    // the define block.
    if (!DT.dominates(Def->getParent(), ChildNode))
      continue;

    // Is the value defined in this slot?
    if (LI->Defs.test(ChildNodeNum)) {
      // The current slot is defined, but also killed!
      if (ChildNode == BB)
        LI->DefKills.set(ChildNodeNum);

      // Do not move across the define slot.
      continue;
    }

    // Reach the alive slot, no need to further visit other known AliveSlots.
    // Please note that test_and_set will return true if the bit is newly set.
    if (LI->Alives.test(ChildNodeNum)) continue;

    // We had got a loop!
    if (BB == ChildNode) continue;

    // Update the live slots.
    LI->Alives.set(ChildNodeNum);
    LI->Kills.reset(ChildNodeNum);

    VisitStack.push_back(std::make_pair(ChildNode, pred_begin(ChildNode)));
  }
}

void BBLiveIntervals::run(VASTModule &VM) {
  buildBBNumbers(VM.getLLVMFunction());

  createInstVarInfo(VM);

  std::set<VASTSeqValue*> SVs;
  typedef VASTModule::seqop_iterator seqop_iterator;
  typedef VASTOperandList::op_iterator op_iterator;
  for (seqop_iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;

    // Extract the used SeqValues.
    SVs.clear();
    for (op_iterator I = Op->op_begin(), E = Op->op_end(); I != E; ++I)
      (*I)->extractSupportingSeqVal(SVs);

    typedef std::set<VASTSeqValue*>::iterator def_iterator;
    for (def_iterator I = SVs.begin(), E = SVs.end(); I != E; ++I) {
      VASTSeqValue *V = *I;

      // Ignore the launch-latch chains.
      if (V->getLLVMValue() == Op->getValue())
        continue;

      if (Instruction *Def = dyn_cast_or_null<Instruction>(V->getLLVMValue()))
        handleUse(Def, Op);
    }
  }

  typedef iplist<LiveInterval>::iterator iterator;
  for (iterator I = VarList.begin(), E = VarList.end(); I != E; ++I)
    I->trimKillOps(BBNumbers);
}

void PSBCompNode::setKillOps(const std::set<VASTSeqOp*> &KillOps,
                             const std::set<VASTSeqOp*> &DefKillOps) {
  this->KillOps.insert(KillOps.begin(), KillOps.end());
  this->KillOps.insert(DefKillOps.begin(), DefKillOps.end());
}



bool PSBCompNode::isSingleBlock() const {
  return getDefs().count() == 1 && getDefs() == getReachables();
}

template<typename T>
static bool intersect(const std::set<T*> &LHS, const std::set<T*> &RHS) {
  typedef typename std::set<T*>::const_iterator iterator;
  iterator I = LHS.begin(), IE = LHS.end(), J = RHS.begin(), JE = RHS.end();

  while (I != IE && J != JE) {
    if (*I < *J)
      ++I;
    else if (*J < *I)
      ++J;
    else
      return true;
  }

  return false;
}

static const float fanin_factor = 1.0f,
                   fanout_factor = 0.1f,
                   area_factor = 0.6f,
                   consistent_factor = 1.0f,
                   scheduling_factor = 0.1f;

bool PSBCompNode::isKillIntersect(const PSBCompNode *RHS) const {
  return intersect(KillOps, RHS->KillOps);
}

void PSBCompNode::increaseCost(PSBCompNode *Succ, float Cost) {
  setCost(Succ, Cost + getCostTo(Succ));
}

bool PSBCompNode::isCompatibleWith(const CompGraphNode *RHS) const {
  if (!isCompatibleWithStructural(RHS))
    return false;

  if (isKillIntersect(static_cast<const PSBCompNode*>(RHS)))
    return false;

  if (!isCompatibleWithInterval(RHS)) {
    // For instructions from different BB, their will never become compatible
    // if they BB live interval overlap.
    if (getDomBlock() != RHS->getDomBlock())
      return false;

    // If the live interval is not dead (there is live-in blocks), their will
    // never become compatible if they BB live interval overlap.
    if (!isSingleBlock() || !static_cast<const PSBCompNode*>(RHS)->isSingleBlock())
      return false;
  }

  return true;
}

namespace {
class PSBCompGraph : public CompGraphBase {
public:
  PSBCompGraph(DominatorTree &DT, CachedStrashTable &CST)
    : CompGraphBase(DT, CST) {}

  CompGraphNode *createNode(VFUs::FUTypes FUType, unsigned FUCost, unsigned Idx,
    DataflowInst Inst, ArrayRef<VASTSelector*> Sels)
    const {
      return new PSBCompNode(FUType, FUCost, Idx, Inst, Sels);
  }

  float compuateCommonFIBenefit(VASTSelector *Sel) const;

  float getEdgeConsistencyBenefit(EdgeType Edge, EdgeType FIEdge) const;

  float computeCost(CompGraphNode *Src, CompGraphNode *Dst) const;
};
}

float PSBCompGraph::computeCost(CompGraphNode *Src, CompGraphNode *Dst) const {
  float Cost = 0.0f;
  float current_area_factor = area_factor;

  // Increase the area benefit for operations that are not located in the same
  // BB, in this case, binding are almost always possible.
  if (VFUs::isNoTrivialFUCompatible(Src->FUType, Dst->FUType) &&
      Src->getDomBlock() == Dst->getDomBlock())
    current_area_factor *= 1.1f;

  // 1. Calculate the saved resource by binding src and dst to the same FU/Reg.
  Cost -= current_area_factor * compuateSavedResource(Src, Dst);

  // 2. Calculate the interconnection cost.
  Cost += fanin_factor * computeIncreasedMuxPorts(Src, Dst);

  // 3. Timing penalty introduced by MUX
  //
  return Cost;
}

float PSBCompGraph::getEdgeConsistencyBenefit(EdgeType Edge,
                                              EdgeType FIEdge) const {
  CompGraphNode *FISrc = FIEdge.first, *FIDst = FIEdge.second;

  if (VFUs::isFUCompatible(FISrc->FUType, FIDst->FUType)) {
    float cost = std::min(FISrc->FUCost, FIDst->FUCost) * consistent_factor;
    // Add interconnection benefit for trivial FU.
    cost = std::max<float>(cost, fanin_factor);
    return cost;
  }

  return 0.0f;
}

float PSBCompGraph::compuateCommonFIBenefit(VASTSelector *Sel) const {
  float Benefit
    = Sel->getBitWidth() * Sel->numNonTrivialFanins() * fanout_factor;

  return Benefit;
}

void PreSchedBinding::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);

  AU.addRequired<DominatorTree>();
  AU.addRequiredID(ControlLogicSynthesisID);
  AU.addRequired<CachedStrashTable>();
  AU.setPreservesAll();
}

void PreSchedBinding::releaseMemory() {
  if (PSBCG) {
    delete PSBCG;
    PSBCG = 0;
  }
}

bool PreSchedBinding::runOnVASTModule(VASTModule &VM) {
  DominatorTree &DT = getAnalysis<DominatorTree>();
  CachedStrashTable &CST = getAnalysis<CachedStrashTable>();
  
  BBLiveIntervals BBLI(DT);
  BBLI.run(VM);

  PSBCG = new PSBCompGraph(DT, CST);

  typedef VASTModule::seqop_iterator iterator;
  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;

    VASTSeqInst *Inst = dyn_cast<VASTSeqInst>(Op);

    if (Inst == 0 || !Inst->isBindingCandidate()) {
      for (unsigned i = 0; i < Op->num_srcs(); ++i)
        PSBCG->addBoundSels(Op->getSrc(i).getSelector());
      continue;
    }

    CompGraphNode *N = PSBCG->addNewNode(Inst);

    if (N->isIntervalEmpty())
      BBLI.setUpInterval(static_cast<PSBCompNode*>(N));

    N->updateOrder(Inst->getSlot()->SlotNum);
  }

  // Do not decompose trivial nodes.
  // PSBCG->decomposeTrivialNodes();
  PSBCG->computeCompatibility();
  PSBCG->fixTransitive();

  PSBCG->compuateEdgeCosts();
  PSBCG->setCommonFIBenefit();

  PSBCG->performBinding();

  return false;
}

PreSchedBinding::PreSchedBinding() : VASTModulePass(ID) {
  initializePreSchedBindingPass(*PassRegistry::getPassRegistry());
}

char PreSchedBinding::ID = 0;

char &llvm::PreSchedBindingID = PreSchedBinding::ID;

INITIALIZE_PASS_BEGIN(PreSchedBinding, "shang-pre-schedule-binding",
                      "Schedule Independent Binding", false, true)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable)
INITIALIZE_PASS_END(PreSchedBinding, "shang-pre-schedule-binding",
                    "Schedule Independent Binding", false, true)
