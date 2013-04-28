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
#include "STGShortestPath.h"
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
static cl::opt<bool>
DisableSingleCycleConstraints("shang-disable-single-cycle-constraints",
                              cl::desc("Disable single cycle constraints generation"),
                              cl::init(true));

STATISTIC(NumTimingPath, "Number of timing paths analyzed (From->To pair)");
STATISTIC(NumMultiCyclesTimingPath, "Number of multicycles timing paths "
                                    "analyzed (From->To pair)");
STATISTIC(NumMaskedMultiCyclesTimingPath,
          "Number of timing paths that masked by path with smaller slack "
          "(From->To pair)");
STATISTIC(NumFalseTimingPath,
          "Number of false timing paths detected (From->To pair)");
STATISTIC(NumConstraints, "Number of timing constraints generated");

namespace{
struct TimingScriptGen;

struct PathIntervalQueryCache {
  TimingNetlist &TNL;
  SeqLiveVariables &SLV;
  STGShortestPath &SSP;
  VASTSeqValue *Dst;
  raw_ostream &OS;
  const uint32_t Inf;

  typedef std::map<VASTSeqValue*, std::map<unsigned, float> > SrcIntervalMapTy;
  SrcIntervalMapTy CyclesFromSrc;

  typedef DenseMap<VASTSeqValue*, unsigned> SeqValSetTy;
  typedef DenseMap<VASTValue*, SeqValSetTy> QueryCacheTy;
  QueryCacheTy QueryCache;

  PathIntervalQueryCache(TimingNetlist &TNL, SeqLiveVariables &SLV,
                         STGShortestPath &SSP, VASTSeqValue *Dst,
                         raw_ostream &OS)
    : TNL(TNL), SLV(SLV), SSP(SSP), Dst(Dst), OS(OS), Inf(STGShortestPath::Inf)
  {}

  void reset() {
    QueryCache.clear();
    CyclesFromSrc.clear();
  }

  void addIntervalFromSrc(VASTSeqValue *Src, unsigned Interval, float Delay) {
    assert(Interval && "unexpected zero interval!");
    float &OldDelay = CyclesFromSrc[Src][Interval];
    OldDelay = std::max(OldDelay, Delay);
  }

  bool annotateSubmoduleLatency(VASTSeqValue * V);

  void annotatePathInterval(VASTValue *Tree, ArrayRef<VASTSlot*> ReadSlots);

  unsigned getMinimalInterval(VASTSeqValue *Src, VASTSlot *ReadSlot);
  unsigned getMinimalInterval(VASTSeqValue *Src, ArrayRef<VASTSlot*> ReadSlots);

  bool updateInterval(SeqValSetTy &To, const SeqValSetTy &From,
                      const SeqValSetTy &LocalIntervalMap) {
    bool Changed = false;

    typedef SeqValSetTy::const_iterator it;
    for (it I = From.begin(), E = From.end(); I != E; ++I) {
      assert(I->second && "Unexpected zero delay!");

      unsigned &ExistInterval = To[I->first];
      // Look up the delay from local delay map, because the delay of the From
      // map may correspond to another data-path with different destination.
      SeqValSetTy::const_iterator at = LocalIntervalMap.find(I->first);
      assert(at != LocalIntervalMap.end() && "Node not visited yet?");
      if (ExistInterval == 0 || ExistInterval > at->second) {
        ExistInterval = at->second;
        Changed |= true;
      }
    }

    return Changed;
  }

  void insertMCPEntries() const;
  // Bind multi-cycle path constraints to the scripting engine.
  unsigned insertMCPWithInterval(VASTSeqValue *Src, float NormalizedDelay,
                                 unsigned Interval, bool SkipThu) const;

  unsigned writeThuNodes(raw_ostream &OS, VASTSeqValue *Src,
                                       unsigned Interval) const;

  typedef SrcIntervalMapTy::mapped_type::const_iterator interval_iterator;
  typedef SrcIntervalMapTy::const_iterator src_iterator;
  src_iterator src_begin() const { return CyclesFromSrc.begin(); }
  src_iterator src_end() const { return CyclesFromSrc.end(); }

  void dump() const;
};

struct TimingScriptGen : public VASTModulePass {
  raw_ostream &OS;

  VASTModule *VM;
  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<SeqLiveVariables>();
    AU.addRequired<STGShortestPath>();
    AU.addRequired<TimingNetlist>();
    AU.addRequired<DataLayout>();
    AU.setPreservesAll();
  }

  void writeConstraintsFor(VASTSeqValue *Dst, TimingNetlist &TNL,
                           SeqLiveVariables &SLV, STGShortestPath &SSP);

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
  
  TimingScriptGen() : VASTModulePass(ID), OS(nulls()) {
    llvm_unreachable("Bad constructor!");
  }

  TimingScriptGen(raw_ostream &O) : VASTModulePass(ID), OS(O), VM(0) {
    initializeTimingScriptGenPass(*PassRegistry::getPassRegistry());
  }
};
}

unsigned PathIntervalQueryCache::getMinimalInterval(VASTSeqValue *Src,
                                                    VASTSlot *ReadSlot) {
  // Try to get the live variable at (Src, ReadSlot), calculate its distance
  // from its defining slots to the read slot.
  return SLV.getIntervalFromDef(Src, ReadSlot, &SSP);
}

unsigned PathIntervalQueryCache::getMinimalInterval(VASTSeqValue *Src,
                                                    ArrayRef<VASTSlot*> ReadSlots) {
  unsigned PathInterval = Inf;
  typedef ArrayRef<VASTSlot*>::iterator iterator;
  for (iterator I = ReadSlots.begin(), E = ReadSlots.end(); I != E; ++I)
    PathInterval = std::min(PathInterval, getMinimalInterval(Src, *I));

  return PathInterval;
}

bool PathIntervalQueryCache::annotateSubmoduleLatency(VASTSeqValue * V) {
  VASTSubModule *SubMod = dyn_cast<VASTSubModule>(V->getParent());
  if (SubMod == 0) return false;

  assert(V->getValType() == VASTSeqValue::IO && "Bad Seqvalue type!");
  assert(V->empty() && "Unexpected assigement to return value!");

  unsigned Latency = SubMod->getLatency();
  // No latency information available.
  if (Latency == 0) return true;

  // Add the timing constraints from operand registers to the output registers.
  typedef VASTSubModule::fanin_iterator fanin_iterator;
  for (fanin_iterator I = SubMod->fanin_begin(), E = SubMod->fanin_end();
       I != E; ++I) {
    VASTSeqValue *Operand = *I;
    addIntervalFromSrc(Operand, Latency, float(Latency));
  }

  return true;
}

void PathIntervalQueryCache::annotatePathInterval(VASTValue *Root,
                                                  ArrayRef<VASTSlot*> ReadSlots) {
  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(Root);

  if (L == 0) return;

  typedef  VASTOperandList::op_iterator ChildIt;

  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;
  std::set<VASTValue*> Visited;
  // Remember the number of cycles from the reachable register to the read slot.
  SeqValSetTy LocalInterval;

  VisitStack.push_back(std::make_pair(Root, L->op_begin()));
  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    SeqValSetTy &ParentReachableRegs = QueryCache[Node];

    // All sources of this node is visited.
    if (It ==  VASTOperandList::GetDatapathOperandList(Node)->op_end()) {
      VisitStack.pop_back();
      // Add the supporting register of current node to its parent's
      // supporting register set.
      if (!VisitStack.empty())
        updateInterval(QueryCache[VisitStack.back().first], ParentReachableRegs,
                       LocalInterval);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (ChildNode == 0) continue;

    // And do not visit a node twice.
    if (!Visited.insert(ChildNode).second) {
      // If there are tighter delay from the child, it means we had already
      // visited the sub-tree.
      updateInterval(ParentReachableRegs, QueryCache[ChildNode], LocalInterval);
      continue;
    }

    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(ChildNode)) {
      if (V->empty()) {
        assert(V->getValType() == VASTSeqValue::IO && "Unexpected empty SeqVal!");
        continue;
      }

      unsigned Interval = getMinimalInterval(V, ReadSlots);

      bool inserted = LocalInterval.insert(std::make_pair(V, Interval)).second;
      assert(inserted && "Node had already been visited?");
      unsigned &ExistedInterval = ParentReachableRegs[V];
      if (ExistedInterval == 0 || Interval < ExistedInterval)
        ExistedInterval = Interval;

      float EstimatedDelay = TNL.getDelay(V, Root, Dst).getNormalizedDelay();
      // Add the information to statistics.
      addIntervalFromSrc(V, Interval, EstimatedDelay);
      continue;
    }

    if (VASTOperandList *L = VASTOperandList::GetDatapathOperandList(ChildNode))
      VisitStack.push_back(std::make_pair(ChildNode, L->op_begin()));
  }

  // Check the result, debug only.
  DEBUG(QueryCacheTy::iterator at = QueryCache.find(Root);
  assert(at != QueryCache.end()
         && "Timing path information for root not found!");
  const SeqValSetTy &RootSet = at->second;
  typedef SeqValSetTy::const_iterator it;
  bool IntervalMasked = false;
  for (it I = LocalInterval.begin(), E = LocalInterval.end(); I != E; ++I) {
    SeqValSetTy::const_iterator ActualIntervalAt = RootSet.find(I->first);
    assert(ActualIntervalAt != RootSet.end() && "Timing path entire missed!");
    assert(ActualIntervalAt->second <= I->second
           && "Interval information not applied?");
    if (ActualIntervalAt->second == I->second) continue;

    DEBUG(dbgs() << "Timing path masked: Root is";
    Root->printAsOperand(dbgs(), false);
    dbgs() << " end node is " << I->first->getName()
           << " masked delay: " << I->second
           << " actual delay: " << ActualIntervalAt->second << '\n');
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

static bool writeObjectName(raw_ostream &OS, const VASTValue *V) {
  if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V)) {
    // The block RAM should be printed as Prefix + ArrayName in the script.
    if (const VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(V)) {
      if (SeqVal->getValType() == VASTSeqValue::BRAM) {
        const VASTBlockRAM *RAM = cast<VASTBlockRAM>(SeqVal->getParent());
        OS << " *"
          // BlockRam name with prefix
          << getFUDesc<VFUBRAM>()->Prefix
          << VFUBRAM::getArrayName(RAM->getBlockRAMNum()) << "* *"
          // Or simply the name of the output register.
          << VFUBRAM::getArrayName(RAM->getBlockRAMNum())
          << "* ";
        return true;
      }
    }

    if (const char *N = NV->getName()) {
      OS << " *" << N << "* ";
      return true;
    }
  } else if (const VASTExpr *E = dyn_cast<VASTExpr>(V)) {
    std::string Name = E->getSubModName();
    if (!Name.empty()) {
      OS << " *" << E->getSubModName() << "* ";
      return true;
    }
  }

  return false;
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
      dbgs() << '#' << RI->second << "), ";
    }

    dbgs() << "}\n";
  }
}

unsigned PathIntervalQueryCache::writeThuNodes(raw_ostream &OS, VASTSeqValue *Src,
                                               unsigned Interval) const {
  unsigned NumNodesPrinted = 0;

  typedef QueryCacheTy::const_iterator it;
  for (it I = QueryCache.begin(), E = QueryCache.end(); I != E; ++I) {
    const SeqValSetTy &Set = I->second;
    SeqValSetTy::const_iterator at = Set.find(Src);
    // The register may be not reachable from this node.
    if (at == Set.end() || at->second != Interval) continue;

    if (writeObjectName(OS, I->first))
      ++NumNodesPrinted;
  }

  return NumNodesPrinted;
}

// The first node of the path is the use node and the last node of the path is
// the define node.
unsigned PathIntervalQueryCache::insertMCPWithInterval(VASTSeqValue *Src,
                                                       float NormalizedDelay,
                                                       unsigned Interval,
                                                       bool IsLB) const {
  OS << "INSERT INTO mcps(src, dst, thu, cycles, normalized_delay) VALUES(\n";
  
  OS << '\'';
  writeObjectName(OS, Src);
  OS << "', \n";

  OS << '\'';
  writeObjectName(OS, Dst);
  OS << "', \n";

  OS << '\'';
  if (IsLB || (writeThuNodes(OS, Src, Interval) == 0))
    OS << "shang-null-node";
  OS << "', \n";

  OS << Interval << ", \n";
  OS << NormalizedDelay << ");\n";

  return 1;
}

void PathIntervalQueryCache::insertMCPEntries() const {
  DEBUG(dbgs() << "Going to bind delay information of graph: \n");
  DEBUG(dump());
  DEBUG(dbgs() << "Binding path for dst register: "
               << Dst->getName() << '\n');
  for (src_iterator I = src_begin(), E = src_end();I != E;++I) {
    VASTSeqValue *Src = I->first;
    const SrcIntervalMapTy::mapped_type &Intervals = I->second;
    unsigned LastInterval = 0;

    for (interval_iterator SI = Intervals.begin(), SE = Intervals.end();
         SI != SE; ++SI) {
      unsigned Interval = SI->first;
      float CriticalDelay = SI->second;
      assert(Interval > LastInterval && "Bad iterate order!");
      LastInterval = Interval;

      ++NumTimingPath;
      if (Interval >= Inf) ++NumFalseTimingPath;
      else if (Interval > 1) ++NumMultiCyclesTimingPath;

      // Do not generate constraints for single-cycle path.
      if (DisableSingleCycleConstraints && Interval == 1) continue;

      // The interval is lowerbound if it is the first element of the interval
      // set.
      bool IsLB = (LastInterval == 0);
      DEBUG(dbgs().indent(2) << "from: " << Src->getName() << '#'
                             << I->first << '\n');
      // If we not visited the path before, this path is the critical path,
      // since we are iteration the path from the smallest delay to biggest
      // delay. For the critical path, we do not generate the constraints with
      // through nodes, this make quartus assume that all paths from src to dst
      // are at least have N cycles available, where N equals to the value of
      // variable "Interval".
      unsigned NumThuNodes
        = insertMCPWithInterval(Src, CriticalDelay, Interval, IsLB);
      NumConstraints += std::max(1u, NumThuNodes);

      if (NumThuNodes == 0 && !IsLB && Interval > 1 && Interval != Inf) {
        ++NumMaskedMultiCyclesTimingPath;
        DEBUG(dbgs() << "Path hidden: " << Src->getName() << " -> "
               << Dst->getName() << " Interval " << Interval << '\n');
      }
    }
  }
}

bool TimingScriptGen::runOnVASTModule(VASTModule &VM)  {
  // No need to write timing script at all.
  if (DisableTimingScriptGeneration) return false;

  bindFunctionToScriptEngine(getAnalysis<DataLayout>(), &VM);

  SeqLiveVariables &SLV = getAnalysis<SeqLiveVariables>();
  STGShortestPath &SSP = getAnalysis<STGShortestPath>();
  TimingNetlist &TNL =getAnalysis<TimingNetlist>();

  //Write the timing constraints.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *V = I;

    if (V->empty()) continue;

    writeConstraintsFor(V, TNL, SLV, SSP);
  }

  return false;
}

void
TimingScriptGen::writeConstraintsFor(VASTSeqValue *Dst, TimingNetlist &TNL,
                                     SeqLiveVariables &SLV,
                                     STGShortestPath &SSP) {
  DenseMap<VASTValue*, SmallVector<VASTSlot*, 8> > DatapathMap;

  typedef VASTSeqValue::const_iterator vn_itertor;
  for (vn_itertor I = Dst->begin(), E = Dst->end(); I != E; ++I) {
    const VASTLatch &DstLatch = *I;
    VASTSlot *ReadSlot = DstLatch.getSlot();
    // Paths for the condition.
    DatapathMap[((VASTValPtr)DstLatch.getPred()).get()].push_back(ReadSlot);
    if (VASTValPtr SlotActive = DstLatch.getSlotActive())
      DatapathMap[SlotActive.get()].push_back(ReadSlot);
    // Paths for the assigning value
    DatapathMap[((VASTValPtr)DstLatch).get()].push_back(ReadSlot);
  }

  PathIntervalQueryCache Cache(TNL, SLV, SSP, Dst, OS);
  typedef DenseMap<VASTValue*, SmallVector<VASTSlot*, 8> >::iterator it;
  for (it I = DatapathMap.begin(), E = DatapathMap.end(); I != E; ++I)
    extractTimingPaths(Cache, I->second, I->first);

  Cache.insertMCPEntries();
}

void TimingScriptGen::extractTimingPaths(PathIntervalQueryCache &Cache,
                                         ArrayRef<VASTSlot*> ReadSlots,
                                         VASTValue *DepTree) {
  VASTValue *SrcValue = DepTree;

  // Trivial case: register to register path.
  if (VASTSeqValue *Src = dyn_cast<VASTSeqValue>(SrcValue)){
    // Src may be the return_value of the submodule.
    if (Cache.annotateSubmoduleLatency(Src)) return;

    if (Src->empty()) {
      assert(Src->getValType() == VASTSeqValue::IO && "Unexpected empty SeqVal!");
      return;
    }

    int Interval = Cache.getMinimalInterval(Src, ReadSlots);
    Cache.addIntervalFromSrc(Src, Interval, 0);
    // Even a trivial path can be a false path, e.g.:
    // slot 1:
    // reg_a <= c + x;
    // slot 2:
    // reg_a <= reg_b
    // For DstAVS = reg_a@1, there are no timing path from reg_b.
    return;
  }

  // If Define Value is immediate or symbol, skip it.
  if (!isa<VASTWire>(SrcValue) && !isa<VASTExpr>(SrcValue)) return;

  Cache.annotatePathInterval(DepTree, ReadSlots);
}

char TimingScriptGen::ID = 0;
INITIALIZE_PASS_BEGIN(TimingScriptGen, "vast-timing-script-generation",
                      "Generate timing script to export the behavior-level timing",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(STGShortestPath)
INITIALIZE_PASS_END(TimingScriptGen, "vast-timing-script-generation",
                    "Generate timing script to export the behavior-level timing",
                    false, true)

Pass *llvm::createTimingScriptGenPass(raw_ostream &O) {
  return new TimingScriptGen(O);
}
