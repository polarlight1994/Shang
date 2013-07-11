//===----------- TimingScriptGen.cpp- Generate the Timing Scripts - C++ ---===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass analysis the slack between two registers.
//
// The "Slack" in VAST means the extra cycles that after data appear in
// the output pin of the src register before the dst register read the data.
// i.e. if we assign reg0 at cycle 1, and the data will appear at the output
// pin of reg0 at cycle 2, and now reg1 can read the data. In this case
// becasue the data appear at cycle 2 and we read the data at the same cycle,
// the slack is 0. But if we read the data at cycle 3, the slack is 1.
//
//===----------------------------------------------------------------------===//
#include "SeqLiveVariables.h"
#include "STGDistances.h"
#include "TimingNetlist.h"

#include "shang/VASTSeqValue.h"
#include "shang/VASTModulePass.h"
#include "shang/VASTSubModules.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"
#include "shang/Utilities.h"

#include "llvm/Pass.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormattedStream.h"
#define DEBUG_TYPE "shang-timing-script"
#include "llvm/Support/Debug.h"
using namespace llvm;

static cl::opt<bool>
DisableTimingScriptGeneration("shang-disable-timing-script",
                              cl::desc("Disable timing script generation"),
                              cl::init(false));

STATISTIC(NumMultiCyclesConstraints, "Number of multicycles timing constraints "
                                     "generated");
STATISTIC(NumFalseTimingPath,
          "Number of false timing paths detected (From->To pair)");
STATISTIC(NumRequiredConstraints, "Number of required timing constraints generated");
STATISTIC(NumConstraints, "Number of timing constraints generated");
STATISTIC(NumTimgViolation, "Number of timing paths with negative slack");

namespace{
struct TimingScriptGen;

/// AnnotatedCone - Combinational cone annotated with timing information.
struct AnnotatedCone {
  TimingNetlist &TNL;
  SeqLiveVariables &SLV;
  VASTSelector *Dst;
  raw_ostream &OS;
  const uint32_t Inf;
  static unsigned ConstrantCounter;

  struct Leaf {
    unsigned NumCycles;
    float CriticalDelay;
    explicit Leaf(unsigned NumCycles = STGDistances::Inf,
                     float CriticalDelay = 0.0f)
      : NumCycles(NumCycles), CriticalDelay(CriticalDelay) {}

    // Update the source information with tighter cycles constraint and larger
    // critical delay
    void update(unsigned NumCycles, float CriticalDelay) {
      this->NumCycles = std::min(this->NumCycles, NumCycles);
      this->CriticalDelay = std::max(this->CriticalDelay, CriticalDelay);
    }

    void update(const Leaf &RHS) {
      update(RHS.NumCycles, RHS.CriticalDelay);
    }
  };

  typedef DenseMap<VASTSeqValue*, Leaf> SeqValSetTy;
  SeqValSetTy CyclesFromSrcLB;

  typedef DenseMap<VASTExpr*, SeqValSetTy> QueryCacheTy;
  QueryCacheTy QueryCache;

  AnnotatedCone(TimingNetlist &TNL, SeqLiveVariables &SLV,
                         VASTSelector *Dst, raw_ostream &OS)
    : TNL(TNL), SLV(SLV), Dst(Dst), OS(OS), Inf(STGDistances::Inf)
  {}

  void reset() {
    QueryCache.clear();
    CyclesFromSrcLB.clear();
  }

  void addIntervalFromSrc(VASTSeqValue *Src, const Leaf &L) {
    assert(L.NumCycles && "unexpected zero interval!");
    assert((!Src->isSlot() || L.NumCycles <= 1)
           && "Bad interval for slot registers!");
    CyclesFromSrcLB[Src].update(L);
  }

  bool generateSubmoduleConstraints(VASTSeqValue *SeqVal);

  void annotatePathInterval(VASTValue *Tree, ArrayRef<VASTSlot*> ReadSlots);

  Leaf buildLeaf(VASTSeqValue *V, ArrayRef<VASTSlot*> ReadSlots, VASTValue *Thu);
  void annotateLeaf(VASTSeqValue *V, VASTExpr *Parent, Leaf CurLeaf,
                    SeqValSetTy &LocalInterval);

  unsigned getMinimalInterval(VASTSeqValue *Src, VASTSlot *ReadSlot);
  unsigned getMinimalInterval(VASTSeqValue *Src, ArrayRef<VASTSlot*> ReadSlots);

  // Propagate the timing information of the current combinational cone.
  void propagateInterval(VASTExpr *Expr, const SeqValSetTy &LocalIntervalMap) {
    typedef VASTOperandList::op_iterator iterator;
    SeqValSetTy &CurSet = QueryCache[Expr];
    for (iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      VASTExpr *SubExpr = dyn_cast<VASTExpr>(VASTValPtr(*I).get());
      // Do not look up the source information across the keep boundary.
      if (SubExpr == 0 || SubExpr->getOpcode() == VASTExpr::dpKeep)
        continue;

      QueryCacheTy::const_iterator at = QueryCache.find(SubExpr);
      if (at == QueryCache.end()) continue;

      const SeqValSetTy &Srcs = at->second;
      typedef SeqValSetTy::const_iterator src_iterator;
      for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
        assert(I->second.NumCycles && "Unexpected zero delay!");

        Leaf &SI = CurSet[I->first];
        // Look up the timing information from the map of current cone.
        SeqValSetTy::const_iterator at = LocalIntervalMap.find(I->first);
        assert(at != LocalIntervalMap.end() && "Node not visited yet?");
        SI.update(at->second);
      }
    }
  }

  void generateMCPEntries() const;
  // Generate the constraints in depth-first order. So that we always cover the
  // whole cone by the constraints on the root, and refine them by the
  // constraints on the leaves.
  void generateMCPEntriesDFOrder(VASTValue *Root) const;

  bool generateMCPThough(VASTExpr *Expr) const;

  // Bind multi-cycle path constraints to the scripting engine.
  template<typename SrcTy>
  void generateMCPWithInterval(SrcTy *Src, const std::string &ThuName,
                             const Leaf &SI, unsigned Order) const;
  unsigned generateMCPThough(VASTExpr *Thu, const SeqValSetTy &SrcSet) const;

  typedef SeqValSetTy::const_iterator src_iterator;
  src_iterator src_begin() const { return CyclesFromSrcLB.begin(); }
  src_iterator src_end() const { return CyclesFromSrcLB.end(); }

  void dump() const;
};

struct TimingScriptGen : public VASTModulePass {
  formatted_raw_ostream OS;

  VASTModule *VM;
  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<SeqLiveVariables>();
    AU.addRequired<STGDistances>();
    AU.addRequired<TimingNetlist>();
    AU.addRequiredID(DatapathNamerID);
    AU.addRequired<DataLayout>();
    AU.setPreservesAll();
  }

  void writeConstraintsFor(VASTSelector *Dst, TimingNetlist &TNL,
                           SeqLiveVariables &SLV);

  void extractTimingPaths(AnnotatedCone &Cache,
                          ArrayRef<VASTSlot*> ReadSlots,
                          VASTValue *DepTree);

  bool runOnVASTModule(VASTModule &VM);

  TimingScriptGen() : VASTModulePass(ID), OS(), VM(0) {
    initializeTimingScriptGenPass(*PassRegistry::getPassRegistry());
  }
};
}

unsigned AnnotatedCone::getMinimalInterval(VASTSeqValue *Src,
                                           VASTSlot *ReadSlot) {
  // Try to get the live variable at (Src, ReadSlot), calculate its distance
  // from its defining slots to the read slot.
  return SLV.getIntervalFromDef(Src, ReadSlot);
}

unsigned AnnotatedCone::getMinimalInterval(VASTSeqValue *Src,
                                           ArrayRef<VASTSlot*> ReadSlots) {
  unsigned PathInterval = Inf;
  typedef ArrayRef<VASTSlot*>::iterator iterator;
  for (iterator I = ReadSlots.begin(), E = ReadSlots.end(); I != E; ++I)
    PathInterval = std::min(PathInterval, getMinimalInterval(Src, *I));

  return PathInterval;
}


AnnotatedCone::Leaf AnnotatedCone::buildLeaf(VASTSeqValue *V,
                                             ArrayRef<VASTSlot*> ReadSlots,
                                             VASTValue *Thu) {
  unsigned Interval = getMinimalInterval(V, ReadSlots);
  float EstimatedDelay = TNL.getDelay(V, Thu, Dst);
  return Leaf(Interval, EstimatedDelay);
}

void AnnotatedCone::annotateLeaf(VASTSeqValue *V, VASTExpr *Parent, Leaf CurLeaf,
                                 SeqValSetTy &LocalInterval) {
  LocalInterval[V].update(CurLeaf);
  QueryCache[Parent][V].update(CurLeaf);
  // Add the information to statistics.
  addIntervalFromSrc(V, CurLeaf);
}

void AnnotatedCone::annotatePathInterval(VASTValue *Root,
                                         ArrayRef<VASTSlot*> ReadSlots) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(Root);

  if (Expr == 0) return;

  typedef  VASTOperandList::op_iterator ChildIt;

  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;
  // Remember the visited node for the current cone.
  std::set<VASTValue*> Visited;
  // Remember the number of cycles from the reachable register to the read slot
  // for the current cone.
  SeqValSetTy LocalInterval;

  VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));
  while (!VisitStack.empty()) {
    VASTExpr *Expr = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // All sources of this node is visited.
    if (It == Expr->op_end()) {
      VisitStack.pop_back();
      // Propagate the interval information from the operands of the current
      // value.
      propagateInterval(Expr, LocalInterval);
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (ChildNode == 0) continue;

    // And do not visit a node twice.
    if (!Visited.insert(ChildNode).second) continue;

    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(ChildNode)) {
      if (generateSubmoduleConstraints(V)) continue;

      annotateLeaf(V, Expr, buildLeaf(V, ReadSlots, Root), LocalInterval);
      continue;
    }  

    VASTExpr *SubExpr = dyn_cast<VASTExpr>(ChildNode);

    if (SubExpr == 0)
      continue;

    // Do not move across the keep nodes.
    if (SubExpr->getOpcode() != VASTExpr::dpKeep) {
      VisitStack.push_back(std::make_pair(SubExpr, SubExpr->op_begin()));
      continue;
    }

    typedef std::set<VASTSeqValue*> SVSet;
    SVSet Srcs;
    SubExpr->extractSupportingSeqVal(Srcs);
    assert(!Srcs.empty() && "Unexpected trivial cone with keep attribute!");
    for (SVSet::iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *V = *I;

      if (generateSubmoduleConstraints(V)) continue;

      // Add the information to statistics. It is ok to add the interval here
      // even V may be masked by the false path indicated by the keep attribute.
      // we will first generate the tight constraints and overwrite them by
      // looser constraints.
      annotateLeaf(V, SubExpr, buildLeaf(V, ReadSlots, Root), LocalInterval);
    }
  }

  // Check the result, debug only.
  DEBUG(QueryCacheTy::iterator at = QueryCache.find(Expr);
  assert(at != QueryCache.end()
         && "Timing path information for root not found!");
  const SeqValSetTy &RootSet = at->second;
  typedef SeqValSetTy::const_iterator it;
  bool IntervalMasked = false;
  for (it I = LocalInterval.begin(), E = LocalInterval.end(); I != E; ++I) {
    SeqValSetTy::const_iterator ActualIntervalAt = RootSet.find(I->first);
    assert(ActualIntervalAt != RootSet.end() && "Timing path entire missed!");
    assert(ActualIntervalAt->second.NumCycles <= I->second.NumCycles
           && "Interval information not applied?");
    if (ActualIntervalAt->second.NumCycles == I->second.NumCycles) continue;

    dbgs() << "Timing path masked: Root is";
    Root->printAsOperand(dbgs(), false);
    dbgs() << " end node is " << I->first->getName()
           << " masked delay: " << I->second.NumCycles
           << " actual delay: " << ActualIntervalAt->second.NumCycles << '\n';
    IntervalMasked = true;
  }

  if (IntervalMasked) {
    dbgs() << " going to dump the nodes in the tree:\n";

    typedef std::set<VASTValue*>::iterator node_it;
    for (node_it NI = Visited.begin(), NE = Visited.end(); NI != NE; ++NI) {
      (*NI)->printAsOperand(dbgs(), false);
      dbgs() << ", ";
    }
    dbgs() << '\n';
  });
}

void AnnotatedCone::dump() const {
  dbgs() << "\nCurrent data-path timing:\n";
  typedef QueryCacheTy::const_iterator it;
  for (it I = QueryCache.begin(), E = QueryCache.end(); I != E; ++I) {
    const SeqValSetTy &Set = I->second;
    typedef SeqValSetTy::const_iterator reg_it;

    //if (!printBindingLuaCode(dbgs(), I->first))
    //  continue;

    dbgs() << "\n\t{ ";
    for (reg_it RI = Set.begin(), RE = Set.end(); RI != RE; ++RI) {
      dbgs() << '(';
      //printBindingLuaCode(dbgs(), RI->first);
      dbgs() << '#' << RI->second.NumCycles << "), ";
    }

    dbgs() << "}\n";
  }
}

unsigned AnnotatedCone::ConstrantCounter = 0;

// The first node of the path is the use node and the last node of the path is
// the define node.
template<typename SrcTy>
void AnnotatedCone::generateMCPWithInterval(SrcTy *Src, const std::string &ThuName,
                                          const Leaf &SI, unsigned Order) const {
  assert(!ThuName.empty() && "Bad through node name!");
  OS << "INSERT INTO mcps(src, dst, thu, cycles, normalized_delay, constraint_order)"
        "VALUES(\n"
     << '\'' << Src->getSTAObjectName() << "', \n"
     << '\'' << Dst->getSTAObjectName() << "', \n"
     << '\'' << ThuName << "', \n"
     << SI.NumCycles << ", \n"
     << SI.CriticalDelay << ", \n"
     << Order << ");\n";

  // Perform the Statistic.
  if (SI.NumCycles > 1) {
    ++NumMultiCyclesConstraints;
    if (SI.CriticalDelay > 1.0f) ++NumRequiredConstraints;
  }
  if (SI.NumCycles == AnnotatedCone::Inf) ++NumFalseTimingPath;
  if (SI.NumCycles < SI.CriticalDelay) ++NumTimgViolation;
}

unsigned
AnnotatedCone::generateMCPThough(VASTExpr *Thu, const SeqValSetTy &SrcSet) const {
  std::string ThuName = "shang-null-node";
  if (Thu) {
    // Do not generate constraints for anonymous nodes.
    if (Thu->isAnonymous()) return 0;

    ThuName = Thu->getSTAObjectName();

    if (ThuName.empty()) return 0;
  }

  typedef SeqValSetTy::const_iterator iterator;
  for (iterator I = SrcSet.begin(), E = SrcSet.end(); I != E; ++I)
    generateMCPWithInterval(I->first, ThuName, I->second, ++ConstrantCounter);

  return SrcSet.size();
}

bool AnnotatedCone::generateMCPThough(VASTExpr *Expr) const {
  // Visit the node before we pushing it into the stack.
  QueryCacheTy::const_iterator I = QueryCache.find(Expr);
  // The children of the current node will not have any annotation if the
  // current node do not have any annotation.
  if (I == QueryCache.end())
    return false;

  NumConstraints += generateMCPThough(Expr, I->second);
  return true;
}

void AnnotatedCone::generateMCPEntriesDFOrder(VASTValue *Root) const {
  VASTExpr *RootExpr = dyn_cast<VASTExpr>(Root);

  // Trivial cone that only consists of 1 register is not handle here.
  if (RootExpr == 0) return;

  typedef  VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;
  // Remember the visited node for the current cone.
  std::set<VASTValue*> Visited;
  generateMCPThough(RootExpr);

  VisitStack.push_back(std::make_pair(RootExpr, RootExpr->op_begin()));
  while (!VisitStack.empty()) {
    VASTExpr *Expr = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    if (It == Expr->op_end()) {
      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (ChildNode == 0) continue;

    // And do not visit a node twice.
    if (!Visited.insert(ChildNode).second) continue;

    if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(ChildNode)) {
      if (generateMCPThough(SubExpr))
        VisitStack.push_back(std::make_pair(SubExpr, SubExpr->op_begin()));
    }
  }
}

void AnnotatedCone::generateMCPEntries() const {
  DEBUG(dbgs() << "Going to bind delay information of graph: \n");
  DEBUG(dump());
  DEBUG(dbgs() << "Binding path for dst register: "
               << Dst->getName() << '\n');

  generateMCPThough(0, CyclesFromSrcLB);
  NumConstraints += CyclesFromSrcLB.size();

  generateMCPEntriesDFOrder(Dst->getGuard().get());
  generateMCPEntriesDFOrder(Dst->getFanin().get());
}

bool AnnotatedCone::generateSubmoduleConstraints(VASTSeqValue *SeqVal) {
  if (!SeqVal->isFUOutput()) return false;

  VASTSubModule *SubMod = dyn_cast<VASTSubModule>(SeqVal->getParent());
  if (SubMod == 0) return true;

  unsigned Latency = SubMod->getLatency();
  // No latency information available.
  if (Latency == 0) return true;

  // Add the timing constraints from operand registers to the output registers.
  typedef VASTSubModule::fanin_iterator fanin_iterator;
  for (fanin_iterator I = SubMod->fanin_begin(), E = SubMod->fanin_end();
       I != E; ++I) {
    VASTSelector *Operand = *I;
    Leaf CurLeaf = Leaf(Latency, float(Latency));
    generateMCPWithInterval(Operand, "shang-null-node", CurLeaf, ++ConstrantCounter);
  }

  return true;
}

bool TimingScriptGen::runOnVASTModule(VASTModule &VM)  {
  // No need to write timing script at all.
  if (DisableTimingScriptGeneration) return false;

  std::string MCPDataBasePath = getStrValueFromEngine("MCPDataBase");
  std::string Error;
  raw_fd_ostream Output(MCPDataBasePath.c_str(), Error);
  OS.setStream(Output);

  OS << "CREATE TABLE mcps( \
          id INTEGER PRIMARY KEY AUTOINCREMENT, \
          src TEXT, \
          dst TEXT, \
          thu TEXT, \
          cycles INTEGER, \
          normalized_delay REAL, \
          constraint_order INTEGER \
          );\n";

  bindFunctionToScriptEngine(getAnalysis<DataLayout>(), &VM);

  SeqLiveVariables &SLV = getAnalysis<SeqLiveVariables>();
  TimingNetlist &TNL =getAnalysis<TimingNetlist>();

  //Write the timing constraints.
  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;

    if (Sel->empty()) continue;

    writeConstraintsFor(Sel, TNL, SLV);
  }

  OS.flush();

  OS.setStream(nulls());
  return false;
}

void
TimingScriptGen::writeConstraintsFor(VASTSelector *Dst, TimingNetlist &TNL,
                                     SeqLiveVariables &SLV) {
  DenseMap<VASTValue*, SmallVector<VASTSlot*, 8> > DatapathMap;
  VASTValue *FI = Dst->getFanin().get(), *Guard = Dst->getGuard().get();

  typedef VASTSelector::ann_iterator ann_iterator;
  for (ann_iterator I = Dst->ann_begin(), E = Dst->ann_end(); I != E; ++I) {
    VASTSelector::Annotation *Ann = *I;
    VASTSlot *S = &Ann->S;
    DatapathMap[Ann->getNode()].push_back(S);
    // Also annotate the slot to FI and Guard, otherwise we may miss some path
    // not block by the keeped nodes.
    DatapathMap[FI].push_back(S);
    DatapathMap[Guard].push_back(S);
  }

  AnnotatedCone Cache(TNL, SLV, Dst, OS);
  typedef DenseMap<VASTValue*, SmallVector<VASTSlot*, 8> >::iterator it;
  for (it I = DatapathMap.begin(), E = DatapathMap.end(); I != E; ++I)
    extractTimingPaths(Cache, I->second, I->first);

  Cache.generateMCPEntries();
}

void TimingScriptGen::extractTimingPaths(AnnotatedCone &Cache,
                                         ArrayRef<VASTSlot*> ReadSlots,
                                         VASTValue *DepTree) {
  // Trivial case: register to register path.
  if (VASTSeqValue *Src = dyn_cast<VASTSeqValue>(DepTree)){
    if (Cache.generateSubmoduleConstraints(Src)) return;

    unsigned Interval = Cache.getMinimalInterval(Src, ReadSlots);
    Cache.addIntervalFromSrc(Src, AnnotatedCone::Leaf(Interval, 0.0f));

    // Even a trivial path can be a false path, e.g.:
    // slot 1:
    // reg_a <= c + x;
    // slot 2:
    // reg_a <= reg_b
    // For DstAVS = reg_a@1, there are no timing path from reg_b.
    return;
  }

  // If Define Value is immediate or symbol, skip it.
  if (!isa<VASTWire>(DepTree) && !isa<VASTExpr>(DepTree)) return;

  Cache.annotatePathInterval(DepTree, ReadSlots);
}

char TimingScriptGen::ID = 0;

INITIALIZE_PASS_BEGIN(TimingScriptGen, "vast-timing-script-generation",
                      "Generate timing script to export the behavior-level timing",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
INITIALIZE_PASS_END(TimingScriptGen, "vast-timing-script-generation",
                    "Generate timing script to export the behavior-level timing",
                    false, true)

Pass *llvm::createTimingScriptGenPass() {
  return new TimingScriptGen();
}
