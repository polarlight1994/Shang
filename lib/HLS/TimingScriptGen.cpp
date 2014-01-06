//===----------- TimingScriptGen.cpp- Generate the Timing Scripts - C++ ---===//
//
//                      The VAST HLS frameowrk                                //
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
#include "vast/TimingAnalysis.h"

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
DisableTimingScriptGeneration("vast-disable-timing-script",
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
/// AnnotatedCone - Combinational cone annotated with timing information.
struct AnnotatedCone {
  TimingAnalysis &TA;
  STGDistances &STGDist;
  VASTNode *Node;
  raw_ostream &OS;
  const uint32_t Inf;
  static unsigned ConstrantCounter;

  typedef std::map<VASTSelector*, unsigned> SeqValSetTy;
  SeqValSetTy CyclesFromSrcLB;

  typedef std::map<VASTExpr*, SeqValSetTy> QueryCacheTy;
  QueryCacheTy QueryCache;
  SmallVector<VASTSelector*, 4> Sels;

  AnnotatedCone(TimingAnalysis &TA, STGDistances &STGDist, VASTNode *Node,
                raw_ostream &OS)
    : TA(TA), STGDist(STGDist), Node(Node), OS(OS), Inf(STGDistances::Inf)
  {}

  void addSelector(VASTSelector *Sel) {
    Sels.push_back(Sel);
  }

  void reset() {
    QueryCache.clear();
    CyclesFromSrcLB.clear();
    Sels.clear();
  }

  static void checkIntervalFromSlot(VASTSelector *Leaf, unsigned Cycles) {
    assert((!Leaf->isSlot() || Cycles <= 1) && "Bad interval for slot registers!");
    (void) Cycles;
    (void) Leaf;
  }

  void addIntervalFromSrc(VASTSelector *Leaf, unsigned Cycles) {
    assert(Cycles && "unexpected zero interval!");
    unsigned &OldCycles = CyclesFromSrcLB[Leaf];
    OldCycles = std::min(OldCycles, Cycles);
    checkIntervalFromSlot(Leaf, OldCycles);
  }

  bool generateSubmoduleConstraints(VASTSeqValue *SeqVal);

  void annotatePathInterval(VASTExpr *Root, VASTSelector *Dst,
                            ArrayRef<VASTSlot*> ReadSlots);

  void annotateLeaf(VASTSelector *Leaf, ArrayRef<VASTSlot*> ReadSlots,
                    VASTExpr *Parent);

  // Propagate the timing information of the current combinational cone.
  void propagateInterval(VASTExpr *Expr, ArrayRef<VASTSlot*> ReadSlots) {
    typedef VASTOperandList::op_iterator iterator;
    SeqValSetTy &CurSet = QueryCache[Expr];
    for (iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      VASTExpr *SubExpr = dyn_cast<VASTExpr>(VASTValPtr(*I).get());
      if (SubExpr == NULL)
        continue;

      QueryCacheTy::const_iterator at = QueryCache.find(SubExpr);
      assert(at != QueryCache.end() && "Visiting the expr tree out of order!");

      const SeqValSetTy &Srcs = at->second;
      typedef SeqValSetTy::const_iterator src_iterator;
      for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
        VASTSelector *Leaf = I->first;
        unsigned ChildCycle = I->second;
        assert(ChildCycle && "Unexpected zero delay!");

        // Propagate the cycles from child expression.
        typedef std::pair<SeqValSetTy::iterator, bool> ret_ty;
        ret_ty r = CurSet.insert(*I);

        // Update the exsiting cycles if we cannot insert the new cycle.
        if (!r.second) {
          unsigned &CurCycles = r.first->second;
          CurCycles = std::min(CurCycles, ChildCycle);
        }
      }
    }

    // Now update the number of read cycles according to the read Slots.
    typedef SeqValSetTy::iterator src_iterator;
    for (src_iterator I = CurSet.begin(), E = CurSet.end(); I != E; ++I) {
      VASTSelector *Leaf = I->first;
      unsigned &CurCycles = I->second;
      CurCycles = std::min(CurCycles, STGDist.getIntervalFromDef(Leaf, ReadSlots));
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
                               unsigned Cycles, float Arrival,
                               unsigned Order) const;
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
    AU.addRequiredID(DatapathNamerID);
    AU.addRequired<STGDistances>();
    AU.addRequired<TimingAnalysis>();
    AU.setPreservesAll();
  }

  void writeConstraintsFor(VASTSelector *Sel, TimingAnalysis &TA,
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

void
AnnotatedCone::annotateLeaf(VASTSelector *Leaf, ArrayRef<VASTSlot*> ReadSlots,
                            VASTExpr *Parent) {
  unsigned &Cycles = QueryCache[Parent][Leaf];
  Cycles = std::min(STGDist.getIntervalFromDef(Leaf, ReadSlots), Cycles);
  addIntervalFromSrc(Leaf, Cycles);
}

void AnnotatedCone::annotatePathInterval(VASTExpr *Root, VASTSelector *Dst,
                                         ArrayRef<VASTSlot*> ReadSlots) {
  VASTExpr *RootExpr = dyn_cast<VASTExpr>(Root);

  if (RootExpr == 0)
    return;

  typedef  VASTOperandList::op_iterator ChildIt;

  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;
  // SlotStack, that immediate (annotated) read slots of current expressions.
  std::vector<ArrayRef<VASTSlot*> > SlotStack;
  // Remember the visited node for the current cone.
  std::set<VASTValue*> Visited;

  SlotStack.push_back(ReadSlots);
  VisitStack.push_back(std::make_pair(RootExpr, RootExpr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Expr = VisitStack.back().first;
    ChildIt &It = VisitStack.back().second;

    // All sources of this node is visited.
    if (It == Expr->op_end()) {
      VisitStack.pop_back();

      // Propagate the interval information from the operands of the current
      // value.
      propagateInterval(Expr, SlotStack.back());

      if (VASTSelector::Annotation *Ann = Dst->lookupAnnotation(Expr)) {
        assert(Ann->getSlots() == SlotStack.back() && "Slot stack broken!");
        SlotStack.pop_back();
        (void) Ann;
      }

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++It;

    if (ChildNode == NULL)
     continue;

    // And do not visit a node twice.
    if (!Visited.insert(ChildNode).second)
     continue;

    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(ChildNode)) {
      if (generateSubmoduleConstraints(V))
        continue;

      annotateLeaf(V->getSelector(), SlotStack.back(), Expr);
      continue;
    }  

    VASTExpr *SubExpr = dyn_cast<VASTExpr>(ChildNode);

    if (SubExpr == NULL)
      continue;

    if (VASTSelector::Annotation *Ann = Dst->lookupAnnotation(SubExpr))
      SlotStack.push_back(Ann->getSlots());

    VisitStack.push_back(std::make_pair(SubExpr, SubExpr->op_begin()));
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
      dbgs() << '#' << RI->second << "), ";
    }

    dbgs() << "}\n";
  }
}

unsigned AnnotatedCone::ConstrantCounter = 0;

// The first node of the path is the use node and the last node of the path is
// the define node.
template<typename SrcTy>
void AnnotatedCone::generateMCPWithInterval(SrcTy *Src, const std::string &ThuName,
                                            unsigned Cycles, float Arrival,
                                            unsigned Order) const {
  assert(!ThuName.empty() && "Bad through node name!");
  OS << "INSERT INTO mcps(src, dst, thu, cycles, normalized_delay, constraint_order)"
        "VALUES(\n"
     << '\'' << Src->getSTAObjectName() << "', \n"
     << '\'' << Node->getSTAObjectName() << "', \n"
     << '\'' << ThuName << "', \n"
     << Cycles << ", \n"
     << Arrival << ", \n"
     << Order << ");\n";

  // Perform the Statistic.
  if (Cycles > 1) {
    ++NumMultiCyclesConstraints;
    if (Arrival > 1.0f)
      ++NumRequiredConstraints;
  }

  if (Cycles == AnnotatedCone::Inf) ++NumFalseTimingPath;
  if (Cycles < Arrival) ++NumTimgViolation;
}

unsigned
AnnotatedCone::generateMCPThough(VASTExpr *Thu, const SeqValSetTy &SrcSet) const {
  std::string ThuName = "shang-null-node";
  if (Thu) {
    ThuName = Thu->getSTAObjectName();

    if (ThuName.empty())
      return 0;

    if (!Thu->isHardAnnotation())
      return 0;
  }

  typedef SeqValSetTy::const_iterator iterator;
  for (iterator I = SrcSet.begin(), E = SrcSet.end(); I != E; ++I) {
    // TODO: Extract the arrival time.
    float Arrival = I->second;
    generateMCPWithInterval(I->first, ThuName, I->second, Arrival, ++ConstrantCounter);
  }

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
  if (!SeqVal->isFUOutput())
    return false;

  VASTSubModule *SubMod = dyn_cast<VASTSubModule>(SeqVal->getParent());
  if (SubMod == NULL)
    return false;

  unsigned Latency = SubMod->getLatency();
  // No latency information available.
  if (Latency == 0)
    return false;

  // Add the timing constraints from operand registers to the output registers.
  typedef VASTSubModule::fanin_iterator fanin_iterator;
  for (fanin_iterator I = SubMod->fanin_begin(), E = SubMod->fanin_end();
       I != E; ++I) {
    VASTSelector *Operand = *I;
    float Arrival = Latency;
    generateMCPWithInterval(Operand, "shang-null-node", Latency, Arrival,
                            ++ConstrantCounter);
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
  TimingAnalysis &TA =getAnalysis<TimingAnalysis>();

  std::map<VASTMemoryBank*, AnnotatedCone*> MemBankCones;

  //Write the timing constraints.
  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;

    if (Sel->empty())
      continue;

    // Handle the trivial case.
    if (isa<VASTRegister>(Sel->getParent()) ||
        isa<VASTOutPort>(Sel->getParent())) {
      writeConstraintsFor(Sel, TA, STGDist);
      continue;
    }

    VASTMemoryBank *Bank = cast<VASTMemoryBank>(Sel->getParent());
    // Input ports of memory bank are pipelined if the read latency of the memory
    // bank is bigger than 1.
    if (Bank->getReadLatency() > 1) {
      writeConstraintsFor(Sel, TA, STGDist);
      continue;
    }

    // Otherwise we can only match them with the underlying memory bank instead
    // the input port themselves.
    AnnotatedCone *&Cone = MemBankCones[Bank];

    if (Cone == NULL)
      Cone = new AnnotatedCone(TA, STGDist, Bank, OS);

    annoataConstraintsFor(*Cone, Sel);
  }

  // Generate the constraints for memory bus and release the cone for memory bus.
  typedef std::map<VASTMemoryBank*, AnnotatedCone*>::iterator iteator;
  for (iteator I = MemBankCones.begin(), E = MemBankCones.end(); I != E; ++I) {
    I->second->generateMCPEntries();
    delete I->second;
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
TimingScriptGen::writeConstraintsFor(VASTSelector *Dst, TimingAnalysis &TA,
                                     STGDistances &STGDist) {
  AnnotatedCone Cache(TA, STGDist, Dst, OS);

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
  extractTimingPaths(Cache, Sel, AllSlots, Sel->getGuard().get());
  extractTimingPaths(Cache, Sel, AllSlots, Sel->getFanin().get());

  Cache.addSelector(Sel);
}

void TimingScriptGen::extractTimingPaths(AnnotatedCone &Cache, VASTSelector *Dst,
                                         ArrayRef<VASTSlot*> ReadSlots,
                                         VASTValue *DepTree) {
  // Trivial case: register to register path.
  if (VASTSeqValue *Src = dyn_cast<VASTSeqValue>(DepTree)){
    if (Cache.generateSubmoduleConstraints(Src)) return;

    VASTSelector *Leaf = Src->getSelector();
    unsigned NumCycles = Cache.STGDist.getIntervalFromDef(Leaf, ReadSlots);
    Cache.addIntervalFromSrc(Leaf, NumCycles);

    // Even a trivial path can be a false path, e.g.:
    // slot 1:
    // reg_a <= c + x;
    // slot 2:
    // reg_a <= reg_b
    // For DstAVS = reg_a@1, there are no timing path from reg_b.
    return;
  }

  VASTExpr *Expr = dyn_cast<VASTExpr>(DepTree);

  // If Value is a constant, just skip it.
  if (Expr == NULL)
    return;

  Cache.annotatePathInterval(Expr, Dst, ReadSlots);
}

char TimingScriptGen::ID = 0;

INITIALIZE_PASS_BEGIN(TimingScriptGen, "vast-timing-script-generation",
                      "Generate timing script to export the behavior-level timing",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
  INITIALIZE_AG_DEPENDENCY(TimingAnalysis)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
INITIALIZE_PASS_END(TimingScriptGen, "vast-timing-script-generation",
                    "Generate timing script to export the behavior-level timing",
                    false, true)

Pass *vast::createTimingScriptGenPass() {
  return new TimingScriptGen();
}
