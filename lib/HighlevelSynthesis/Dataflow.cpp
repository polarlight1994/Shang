//===------- Dataflow.cpp - Dataflow Analysis on LLVM IR --------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface of Dataflow Analysis. The dataflow analysis
// build the flow dependencies on LLVM IR.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "TimingNetlist.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/STGDistances.h"

#include "llvm/Analysis/Dominators.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#define DEBUG_TYPE "shang-dataflow"
#include "llvm/Support/Debug.h"

using namespace llvm;

Dataflow::Dataflow() : FunctionPass(ID), generation(0) {
  initializeDataflowPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(Dataflow,
                      "vast-dataflow", "Dataflow Anlaysis", false, true)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
INITIALIZE_PASS_END(Dataflow,
                    "vast-dataflow", "Dataflow Anlaysis", false, true)

char Dataflow::ID = 0;
char &llvm::DataflowID = Dataflow::ID;

void Dataflow::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTree>();
  AU.setPreservesAll();
}

void Dataflow::releaseMemory() {
  FlowDeps.clear();
  Incomings.clear();
  generation = 0;
}

bool Dataflow::runOnFunction(Function &F) {
  DT = &getAnalysis<DominatorTree>();
  return false;
}

void Dataflow::getFlowDep(DataflowInst Inst, SrcSet &Set) const {
  FlowDepMapTy::const_iterator I = FlowDeps.find(Inst);
  if (I == FlowDeps.end()) {
    //assert(isa<TerminatorInst>(Inst) && "Flow dependencies do not exists?");
    return;
  }

  const TimedSrcSet &Srcs = I->second;

  typedef TimedSrcSet::const_iterator iterator;
  for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    float &Delay = Set[I->first];
    Delay = std::max(I->second.first, Delay);
  }
}

void
Dataflow::getIncomingFrom(DataflowInst Inst, BasicBlock *BB, SrcSet &Set) const {
  IncomingMapTy::const_iterator I = Incomings.find(Inst);

  if (I == Incomings.end()) {
    assert((!isa<PHINode>(Inst) ||
            isa<Constant>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB)) ||
            isa<GlobalVariable>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB))) &&
           "Incoming value dose not existed?");
    return;
  }

  IncomingBBMapTy::const_iterator J = I->second.find(BB);
  if (J == I->second.end()) {
    //assert((!isa<PHINode>(Inst) ||
    //        isa<Constant>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB)) ||
    //        isa<GlobalVariable>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB))) &&
    //       "Incoming value dose not existed?");
    return;
  }

  const TimedSrcSet &Srcs = J->second;

  typedef TimedSrcSet::const_iterator iterator;
  for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    float &Delay = Set[I->first];
    Delay = std::max(I->second.first, Delay);
  }
}

Dataflow::TimedSrcSet &Dataflow::getDeps(DataflowInst Inst, BasicBlock *Parent) {
  if (!isa<PHINode>(Inst) && Inst->getParent() == Parent)
    return FlowDeps[Inst];

  return Incomings[Inst][Parent];
}

float Dataflow::getSlackFromLaunch(Instruction *Inst) const {
  if (!Inst) return 0;

  FlowDepMapTy::const_iterator I = FlowDeps.find(DataflowInst(Inst, false));
  if (I == FlowDeps.end())
    return 0.0f;

  const TimedSrcSet &Srcs = I->second;
  TimedSrcSet::const_iterator J = Srcs.find(DataflowValue(Inst, true));
  if (J == Srcs.end())
    return 0.0f;

  return (1.0f - J->second.first);
}

float Dataflow::annotateDelay(DataflowInst Inst, VASTSlot *S, DataflowValue V,
                              float delay, unsigned Slack) {
  assert(V && "Unexpected VASTSeqValue without underlying llvm Value!");
  BasicBlock *ParentBB = S->getParent();
  bool IsTimingVoilation = Slack < delay && generation != 0;

  // Adjust to actual parent BB for the incoming value.
  if (isa<PHINode>(Inst) || isa<BranchInst>(Inst) || isa<SwitchInst>(Inst)) {
    S = S->getParentGroup();
    if (BasicBlock *BB = S->getParent())
      ParentBB = BB;
  }

  assert((ParentBB == Inst->getParent() || isa<PHINode>(Inst)) &&
         "Parent not match!");

  if (Instruction *Src = dyn_cast<Instruction>(V)) {
    // While Src not dominate BB, this is due to CFG folding. We need to get the
    // parent BB of the actual user, this can be done by move up in the subgroup
    // tree until we get a BB that is dominated by Src.
    while (!DT->dominates(Src->getParent(), ParentBB)) {
      S = S->getParentGroup();
      ParentBB = S->getParent();
    }
  }

  TimedSrcSet &Srcs = getDeps(Inst, ParentBB);

  // Assign the current delay a bigger weigth if there is timing violation. So
  // that the scheduler can make quick respond on the timing violation.
  float Ratio = IsTimingVoilation ? 0.9f : 0.5f;

  float OldDelay = updateDelay(delay, Ratio, Srcs[V]);
  if (IsTimingVoilation) {
    dbgs() << "Potential timing violation: "<< Slack << ' ' << delay
           << " Old delay " << OldDelay
           << '(' << ((delay - OldDelay) / delay) << ')' << " \n";
  }

  return OldDelay;
}

float Dataflow::updateDelay(float NewDelay, float Ratio,
                            std::pair<float, unsigned> &OldDelay) {
  float tmp = OldDelay.first;
  if (OldDelay.second == generation) {
    OldDelay.first = std::max(NewDelay, OldDelay.first);
    return tmp;
  }

  OldDelay.second = generation;
  OldDelay.first = OldDelay.first * (1.0f - Ratio) + NewDelay * Ratio;
  return tmp;
}

DataflowAnnotation::DataflowAnnotation(bool Accumulative)
  : VASTModulePass(ID), Accumulative(Accumulative) {
  initializeDataflowPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(DataflowAnnotation,
                      "vast-dataflow-annotation", "Dataflow Annotation",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(Dataflow)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(SelectorSynthesis)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
INITIALIZE_PASS_END(DataflowAnnotation,
                    "vast-dataflow-annotation", "Dataflow Annotation",
                    false, true)

char DataflowAnnotation::ID = 0;

void DataflowAnnotation::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<Dataflow>();

  AU.addRequiredID(ControlLogicSynthesisID);
  AU.addRequiredID(DatapathNamerID);

  AU.addRequiredID(STGDistancesID);
  AU.addPreservedID(STGDistancesID);
  AU.addRequired<TimingNetlist>();
  AU.setPreservesAll();
}

float DataflowAnnotation::annotateDelay(DataflowInst Inst, VASTSlot *S,
                                        VASTSeqValue *SV, float delay) {
  DataflowValue V(SV->getLLVMValue(), SV->isFUOutput() || SV->isFUInput());
  unsigned Slack = Distances->getIntervalFromDef(SV, S);
  float OldDelay = DF->annotateDelay(Inst, S, V, delay, Slack);
  return OldDelay;
}

void DataflowAnnotation::extractFlowDep(VASTSeqOp *Op, TimingNetlist &TNL) {
  Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());

  // Nothing to do if Op does not have an underlying instruction.
  if (!Inst)
    return;

  bool IsLaunch = false;
  if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op))
    IsLaunch = SeqInst->isLaunch();

  std::map<VASTSeqValue*, float> Srcs;

  VASTValPtr Cnd = Op->getGuard();
  TNL.extractDelay(0, Cnd.get(), Srcs);

  for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
    VASTLatch L = Op->getSrc(i);
    VASTSelector *Sel = L.getSelector();
    if (Sel->isTrivialFannin(L))
      continue;

    VASTValPtr FI = L;

    // Extract the delay from the fan-in and the guarding condition.
    TNL.extractDelay(Sel, FI.get(), Srcs);
    TNL.extractDelay(Sel, Cnd.get(), Srcs);
  }

  typedef std::map<VASTSeqValue*, float>::iterator src_iterator;
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    VASTSeqValue *Src = I->first;
    float delay = I->second;
    annotateDelay(DataflowInst(Inst, IsLaunch), Op->getSlot(), Src, delay);
  }
}

void DataflowAnnotation::internalDelayAnnotation(VASTModule &VM) {
  TimingNetlist &TNL = getAnalysis<TimingNetlist>();

  typedef VASTModule::seqop_iterator iterator;
  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I)
    extractFlowDep(I, TNL);
}

bool DataflowAnnotation::runOnVASTModule(VASTModule &VM) {
  DF = &getAnalysis<Dataflow>();
  Distances = &getAnalysis<STGDistances>();

  // Force release the context if the annotatio is not accumulative.
  if (!Accumulative)
    DF->releaseMemory();

  if (DF->getGeneration() == 0)
    internalDelayAnnotation(VM);
  else
    externalDelayAnnotation(VM);

  DF->increaseGeneration();

  DEBUG(DF->dumpToSQL());

  return false;
}

void Dataflow::dumpToSQL() const {
  SmallString<256> SQLPath
    = sys::path::parent_path(getStrValueFromEngine("RTLOutput"));
  sys::path::append(SQLPath, "delay_annotation.sql");

  std::string Error;
  raw_fd_ostream Output(SQLPath.c_str(), Error);
  dumpFlowDeps(Output);
  dumpIncomings(Output);
}

void Dataflow::dumpFlowDeps(raw_ostream &OS) const {
  OS << "CREATE TABLE flowdeps( \
        id INTEGER PRIMARY KEY AUTOINCREMENT, \
        src TEXT, \
        dst TEXT, \
        generation INTEGER, \
        delay REAL \
        );\n";

  typedef FlowDepMapTy::const_iterator iterator;
  typedef TimedSrcSet::const_iterator src_iterator;
  for (iterator I = FlowDeps.begin(), E = FlowDeps.end(); I != E; ++I) {
    DataflowInst Dst = I->first;
    const TimedSrcSet &Srcs = I->second;
    for (src_iterator J = Srcs.begin(), E = Srcs.end(); J != E; ++J) {
      OS << "INSERT INTO flowdeps(src, dst, generation, delay) VALUES(\n"
         << '\'' << *J->first << "', \n"
         << '\'' << *Dst << "', \n"
         << '\'' << J->second.second << "', \n"
         << J->second.first << ");\n";
    }
  }
}

void Dataflow::dumpIncomings(raw_ostream &OS) const {
  OS << "CREATE TABLE incomings( \
        id INTEGER PRIMARY KEY AUTOINCREMENT, \
        src TEXT, \
        bb TEXT, \
        dst TEXT, \
        generation INTEGER, \
        delay REAL \
        );\n";

  typedef IncomingMapTy::const_iterator iterator;
  typedef IncomingBBMapTy::const_iterator bb_iterator;
  typedef TimedSrcSet::const_iterator src_iterator;
  for (iterator I = Incomings.begin(), E = Incomings.end(); I != E; ++I) {
    DataflowInst Dst = I->first;
    const std::map<BasicBlock*, TimedSrcSet> &BBs = I->second;
    for (bb_iterator J = BBs.begin(), E = BBs.end(); J != E; ++J) {
      BasicBlock *BB = J->first;
      const TimedSrcSet &Srcs = J->second;
      for (src_iterator K = Srcs.begin(), E = Srcs.end(); K != E; ++K) {
        OS << "INSERT INTO incomings(src, bb, dst, generation, delay) VALUES(\n"
           << '\'' << *K->first << "', \n"
           << '\'' << BB->getName() << "', \n"
           << '\'' << *Dst << "', \n"
           << '\'' << K->second.second << "', \n"
           << K->second.first << ");\n";
      }
    }
  }
}
