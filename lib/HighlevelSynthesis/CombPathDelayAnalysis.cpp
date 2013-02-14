//CombPathDelayAnalysis.cpp- Analysis the Path delay between registers- C++ -=//
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

#include "SeqReachingDefAnalysis.h"

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
#define DEBUG_TYPE "vtm-comb-path-delay"
#include "llvm/Support/Debug.h"
using namespace llvm;

static cl::opt<bool>
DisableTimingScriptGeneration("vtm-disable-timing-script",
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
struct CombPathDelayAnalysis;

struct PathDelayQueryCache {
  typedef std::map<unsigned, DenseSet<VASTSeqValue*> > DelayStatsMapTy;
  typedef DenseMap<VASTSeqValue*, unsigned> SeqValSetTy;
  typedef DenseMap<VASTValue*, SeqValSetTy> QueryCacheTy;
  QueryCacheTy QueryCache;
  // Statistics for simple path and complex paths.
  DelayStatsMapTy Stats[2];

  void reset() {
    QueryCache.clear();
    Stats[0].clear();
    Stats[1].clear();
  }

  void addDelayFromToStats(VASTSeqValue *Src, unsigned Delay, bool isSimple) {
    Stats[isSimple][Delay].insert(Src);
  }

  bool annotateSubmoduleLatency(VASTSeqValue * V);

  void annotatePathDelay(SeqReachingDefAnalysis *R, VASTValue *Tree,
                         ArrayRef<VASTSeqUse> DstUses);

  bool updateDelay(SeqValSetTy &To, const SeqValSetTy &From,
                   const SeqValSetTy &LocalDelayMap) {
    bool Changed = false;

    typedef SeqValSetTy::const_iterator it;
    for (it I = From.begin(), E = From.end(); I != E; ++I) {
      assert(I->second && "Unexpected zero delay!");

      unsigned &ExistDelay = To[I->first];
      // Look up the delay from local delay map, because the delay of the From
      // map may correspond to another data-path with different destination.
      SeqValSetTy::const_iterator at = LocalDelayMap.find(I->first);
      assert(at != LocalDelayMap.end() && "Node not visited yet?");
      if (ExistDelay == 0 || ExistDelay > at->second) {
        ExistDelay = at->second;
        Changed |= true;
      }
    }

    return Changed;
  }

  void bindAllPath2ScriptEngine(VASTSeqValue *Dst) const;
  void bindAllPath2ScriptEngine(VASTSeqValue *Dst, bool IsSimple,
                                DenseSet<VASTSeqValue*> &BoundSrc) const;
  unsigned bindPath2ScriptEngine(VASTSeqValue *Dst, VASTSeqValue *Src,
                                 unsigned Delay, bool SkipThu, bool IsCritical)
                                 const;
  unsigned printPathWithDelayFrom(raw_ostream &OS, VASTSeqValue *Src,
                                  unsigned Delay) const;

  typedef DenseSet<VASTSeqValue*>::const_iterator src_it;
  typedef DelayStatsMapTy::const_iterator delay_it;
  delay_it stats_begin(bool IsSimple) const { return Stats[IsSimple].begin(); }
  delay_it stats_end(bool IsSimple) const { return Stats[IsSimple].end(); }

  void dump() const;
};

struct CombPathDelayAnalysis : public VASTModulePass {
  SeqReachingDefAnalysis *ReachingDef;
  VASTModule *VM;

  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<SeqReachingDefAnalysis>();
    AU.setPreservesAll();
  }

  void writeConstraintsForDst(VASTSeqValue *Dst);

  void extractTimingPaths(PathDelayQueryCache &Cache, ArrayRef<VASTSeqUse> Uses,
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

  CombPathDelayAnalysis() : VASTModulePass(ID), ReachingDef(0), VM(0) {
    initializeCombPathDelayAnalysisPass(*PassRegistry::getPassRegistry());
  }
};
}

static unsigned getMinimalDelay(SeqReachingDefAnalysis *R, VASTSeqValue *Src,
                                const VASTSeqUse &Dst) {
  unsigned PathDelay = 10000;
  SlotInfo *DstSI = R->getSlotInfo(Dst.getSlot());

  typedef VASTSeqValue::const_itertor vn_itertor;
  for (vn_itertor I = Src->begin(), E = Src->end(); I != E; ++I) {
    VASTSeqUse Def = *I;

    // Update the PathDelay if the source VAS reaches DstSlot.
    if (unsigned Distance = DstSI->getCyclesFromDef(Def)) {
      assert(Distance < 10000 && "Distance too large!");
      PathDelay = std::min(PathDelay, Distance);
    }
  }

  return PathDelay;
}

static unsigned getMinimalDelay(SeqReachingDefAnalysis *R, VASTSeqValue *Src,
                                ArrayRef<VASTSeqUse> DstUses) {
  unsigned PathDelay = 10000;
  typedef ArrayRef<VASTSeqUse>::iterator iterator;
  for (iterator I = DstUses.begin(), E = DstUses.end(); I != E; ++I)
    PathDelay = std::min(PathDelay, getMinimalDelay(R, Src, *I));

  return PathDelay;
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

bool PathDelayQueryCache::annotateSubmoduleLatency(VASTSeqValue * V) {
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
    addDelayFromToStats(Operand, Latency, true);
  }

  return true;
}

void PathDelayQueryCache::annotatePathDelay(SeqReachingDefAnalysis *R,
                                            VASTValue *Root,
                                            ArrayRef<VASTSeqUse> DstUses) {
  assert((isa<VASTWire>(Root) || isa<VASTExpr>(Root)) && "Bad root type!");
  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;
  std::set<VASTValue*> Visited;
  SeqValSetTy LocalDelay;

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
        updateDelay(QueryCache[VisitStack.back().first], ParentReachableRegs,
                    LocalDelay);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->getAsLValue<VASTValue>();
    ++VisitStack.back().second;

    // And do not visit a node twice.
    if (!Visited.insert(ChildNode).second) {
      // If there are tighter delay from the child, it means we had already
      // visited the sub-tree.
      updateDelay(ParentReachableRegs, QueryCache[ChildNode], LocalDelay);
      continue;
    }

    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(ChildNode)) {
      unsigned Delay = getMinimalDelay(R, V, DstUses);

      bool inserted = LocalDelay.insert(std::make_pair(V, Delay)).second;
      assert(inserted && "Node had already been visited?");
      unsigned &ExistedDelay = ParentReachableRegs[V];
      if (ExistedDelay == 0 || Delay < ExistedDelay)
        ExistedDelay = Delay;

      // Add the information to statistics.
      addDelayFromToStats(V, Delay, false);
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
  bool DelayMasked = false;
  for (it I = LocalDelay.begin(), E = LocalDelay.end(); I != E; ++I) {
    SeqValSetTy::const_iterator ActualDelayAt = RootSet.find(I->first);
    assert(ActualDelayAt != RootSet.end() && "Timing path entire missed!");
    assert(ActualDelayAt->second <= I->second
           && "Delay information not applied?");
    if (ActualDelayAt->second == I->second) continue;

    dbgs() << "Timing path masked: Root is";
    Root->printAsOperand(dbgs(), false);
    dbgs() << " end node is " << I->first->getName()
           << " masked delay: " << I->second
           << " actual delay: " << ActualDelayAt->second << '\n';
    DelayMasked = true;
  }

  if (DelayMasked) {
    dbgs() << " going to dump the nodes in the tree:\n";

    typedef std::set<VASTValue*>::iterator node_it;
    for (node_it NI = Visited.begin(), NE = Visited.end(); NI != NE; ++NI) {
      (*NI)->printAsOperand(dbgs(), false);
      dbgs() << ", ";
    }
    dbgs() << '\n';
  });
}

unsigned PathDelayQueryCache::printPathWithDelayFrom(raw_ostream &OS,
                                                     VASTSeqValue *Src,
                                                     unsigned Delay) const {
  unsigned NumNodesPrinted = 0;

  typedef QueryCacheTy::const_iterator it;
  for (it I = QueryCache.begin(), E = QueryCache.end(); I != E; ++I) {
    const SeqValSetTy &Set = I->second;
    SeqValSetTy::const_iterator at = Set.find(Src);
    // The register may be not reachable from this node.
    if (at == Set.end() || at->second != Delay) continue;

    if (printBindingLuaCode(OS, I->first)) {
      OS << ", ";
      ++NumNodesPrinted;
    }
  }

  return NumNodesPrinted;
}

void PathDelayQueryCache::dump() const {
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

// The first node of the path is the use node and the last node of the path is
// the define node.
unsigned PathDelayQueryCache::bindPath2ScriptEngine(VASTSeqValue *Dst,
                                                    VASTSeqValue *Src,
                                                    unsigned Delay, bool SkipThu,
                                                    bool IsCritical) const {
  // Path table:
  // Datapath: {
  //  unsigned Slack,
  //  table NodesInPath
  // }
  SMDiagnostic Err;

  if (!runScriptStr("RTLDatapath = {}\n", Err))
    llvm_unreachable("Cannot create RTLDatapath table in scripting pass!");

  std::string Script;
  raw_string_ostream SS(Script);
  SS << "RTLDatapath.Slack = " << Delay << '\n';
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
    NumThuNodePrinted = printPathWithDelayFrom(SS, Src, Delay);

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
PathDelayQueryCache::bindAllPath2ScriptEngine(VASTSeqValue *Dst, bool IsSimple,
                                              DenseSet<VASTSeqValue*> &BoundSrc)
                                              const {
  DEBUG(dbgs() << "Binding path for dst register: "
               << Dst->getName() << '\n');
  for (delay_it I = stats_begin(IsSimple), E = stats_end(IsSimple);I != E;++I) {
    unsigned Delay = I->first;

    for (src_it SI = I->second.begin(), SE = I->second.end(); SI != SE; ++SI) {
      VASTSeqValue *Src = *SI;
      bool Visited = !BoundSrc.insert(Src).second;
      assert((!IsSimple || !Visited)
             && "A simple path should not have been visited!");
      ++NumTimingPath;
      if (Delay == 10000) ++NumFalseTimingPath;
      else if (Delay > 1) ++NumMultiCyclesTimingPath;

      DEBUG(dbgs().indent(2) << "from: " << Src->getName() << '#'
                             << I->first << '\n');
      // If we not visited the path before, this path is the critical path,
      // since we are iteration the path from the smallest delay to biggest
      // delay.
      unsigned NumConstraints = bindPath2ScriptEngine(Dst, Src, Delay,
                                                      IsSimple, !Visited);
      if (NumConstraints == 0 && !IsSimple && Delay > 1)
        ++NumMaskedMultiCyclesTimingPath;
    }
  }
}

void PathDelayQueryCache::bindAllPath2ScriptEngine(VASTSeqValue *Dst) const {
  DenseSet<VASTSeqValue*> BoundSrc;
  DEBUG(dbgs() << "Going to bind delay information of graph: \n");
  DEBUG(dump());
  // Bind the simple paths first, which are the most general.
  bindAllPath2ScriptEngine(Dst, true, BoundSrc);
  bindAllPath2ScriptEngine(Dst, false, BoundSrc);
}

bool CombPathDelayAnalysis::runOnVASTModule(VASTModule &VM)  {
  // No need to write timing script at all.
  if (DisableTimingScriptGeneration) return false;

  bindFunctionToScriptEngine(getAnalysis<DataLayout>(), &VM);

  ReachingDef = &getAnalysis<SeqReachingDefAnalysis>();

  //Write the timing constraints.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I)
    writeConstraintsForDst(I);

  return false;
}

void CombPathDelayAnalysis::writeConstraintsForDst(VASTSeqValue *Dst) {
  DenseMap<VASTValue*, SmallVector<VASTSeqUse, 8> > DatapathMap;

  typedef VASTSeqValue::const_itertor vn_itertor;
  for (vn_itertor I = Dst->begin(), E = Dst->end(); I != E; ++I) {
    const VASTSeqUse &DstUse = *I;
    // Paths for the condition.
    DatapathMap[((VASTValPtr)DstUse.getPred()).get()].push_back(DstUse);
    if (VASTValPtr SlotActive = DstUse.getSlotActive())
      DatapathMap[SlotActive.get()].push_back(DstUse);
    // Paths for the assigning value
    DatapathMap[((VASTValPtr)DstUse).get()].push_back(DstUse);
  }

  PathDelayQueryCache Cache;
  typedef DenseMap<VASTValue*, SmallVector<VASTSeqUse, 8> >::iterator it;
  for (it I = DatapathMap.begin(), E = DatapathMap.end(); I != E; ++I)
    extractTimingPaths(Cache, I->second, I->first);

  Cache.bindAllPath2ScriptEngine(Dst);
}

void CombPathDelayAnalysis::extractTimingPaths(PathDelayQueryCache &Cache,
                                               ArrayRef<VASTSeqUse> Uses,
                                               VASTValue *DepTree) {
  VASTValue *SrcValue = DepTree;

  // Trivial case: register to register path.
  if (VASTSeqValue *Src = dyn_cast<VASTSeqValue>(SrcValue)){
    // Src may be the return_value of the submodule.
    if (Cache.annotateSubmoduleLatency(Src)) return;

    int Delay = getMinimalDelay(ReachingDef, Src, Uses);
    Cache.addDelayFromToStats(Src, Delay, true);
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

  Cache.annotatePathDelay(ReachingDef, DepTree, Uses);
}

char CombPathDelayAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(CombPathDelayAnalysis, "CombPathDelayAnalysis",
                      "CombPathDelayAnalysis", false, false)
  INITIALIZE_PASS_DEPENDENCY(SeqReachingDefAnalysis);
INITIALIZE_PASS_END(CombPathDelayAnalysis, "CombPathDelayAnalysis",
                    "CombPathDelayAnalysis", false, false)

Pass *llvm::createCombPathDelayAnalysisPass() {
  return new CombPathDelayAnalysis();
}
