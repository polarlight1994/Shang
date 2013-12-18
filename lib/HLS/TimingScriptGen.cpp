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
#include "TimingNetlist.h"

#include "vast/VASTSeqValue.h"
#include "vast/STGDistances.h"
#include "vast/VASTModulePass.h"
#include "vast/VASTMemoryBank.h"
#include "vast/VASTSubModules.h"
#include "vast/VASTModule.h"
#include "vast/Passes.h"
#include "vast/Utilities.h"
#include "vast/LuaI.h"

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
static cl::opt<bool>
KeepNodesOnly("shang-timing-script-keep-only",
              cl::desc("Only generate timing script thu 'keep' nodes"),
              cl::init(true));

STATISTIC(NumMultiCyclesConstraints, "Number of multicycles timing constraints "
                                     "generated");
STATISTIC(NumFalseTimingPath,
          "Number of false timing paths detected (From->To pair)");
STATISTIC(NumRequiredConstraints, "Number of required timing constraints generated");
STATISTIC(NumConstraints, "Number of timing constraints generated");
STATISTIC(NumTimgViolation, "Number of timing paths with negative slack");

namespace{
struct TimingScriptGen;
struct Leaf {
  unsigned NumCycles;
  float CriticalDelay;
  Leaf(unsigned NumCycles = STGDistances::Inf, float CriticalDelay = 0.0f)
    : NumCycles(NumCycles), CriticalDelay(CriticalDelay) {}

  // Update the source information with tighter cycles constraint and larger
  // critical delay
  void update(unsigned NumCycles, float CriticalDelay) {
    this->NumCycles = std::min(this->NumCycles, NumCycles);
    this->CriticalDelay = std::max(this->CriticalDelay, CriticalDelay);
  }

  Leaf &update(const Leaf &RHS) {
    update(RHS.NumCycles, RHS.CriticalDelay);
    return *this;
  }
};

/// AnnotatedCone - Combinational cone annotated with timing information.
struct AnnotatedCone {
  TimingNetlist &TNL;
  STGDistances &STGDist;
  VASTNode *Node;
  raw_ostream &OS;
  const uint32_t Inf;
  static unsigned ConstrantCounter;

  typedef DenseMap<VASTSelector*, Leaf> SeqValSetTy;
  SeqValSetTy CyclesFromSrcLB;

  typedef DenseMap<VASTExpr*, SeqValSetTy> QueryCacheTy;
  QueryCacheTy QueryCache;
  SmallVector<VASTSelector*, 4> Sels;

  AnnotatedCone(TimingNetlist &TNL, STGDistances &STGDist, VASTNode *Node,
                raw_ostream &OS)
    : TNL(TNL), STGDist(STGDist), Node(Node), OS(OS), Inf(STGDistances::Inf)
  {}

  void addSelector(VASTSelector *Sel) {
    Sels.push_back(Sel);
  }

  void reset() {
    QueryCache.clear();
    CyclesFromSrcLB.clear();
    Sels.clear();
  }

  static void checkIntervalFromSlot(VASTSelector *Sel, unsigned Cycles) {
    assert((!Sel->isSlot() || Cycles <= 1) && "Bad interval for slot registers!");
    (void) Cycles;
    (void) Sel;
  }

  void addIntervalFromSrc(VASTSelector *Sel, const Leaf &L) {
    assert(L.NumCycles && "unexpected zero interval!");
    unsigned Cycles = CyclesFromSrcLB[Sel].update(L).NumCycles;
    checkIntervalFromSlot(Sel, Cycles);
  }

  bool generateSubmoduleConstraints(VASTSeqValue *SeqVal);

  void annotatePathInterval(VASTValue *Tree, VASTSelector *Dst,
                            ArrayRef<VASTSlot*> ReadSlots);

  Leaf buildLeaf(VASTSeqValue *V, ArrayRef<VASTSlot*> ReadSlots, VASTValue *Thu,
                 VASTSelector *Dst);
  void annotateLeaf(VASTSelector *Sel, VASTExpr *Parent, Leaf CurLeaf,
                    SeqValSetTy &LocalInterval);

  // Propagate the timing information of the current combinational cone.
  void propagateInterval(VASTExpr *Expr, const SeqValSetTy &LocalIntervalMap) {
    typedef VASTOperandList::op_iterator iterator;
    SeqValSetTy &CurSet = QueryCache[Expr];
    for (iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      VASTExpr *SubExpr = dyn_cast<VASTExpr>(VASTValPtr(*I).get());
      if (SubExpr == 0)
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

  void generateMCPEntries(VASTSelector *Dst) const;
  void generateMCPEntries();
  // Generate the constraints in depth-first order. So that we always cover the
  // whole cone by the constraints on the root, and refine them by the
  // constraints on the leaves.
  void generateMCPEntriesDFOrder(VASTValue *Root) const;

  void generateMCPThough(VASTExpr *Expr) const;

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
    AU.addRequired<STGDistances>();
    AU.addRequired<TimingNetlist>();
    AU.addRequiredID(DatapathNamerID);
    AU.addRequired<DataLayout>();
    AU.setPreservesAll();
  }

  void writeConstraintsFor(VASTSelector *Sel, TimingNetlist &TNL,
                           STGDistances &STGDist);
  void annoataConstraintsFor(AnnotatedCone &Cache, VASTSelector *Sel);

  void extractTimingPaths(AnnotatedCone &Cache, VASTSelector *Dst,
                          ArrayRef<VASTSlot*> ReadSlots,
                          VASTValue *DepTree);

  bool runOnVASTModule(VASTModule &VM);

  TimingScriptGen() : VASTModulePass(ID), OS(), VM(0) {
    initializeTimingScriptGenPass(*PassRegistry::getPassRegistry());
  }
};
}

Leaf AnnotatedCone::buildLeaf(VASTSeqValue *V, ArrayRef<VASTSlot*> ReadSlots,
                              VASTValue *Thu, VASTSelector *Dst) {
  unsigned Interval = STGDist.getIntervalFromDef(V->getSelector(), ReadSlots);
  float EstimatedDelay = TNL.getDelay(V, Thu, Dst);
  return Leaf(Interval, EstimatedDelay);
}

void
AnnotatedCone::annotateLeaf(VASTSelector *Sel, VASTExpr *Parent, Leaf CurLeaf,
                            SeqValSetTy &LocalInterval) {
  LocalInterval[Sel].update(CurLeaf);
  QueryCache[Parent][Sel].update(CurLeaf);
  // Add the information to statistics.
  addIntervalFromSrc(Sel, CurLeaf);
}

void AnnotatedCone::annotatePathInterval(VASTValue *Root, VASTSelector *Dst,
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

      annotateLeaf(V->getSelector(), Expr, buildLeaf(V, ReadSlots, Root, Dst),
                   LocalInterval);
      continue;
    }  

    VASTExpr *SubExpr = dyn_cast<VASTExpr>(ChildNode);

    if (SubExpr == 0)
      continue;

    // Do not move across the keep nodes.
    if (!SubExpr->isTimingBarrier()) {
      VisitStack.push_back(std::make_pair(SubExpr, SubExpr->op_begin()));
      continue;
    }

    typedef std::set<VASTSeqValue*> SVSet;
    SVSet Srcs;
    // Get *all* source register of the cone rooted on SubExpr.
    SubExpr->extractSupportingSeqVal(Srcs, false /*Search across keep nodes!*/);
    assert(!Srcs.empty() && "Unexpected trivial cone with keep attribute!");
    for (SVSet::iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *V = *I;

      if (generateSubmoduleConstraints(V)) continue;

      // Add the information to statistics. It is ok to add the interval here
      // even V may be masked by the false path indicated by the keep attribute.
      // we will first generate the tight constraints and overwrite them by
      // looser constraints.
      annotateLeaf(V->getSelector(), Expr, buildLeaf(V, ReadSlots, Root, Dst),
                   LocalInterval);
    }
  }
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
     << '\'' << Node->getSTAObjectName() << "', \n"
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

    if (KeepNodesOnly && !Thu->isTimingBarrier()) return 0;
  }

  typedef SeqValSetTy::const_iterator iterator;
  for (iterator I = SrcSet.begin(), E = SrcSet.end(); I != E; ++I)
    generateMCPWithInterval(I->first, ThuName, I->second, ++ConstrantCounter);

  return SrcSet.size();
}

void AnnotatedCone::generateMCPThough(VASTExpr *Expr) const {
  // Visit the node before we pushing it into the stack.
  QueryCacheTy::const_iterator I = QueryCache.find(Expr);

  // Write the annotation if there is any.
  if (I != QueryCache.end())
    NumConstraints += generateMCPThough(Expr, I->second);
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
      generateMCPThough(SubExpr);

      VisitStack.push_back(std::make_pair(SubExpr, SubExpr->op_begin()));
    }
  }
}

void AnnotatedCone::generateMCPEntries(VASTSelector *Dst) const {
  generateMCPThough(0, CyclesFromSrcLB);
  NumConstraints += CyclesFromSrcLB.size();

  generateMCPEntriesDFOrder(Dst->getGuard().get());
  generateMCPEntriesDFOrder(Dst->getFanin().get());
}

void AnnotatedCone::generateMCPEntries() {
  while (!Sels.empty())
    generateMCPEntries(Sels.pop_back_val());
}

bool AnnotatedCone::generateSubmoduleConstraints(VASTSeqValue *SeqVal) {
  if (!SeqVal->isFUOutput()) return false;

  VASTSubModule *SubMod = dyn_cast<VASTSubModule>(SeqVal->getParent());
  if (SubMod == 0) return false;

  unsigned Latency = SubMod->getLatency();
  // No latency information available.
  if (Latency == 0) return false;

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

  std::string MCPDataBasePath = LuaI::GetString("MCPDataBase");
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

  STGDistances &STGDist = getAnalysis<STGDistances>();
  TimingNetlist &TNL =getAnalysis<TimingNetlist>();

  //Write the timing constraints.
  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;

    if (Sel->empty()) continue;

    // Handle the trivial case.
    writeConstraintsFor(Sel, TNL, STGDist);
  }

  // Also generate the location constraints.
  OS << "CREATE TABLE locations( \
        id INTEGER PRIMARY KEY AUTOINCREMENT, \
        node TEXT, x INTEGER, y INTEGER, width INTEGER, height INTEGER);\n";
  if (VM.hasBoundingBoxConstraint()) {
    OS << "INSERT INTO locations(node, x, y, width, height) VALUES(\""
       << VM.getName() << "\", "
       << VM.getBBX() << ", " << VM.getBBY() << ", "
       << VM.getBBWidth()  << ", " << VM.getBBHeight() << ");\n";
  }

  OS.flush();

  OS.setStream(nulls());
  return false;
}

void
TimingScriptGen::writeConstraintsFor(VASTSelector *Dst, TimingNetlist &TNL,
                                     STGDistances &STGDist) {
  AnnotatedCone Cache(TNL, STGDist, Dst, OS);

  annoataConstraintsFor(Cache, Dst);

  Cache.generateMCPEntries();
}

void TimingScriptGen::annoataConstraintsFor(AnnotatedCone &Cache,
                                            VASTSelector *Sel) {
  SmallVector<VASTSlot*, 8> AllSlots;
  typedef VASTSelector::const_iterator iterator;
  for (iterator I = Sel->begin(), E = Sel->end(); I != E; ++I)
    AllSlots.push_back((*I).getSlot());
  // Annotate all slots to FI and Guard, otherwise we may miss some path not
  // block by the keeped nodes.
  //extractTimingPaths(Cache, Sel, AllSlots, Sel->getFanin().get());
  //extractTimingPaths(Cache, Sel, AllSlots, Sel->getGuard().get());

  for (VASTSelector::const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U)) continue;

    extractTimingPaths(Cache, Sel, U.getSlot(), VASTValPtr(U).get());
    extractTimingPaths(Cache, Sel, U.getSlot(), VASTValPtr(U.getGuard()).get());
  }

  typedef VASTSelector::ann_iterator ann_iterator;
  for (ann_iterator I = Sel->ann_begin(), E = Sel->ann_end(); I != E; ++I) {
    ArrayRef<VASTSlot*> Slots(I->second);
    extractTimingPaths(Cache, Sel, Slots, VASTValPtr(I->first).get());
  }

  Cache.addSelector(Sel);
}

void TimingScriptGen::extractTimingPaths(AnnotatedCone &Cache, VASTSelector *Dst,
                                         ArrayRef<VASTSlot*> ReadSlots,
                                         VASTValue *DepTree) {
  // Trivial case: register to register path.
  if (VASTSeqValue *Src = dyn_cast<VASTSeqValue>(DepTree)){
    if (Cache.generateSubmoduleConstraints(Src)) return;

    Cache.addIntervalFromSrc(Src->getSelector(),
                             Cache.buildLeaf(Src, ReadSlots, 0, Dst));

    // Even a trivial path can be a false path, e.g.:
    // slot 1:
    // reg_a <= c + x;
    // slot 2:
    // reg_a <= reg_b
    // For DstAVS = reg_a@1, there are no timing path from reg_b.
    return;
  }

  // If Define Value is immediate or symbol, skip it.
  if (!isa<VASTExpr>(DepTree)) return;

  Cache.annotatePathInterval(DepTree, Dst, ReadSlots);
}

char TimingScriptGen::ID = 0;

INITIALIZE_PASS_BEGIN(TimingScriptGen, "vast-timing-script-generation",
                      "Generate timing script to export the behavior-level timing",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
INITIALIZE_PASS_END(TimingScriptGen, "vast-timing-script-generation",
                    "Generate timing script to export the behavior-level timing",
                    false, true)

Pass *llvm::createTimingScriptGenPass() {
  return new TimingScriptGen();
}