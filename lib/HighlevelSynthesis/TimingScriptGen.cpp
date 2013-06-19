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

struct PathIntervalQueryCache {
  TimingNetlist &TNL;
  SeqLiveVariables &SLV;
  VASTSelector *Dst;
  raw_ostream &OS;
  const uint32_t Inf;

  struct SrcInfo {
    unsigned NumCycles;
    float CriticalDelay;
    explicit SrcInfo(unsigned NumCycles = STGDistances::Inf,
                     float CriticalDelay = 0.0f)
      : NumCycles(NumCycles), CriticalDelay(CriticalDelay) {}

    // Update the source information with tighter cycles constraint and larger
    // critical delay
    void update(unsigned NumCycles, float CriticalDelay) {
      this->NumCycles = std::min(this->NumCycles, NumCycles);
      this->CriticalDelay = std::max(this->CriticalDelay, CriticalDelay);
    }

    void update(const SrcInfo &RHS) {
      update(RHS.NumCycles, RHS.CriticalDelay);
    }
  };

  typedef DenseMap<VASTSeqValue*, SrcInfo> SeqValSetTy;
  SeqValSetTy CyclesFromSrcLB;

  typedef DenseMap<VASTExpr*, SeqValSetTy> QueryCacheTy;
  QueryCacheTy QueryCache;

  PathIntervalQueryCache(TimingNetlist &TNL, SeqLiveVariables &SLV,
                         VASTSelector *Dst, raw_ostream &OS)
    : TNL(TNL), SLV(SLV), Dst(Dst), OS(OS), Inf(STGDistances::Inf)
  {}

  void reset() {
    QueryCache.clear();
    CyclesFromSrcLB.clear();
  }

  void addIntervalFromSrc(VASTSeqValue *Src,  unsigned Interval,
                          float NormalizedDelay) {
    assert(Interval && "unexpected zero interval!");
    assert((!Src->isSlot() || Interval <= 1)
           && "Bad interval for slot registers!");
    CyclesFromSrcLB[Src].update(Interval, NormalizedDelay);
  }

  bool generateSubmoduleConstraints(VASTSeqValue *SeqVal);

  void annotatePathInterval(VASTValue *Tree, ArrayRef<VASTSlot*> ReadSlots);

  unsigned getMinimalInterval(VASTSeqValue *Src, VASTSlot *ReadSlot);
  unsigned getMinimalInterval(VASTSeqValue *Src, ArrayRef<VASTSlot*> ReadSlots);

  // Propagate the timing information of the current combinational cone.
  void propagateInterval(VASTExpr *Expr, const SeqValSetTy &LocalIntervalMap) {
    typedef VASTOperandList::op_iterator iterator;
    SeqValSetTy &CurSet = QueryCache[Expr];
    for (iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      VASTExpr *SubExpr = dyn_cast<VASTExpr>(VASTValPtr(*I).get());
      if (SubExpr == 0) continue;

      QueryCacheTy::const_iterator at = QueryCache.find(SubExpr);
      if (at == QueryCache.end()) continue;

      const SeqValSetTy &Srcs = at->second;
      typedef SeqValSetTy::const_iterator src_iterator;
      for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
        assert(I->second.NumCycles && "Unexpected zero delay!");

        SrcInfo &SI = CurSet[I->first];
        // Look up the timing information from the map of current cone.
        SeqValSetTy::const_iterator at = LocalIntervalMap.find(I->first);
        assert(at != LocalIntervalMap.end() && "Node not visited yet?");
        SI.update(at->second);
      }
    }
  }

  void insertMCPEntries() const;
  // Bind multi-cycle path constraints to the scripting engine.
  template<typename SrcTy>
  void insertMCPWithInterval(SrcTy *Src, const std::string &ThuName,
                             const SrcInfo &SI) const;
  unsigned insertMCPThough(VASTExpr *Thu, const SeqValSetTy &SrcSet) const;

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

  void extractTimingPaths(PathIntervalQueryCache &Cache,
                          ArrayRef<VASTSlot*> ReadSlots,
                          VASTValue *DepTree);

  bool runOnVASTModule(VASTModule &VM);

  bool doInitialization(Module &) {
    OS <<
      "CREATE TABLE mcps( \
       id INTEGER PRIMARY KEY AUTOINCREMENT, \
       src TEXT, \
       dst TEXT, \
       thu TEXT, \
       cycles INTEGER, \
       normalized_delay REAL \
       );\n";

    return false;
  }

  TimingScriptGen() : VASTModulePass(ID), OS(), VM(0) {
    std::string MCPDataBasePath = getStrValueFromEngine("MCPDataBase");
    std::string Error;
    raw_fd_ostream *Output = new raw_fd_ostream(MCPDataBasePath.c_str(), Error);
    OS.setStream(*Output, true);

    initializeTimingScriptGenPass(*PassRegistry::getPassRegistry());
  }
};
}

unsigned PathIntervalQueryCache::getMinimalInterval(VASTSeqValue *Src,
                                                    VASTSlot *ReadSlot) {
  // Try to get the live variable at (Src, ReadSlot), calculate its distance
  // from its defining slots to the read slot.
  return SLV.getIntervalFromDef(Src, ReadSlot);
}

unsigned PathIntervalQueryCache::getMinimalInterval(VASTSeqValue *Src,
                                                    ArrayRef<VASTSlot*> ReadSlots) {
  unsigned PathInterval = Inf;
  typedef ArrayRef<VASTSlot*>::iterator iterator;
  for (iterator I = ReadSlots.begin(), E = ReadSlots.end(); I != E; ++I)
    PathInterval = std::min(PathInterval, getMinimalInterval(Src, *I));

  return PathInterval;
}

void PathIntervalQueryCache::annotatePathInterval(VASTValue *Root,
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

      unsigned Interval = getMinimalInterval(V, ReadSlots);
      float EstimatedDelay = TNL.getDelay(V, Root, Dst);
      SrcInfo CurInfo(Interval, EstimatedDelay);

      bool inserted = LocalInterval.insert(std::make_pair(V, CurInfo)).second;
      assert(inserted && "Node had already been visited?");
      QueryCache[Expr][V].update(CurInfo);
      // Add the information to statistics.
      addIntervalFromSrc(V, Interval, EstimatedDelay);
      continue;
    }  

    if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(ChildNode))
      VisitStack.push_back(std::make_pair(SubExpr, SubExpr->op_begin()));
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

void PathIntervalQueryCache::dump() const {
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
// The first node of the path is the use node and the last node of the path is
// the define node.
template<typename SrcTy>
void PathIntervalQueryCache::insertMCPWithInterval(SrcTy *Src,
                                                   const std::string &ThuName,
                                                   const SrcInfo &SI) const {
  assert(!ThuName.empty() && "Bad through node name!");
  OS << "INSERT INTO mcps(src, dst, thu, cycles, normalized_delay) VALUES(\n"
     << '\'' << Src->getSTAObjectName() << "', \n"
     << '\'' << Dst->getSTAObjectName() << "', \n"
     << '\'' << ThuName << "', \n"
     << SI.NumCycles << ", \n"
     << SI.CriticalDelay << ");\n";

  // Perform the Statistic.
  if (SI.NumCycles > 1) {
    ++NumMultiCyclesConstraints;
    if (SI.CriticalDelay > 1.0f) ++NumRequiredConstraints;
  }
  if (SI.NumCycles == PathIntervalQueryCache::Inf) ++NumFalseTimingPath;
  if (SI.NumCycles < SI.CriticalDelay) ++NumTimgViolation;
}

unsigned PathIntervalQueryCache::insertMCPThough(VASTExpr *Thu,
                                                 const SeqValSetTy &SrcSet)
                                                 const {
  std::string ThuName = "shang-null-node";
  if (Thu && !Thu->isAnonymous()) ThuName = Thu->getSTAObjectName();
  
  if (ThuName.empty()) return 0;

  typedef SeqValSetTy::const_iterator iterator;
  for (iterator I = SrcSet.begin(), E = SrcSet.end(); I != E; ++I)
    insertMCPWithInterval(I->first, ThuName, I->second);

  return SrcSet.size();
}

void PathIntervalQueryCache::insertMCPEntries() const {
  DEBUG(dbgs() << "Going to bind delay information of graph: \n");
  DEBUG(dump());
  DEBUG(dbgs() << "Binding path for dst register: "
               << Dst->getName() << '\n');

  insertMCPThough(0, CyclesFromSrcLB);
  NumConstraints += CyclesFromSrcLB.size();

  typedef QueryCacheTy::const_iterator iterator;
  for (iterator I = QueryCache.begin(), E = QueryCache.end(); I != E; ++I)
    NumConstraints += insertMCPThough(I->first, I->second);
}

bool PathIntervalQueryCache::generateSubmoduleConstraints(VASTSeqValue *SeqVal) {
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
    insertMCPWithInterval(Operand, "shang-null-node",
                          SrcInfo(Latency, float(Latency)));
  }

  return true;
}

bool TimingScriptGen::runOnVASTModule(VASTModule &VM)  {
  // No need to write timing script at all.
  if (DisableTimingScriptGeneration) return false;

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

  return false;
}

void
TimingScriptGen::writeConstraintsFor(VASTSelector *Dst, TimingNetlist &TNL,
                                     SeqLiveVariables &SLV) {
  DenseMap<VASTValue*, SmallVector<VASTSlot*, 8> > DatapathMap;

  typedef VASTSelector::const_iterator vn_itertor;
  for (vn_itertor I = Dst->begin(), E = Dst->end(); I != E; ++I) {
    const VASTLatch &DstLatch = *I;
    VASTSlot *ReadSlot = DstLatch.getSlot();
    // Paths for the condition.
    DatapathMap[VASTValPtr(DstLatch.getGuard()).get()].push_back(ReadSlot);
    if (VASTValPtr SlotActive = DstLatch.getSlotActive())
      DatapathMap[SlotActive.get()].push_back(ReadSlot);
    // Paths for the assigning value
    DatapathMap[VASTValPtr(DstLatch).get()].push_back(ReadSlot);
  }

  PathIntervalQueryCache Cache(TNL, SLV, Dst, OS);
  typedef DenseMap<VASTValue*, SmallVector<VASTSlot*, 8> >::iterator it;
  for (it I = DatapathMap.begin(), E = DatapathMap.end(); I != E; ++I)
    extractTimingPaths(Cache, I->second, I->first);

  Cache.insertMCPEntries();
}

void TimingScriptGen::extractTimingPaths(PathIntervalQueryCache &Cache,
                                         ArrayRef<VASTSlot*> ReadSlots,
                                         VASTValue *DepTree) {
  // Trivial case: register to register path.
  if (VASTSeqValue *Src = dyn_cast<VASTSeqValue>(DepTree)){
    if (Cache.generateSubmoduleConstraints(Src)) return;

    unsigned Interval = Cache.getMinimalInterval(Src, ReadSlots);
    Cache.addIntervalFromSrc(Src, Interval, 0.0f);

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
