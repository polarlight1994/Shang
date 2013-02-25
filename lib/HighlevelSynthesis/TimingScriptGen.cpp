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

#include "shang/VASTModulePass.h"
#include "shang/VASTSubModules.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"
#include "shang/Utilities.h"

#include "llvm/Pass.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-comb-path-interval"
#include "llvm/Support/Debug.h"
using namespace llvm;

static cl::opt<bool>
DisableTimingScriptGeneration("shang-disable-timing-script",
                              cl::desc("Disable timing script generation"),
                              cl::init(false));

STATISTIC(NumTimingPath, "Number of timing paths analyzed (From->To pair)");
STATISTIC(NumMultiCyclesTimingPath, "Number of multicycles timing paths "
                                    "analyzed (From->To pair)");
STATISTIC(NumMaskedMultiCyclesTimingPath,
          "Number of timing paths that masked by path with smaller slack "
          "(From->To pair)");
STATISTIC(NumFalseTimingPath,
          "Number of false timing paths detected (From->To pair)");

namespace{
struct TimingScriptGen;

struct PathIntervalQueryCache {
  TimingNetlist *TNL;

  typedef std::map<unsigned, DenseSet<VASTSeqValue*> > IntervalStatsMapTy;
  typedef DenseMap<VASTSeqValue*, unsigned> SeqValSetTy;
  typedef DenseMap<VASTValue*, SeqValSetTy> QueryCacheTy;
  QueryCacheTy QueryCache;
  // Statistics for simple path and complex paths.
  IntervalStatsMapTy Stats[2];

  explicit PathIntervalQueryCache(TimingNetlist *TNL) : TNL(TNL) {}

  void reset() {
    QueryCache.clear();
    Stats[0].clear();
    Stats[1].clear();
  }

  void addIntervalFromToStats(VASTSeqValue *Src, unsigned Interval, bool isSimple) {
    Stats[isSimple][Interval].insert(Src);
  }

  bool annotateSubmoduleLatency(VASTSeqValue * V);

  void annotatePathInterval(/*SeqReachingDefAnalysis *R,*/ VASTValue *Tree,
                         ArrayRef<VASTSeqUse> DstUses);

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

  void bindAllPath2ScriptEngine(VASTSeqValue *Dst) const;
  void bindAllPath2ScriptEngine(VASTSeqValue *Dst, bool IsSimple,
                                DenseSet<VASTSeqValue*> &BoundSrc) const;
  // Bind multi-cycle path constraints to the scripting engine.
  unsigned bindMCPC2ScriptEngine(VASTSeqValue *Dst, VASTSeqValue *Src,
                                 unsigned Interval, bool SkipThu,
                                 bool IsCritical) const;

  // Bind the combinational delay to the scripting engine to verify the timing
  // analysis.
  unsigned bindDelay2ScriptEngine(VASTSeqValue *Dst, VASTSeqValue *Src,
                                  unsigned Interval, bool SkipThu,
                                  bool IsCritical) const;

  unsigned printPathWithIntervalFrom(raw_ostream &OS, VASTSeqValue *Src,
                                     unsigned Interval) const;

  typedef DenseSet<VASTSeqValue*>::const_iterator src_it;
  typedef IntervalStatsMapTy::const_iterator delay_it;
  delay_it stats_begin(bool IsSimple) const { return Stats[IsSimple].begin(); }
  delay_it stats_end(bool IsSimple) const { return Stats[IsSimple].end(); }

  void dump() const;
};

struct TimingScriptGen : public VASTModulePass {
  VASTModule *VM;

  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    //AU.addRequired<SeqLiveVariables>();
    AU.addRequired<DataLayout>();
    AU.addRequired<TimingNetlist>();
    AU.setPreservesAll();
  }

  void writeConstraintsForDst(VASTSeqValue *Dst, TimingNetlist *TNL);

  void extractTimingPaths(PathIntervalQueryCache &Cache, ArrayRef<VASTSeqUse> Uses,
                          VASTValue *DepTree);

  bool runOnVASTModule(VASTModule &VM);

  bool doInitialization(Module &) {
    SMDiagnostic Err;
    // Get the script from script engine.
    const char *HeaderScriptPath[] = { "Misc",
                                       "TimingConstraintsHeaderScript" };
    if (!runScriptStr(getStrValueFromEngine(HeaderScriptPath), Err))
      report_fatal_error("Error occur while running timing header script:\n"
                         + Err.getMessage());
    return false;
  }

  TimingScriptGen() : VASTModulePass(ID), VM(0) {
    initializeTimingScriptGenPass(*PassRegistry::getPassRegistry());
  }
};
}

static unsigned getMinimalInterval(/*SeqReachingDefAnalysis *R,*/ VASTSeqValue *Src,
                                const VASTSeqUse &Dst) {
  unsigned PathInterval = 2;
  VASTSlot *DstSlot = Dst.getSlot();

  typedef VASTSeqValue::const_iterator vn_itertor;
  for (vn_itertor I = Src->begin(), E = Src->end(); I != E; ++I) {
    VASTSeqUse Def = *I;

    // Update the PathInterval if the source VAS reaches DstSlot.
    //if (unsigned Distance = DstSI->getCyclesFromDef(Def)) {
    //  assert(Distance < 10000 && "Distance too large!");
    //  PathInterval = std::min(PathInterval, Distance);
    //}
  }

  return PathInterval;
}

static unsigned getMinimalInterval(/*SeqReachingDefAnalysis *R,*/ VASTSeqValue *Src,
                                ArrayRef<VASTSeqUse> DstUses) {
  unsigned PathInterval = 10000;
  typedef ArrayRef<VASTSeqUse>::iterator iterator;
  for (iterator I = DstUses.begin(), E = DstUses.end(); I != E; ++I)
    PathInterval = std::min(PathInterval, getMinimalInterval(/*R,*/ Src, *I));

  return PathInterval;
}

static bool printBindingLuaCode(raw_ostream &OS, const VASTValue *V) {
  if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V)) {
    // The block RAM should be printed as Prefix + ArrayName in the script.
    if (const VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(V)) {
      if (SeqVal->getValType() == VASTSeqValue::BRAM) {
        const VASTBlockRAM *RAM = cast<VASTBlockRAM>(SeqVal->getParent());
        OS << " { NameSet =[=[ [ list "
          // BlockRam name with prefix
          << getFUDesc<VFUBRAM>()->Prefix
          << VFUBRAM::getArrayName(RAM->getBlockRAMNum()) << ' '
          // Or simply the name of the output register.
          << VFUBRAM::getArrayName(RAM->getBlockRAMNum())
          << " ] ]=] }";
        return true;
      }
    }

    if (const char *N = NV->getName()) {
      OS << " { NameSet =[=[ [ list " << N << " ] ]=] }";
      return true;
    }
  } else if (const VASTExpr *E = dyn_cast<VASTExpr>(V)) {
    std::string Name = E->getSubModName();
    if (!Name.empty()) {
      OS << " { NameSet =[=[ [ list " << E->getSubModName() << " ] ]=] }";
      return true;
    }
  }

  return false;
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
    addIntervalFromToStats(Operand, Latency, true);
  }

  return true;
}

void PathIntervalQueryCache::annotatePathInterval(/*SeqReachingDefAnalysis *R,*/
                                            VASTValue *Root,
                                            ArrayRef<VASTSeqUse> DstUses) {
  assert((isa<VASTWire>(Root) || isa<VASTExpr>(Root)) && "Bad root type!");
  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;
  std::set<VASTValue*> Visited;
  SeqValSetTy LocalInterval;

  VisitStack.push_back(std::make_pair(Root, VASTValue::dp_dep_begin(Root)));
  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    SeqValSetTy &ParentReachableRegs = QueryCache[Node];

    // All sources of this node is visited.
    if (It == VASTValue::dp_dep_end(Node)) {
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
      unsigned Interval = getMinimalInterval(/*R,*/ V, DstUses);

      bool inserted = LocalInterval.insert(std::make_pair(V, Interval)).second;
      assert(inserted && "Node had already been visited?");
      unsigned &ExistedInterval = ParentReachableRegs[V];
      if (ExistedInterval == 0 || Interval < ExistedInterval)
        ExistedInterval = Interval;

      // Add the information to statistics.
      addIntervalFromToStats(V, Interval, false);
      continue;
    }

    if (!isa<VASTWire>(ChildNode) && !isa<VASTExpr>(ChildNode)) continue;

    VisitStack.push_back(std::make_pair(ChildNode,
                                        VASTValue::dp_dep_begin(ChildNode)));
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

    dbgs() << "Timing path masked: Root is";
    Root->printAsOperand(dbgs(), false);
    dbgs() << " end node is " << I->first->getName()
           << " masked delay: " << I->second
           << " actual delay: " << ActualIntervalAt->second << '\n';
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

unsigned PathIntervalQueryCache::printPathWithIntervalFrom(raw_ostream &OS,
                                                           VASTSeqValue *Src,
                                                           unsigned Interval)
                                                           const {
  unsigned NumNodesPrinted = 0;

  typedef QueryCacheTy::const_iterator it;
  for (it I = QueryCache.begin(), E = QueryCache.end(); I != E; ++I) {
    const SeqValSetTy &Set = I->second;
    SeqValSetTy::const_iterator at = Set.find(Src);
    // The register may be not reachable from this node.
    if (at == Set.end() || at->second != Interval) continue;

    if (printBindingLuaCode(OS, I->first)) {
      OS << ", ";
      ++NumNodesPrinted;
    }
  }

  return NumNodesPrinted;
}

void PathIntervalQueryCache::dump() const {
  dbgs() << "\nCurrent data-path timing:\n";
  typedef QueryCacheTy::const_iterator it;
  for (it I = QueryCache.begin(), E = QueryCache.end(); I != E; ++I) {
    const SeqValSetTy &Set = I->second;
    typedef SeqValSetTy::const_iterator reg_it;

    if (!printBindingLuaCode(dbgs(), I->first))
      continue;

    dbgs() << "\n\t{ ";
    for (reg_it RI = Set.begin(), RE = Set.end(); RI != RE; ++RI) {
      dbgs() << '(';
      printBindingLuaCode(dbgs(), RI->first);
      dbgs() << '#' << RI->second << "), ";
    }

    dbgs() << "}\n";
  }
}

static bool printDelayRecord(raw_ostream &OS, VASTSeqValue *Dst,
                             VASTSeqValue *Src, VASTValue *Thu,
                             const TNLDelay &delay) {
  // Record: {Src = '', Dst = '', THU = '', delay = '', max_ll = '', min_ll = ''}
  OS << "{ Src=";
  printBindingLuaCode(OS, Src);
  OS << ", ";

  OS << "Dst=";
  printBindingLuaCode(OS, Dst);
  OS << ", ";

  OS << "Thu=";
  if (Thu == 0)
    OS << "nil";
  else if (!printBindingLuaCode(OS, Thu))
    OS << "'<null>'";
  OS << ", ";

  OS << "Delay = '" << delay.getDelay() << "', ";

  OS << "MaxLL = '" << delay.getMaxLL() << "', ";
  OS << "MinLL = '" << delay.getMinLL() << '\'';

  OS << " }";
  return Thu != 0;
}

unsigned PathIntervalQueryCache::bindDelay2ScriptEngine(VASTSeqValue *Dst,
                                                        VASTSeqValue *Src,
                                                        unsigned Interval,
                                                        bool SkipThu,
                                                        bool IsCritical) const {
  // Delay table:
  // Datapath: {
  //  {Src = '', Dst = '', THU = '', delay = '', max_ll = '', min_ll = ''}
  // }
  // }
  SMDiagnostic Err;

  if (!runScriptStr("RTLDatapathDelay = {}\n", Err))
    llvm_unreachable("Cannot create RTLDatapath table in scripting pass!");

  std::string Script;
  raw_string_ostream SS(Script);

  Script.clear();

  unsigned NumThuNodePrinted = 0;
  SS << "RTLDatapathDelay = { ";

  // Only print the source and the dst.
  if (SkipThu) {
    printDelayRecord(SS, Dst, Src, 0, TNL->getDelay(Src, Dst));
  } else {
    typedef QueryCacheTy::const_iterator it;
    for (it I = QueryCache.begin(), E = QueryCache.end(); I != E; ++I) {
      const SeqValSetTy &Set = I->second;
      SeqValSetTy::const_iterator at = Set.find(Src);
      // The register may be not reachable from this node.
      if (at == Set.end() || at->second != Interval) continue;
      if (NumThuNodePrinted) SS << ", ";

      const TNLDelay &Delay = TNL->getDelay(Src, I->first, Dst);

      if (printDelayRecord(SS, Dst, Src, I->first, Delay)) {
        ++NumThuNodePrinted;
      }
    }
  }

  SS << " }";

  SS.flush();
  if (!runScriptStr(Script, Err))
    llvm_unreachable("Cannot create node table of RTLDatapath!");

  // Get the script from script engine.
  const char *DatapathScriptPath[] = { "Misc", "DelayVerifyScript" };
  if (!runScriptStr(getStrValueFromEngine(DatapathScriptPath), Err))
    report_fatal_error("Error occur while running datapath script:\n"
                       + Err.getMessage());

  return NumThuNodePrinted;
}

// The first node of the path is the use node and the last node of the path is
// the define node.
unsigned PathIntervalQueryCache::bindMCPC2ScriptEngine(VASTSeqValue *Dst,
                                                       VASTSeqValue *Src,
                                                       unsigned Interval,
                                                       bool SkipThu,
                                                       bool IsCritical) const {
  // Path table:
  // Datapath: {
  //  unsigned Slack,
  //  table NodesInPath
  // }
  SMDiagnostic Err;

  if (!runScriptStr("RTLDatapath = {}\n", Err))
    llvm_unreachable("Cannot create RTLDatapath table in scripting pass!");

  // Hack the interval.
  // DIRTY HACK: Ignore the div/rem
  if (Interval < 32)
    Interval = std::max<unsigned>(TNL->getDelay(Src, Dst).getNumCycles(), 1);

  std::string Script;
  raw_string_ostream SS(Script);
  SS << "RTLDatapath.Slack = " << Interval << '\n';
  SS << "RTLDatapath.isCriticalPath = " << (SkipThu || IsCritical) << '\n';
  SS.flush();
  if (!runScriptStr(Script, Err))
    llvm_unreachable("Cannot create slack of RTLDatapath!");

  Script.clear();

  unsigned NumThuNodePrinted = 0;
  SS << "RTLDatapath.Nodes = { ";
  printBindingLuaCode(SS, Dst);
  SS << ", ";

  if (!SkipThu)
    NumThuNodePrinted = printPathWithIntervalFrom(SS, Src, Interval);

  printBindingLuaCode(SS, Src);
  SS << " }";

  SS.flush();
  if (!runScriptStr(Script, Err))
    llvm_unreachable("Cannot create node table of RTLDatapath!");

  // Get the script from script engine.
  const char *DatapathScriptPath[] = { "Misc", "DatapathScript" };
  if (!runScriptStr(getStrValueFromEngine(DatapathScriptPath), Err))
    report_fatal_error("Error occur while running datapath script:\n"
                       + Err.getMessage());

  return NumThuNodePrinted;
}

void
PathIntervalQueryCache::bindAllPath2ScriptEngine(VASTSeqValue *Dst, bool IsSimple,
                                              DenseSet<VASTSeqValue*> &BoundSrc)
                                              const {
  DEBUG(dbgs() << "Binding path for dst register: "
               << Dst->getName() << '\n');
  for (delay_it I = stats_begin(IsSimple), E = stats_end(IsSimple);I != E;++I) {
    unsigned Interval = I->first;

    for (src_it SI = I->second.begin(), SE = I->second.end(); SI != SE; ++SI) {
      VASTSeqValue *Src = *SI;
      bool Visited = !BoundSrc.insert(Src).second;
      assert((!IsSimple || !Visited)
             && "A simple path should not have been visited!");
      ++NumTimingPath;
      if (Interval == 10000) ++NumFalseTimingPath;
      else if (Interval > 1) ++NumMultiCyclesTimingPath;

      DEBUG(dbgs().indent(2) << "from: " << Src->getName() << '#'
                             << I->first << '\n');
      // If we not visited the path before, this path is the critical path,
      // since we are iteration the path from the smallest delay to biggest
      // delay.
      unsigned NumConstraints = bindMCPC2ScriptEngine(Dst, Src, Interval,
                                                      IsSimple, !Visited);
      if (NumConstraints == 0 && !IsSimple && Interval > 1)
        ++NumMaskedMultiCyclesTimingPath;

      // Try to verify the result of the timing netlist.
      if (TNL && Interval < 32)
        bindDelay2ScriptEngine(Dst, Src, Interval, IsSimple, !Visited);
    }
  }
}

void PathIntervalQueryCache::bindAllPath2ScriptEngine(VASTSeqValue *Dst) const {
  DenseSet<VASTSeqValue*> BoundSrc;
  DEBUG(dbgs() << "Going to bind delay information of graph: \n");
  DEBUG(dump());
  // Bind the simple paths first, they apply the constraints to all paths
  // between two registers.
  bindAllPath2ScriptEngine(Dst, true, BoundSrc);
  // Then we refine the constraints by applying the constraints with thu nodes.
  bindAllPath2ScriptEngine(Dst, false, BoundSrc);
}

bool TimingScriptGen::runOnVASTModule(VASTModule &VM)  {
  // No need to write timing script at all.
  if (DisableTimingScriptGeneration) return false;

  bindFunctionToScriptEngine(getAnalysis<DataLayout>(), &VM);

  TimingNetlist &TNL = getAnalysis<TimingNetlist>();

  //Write the timing constraints.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I)
    writeConstraintsForDst(I, &TNL);

  return false;
}

void TimingScriptGen::writeConstraintsForDst(VASTSeqValue *Dst,
                                             TimingNetlist *TNL) {
  DenseMap<VASTValue*, SmallVector<VASTSeqUse, 8> > DatapathMap;

  typedef VASTSeqValue::const_iterator vn_itertor;
  for (vn_itertor I = Dst->begin(), E = Dst->end(); I != E; ++I) {
    const VASTSeqUse &DstUse = *I;
    // Paths for the condition.
    DatapathMap[((VASTValPtr)DstUse.getPred()).get()].push_back(DstUse);
    if (VASTValPtr SlotActive = DstUse.getSlotActive())
      DatapathMap[SlotActive.get()].push_back(DstUse);
    // Paths for the assigning value
    DatapathMap[((VASTValPtr)DstUse).get()].push_back(DstUse);
  }

  PathIntervalQueryCache Cache(TNL);
  typedef DenseMap<VASTValue*, SmallVector<VASTSeqUse, 8> >::iterator it;
  for (it I = DatapathMap.begin(), E = DatapathMap.end(); I != E; ++I)
    extractTimingPaths(Cache, I->second, I->first);

  Cache.bindAllPath2ScriptEngine(Dst);
}

void TimingScriptGen::extractTimingPaths(PathIntervalQueryCache &Cache,
                                               ArrayRef<VASTSeqUse> Uses,
                                               VASTValue *DepTree) {
  VASTValue *SrcValue = DepTree;

  // Trivial case: register to register path.
  if (VASTSeqValue *Src = dyn_cast<VASTSeqValue>(SrcValue)){
    // Src may be the return_value of the submodule.
    if (Cache.annotateSubmoduleLatency(Src)) return;

    int Interval = getMinimalInterval(/*ReachingDef,*/ Src, Uses);
    Cache.addIntervalFromToStats(Src, Interval, true);
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

  Cache.annotatePathInterval(/*ReachingDef,*/ DepTree, Uses);
}

char TimingScriptGen::ID = 0;
INITIALIZE_PASS_BEGIN(TimingScriptGen, "CombPathDelayAnalysis",
                      "CombPathDelayAnalysis", false, false)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  //INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables);
INITIALIZE_PASS_END(TimingScriptGen, "CombPathDelayAnalysis",
                    "CombPathDelayAnalysis", false, false)

Pass *llvm::createTimingScriptGenPass() {
  return new TimingScriptGen();
}
