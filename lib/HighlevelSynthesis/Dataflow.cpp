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
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-dataflow"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<float>
  SignmaRatio("vast-back-annotation-sigma-ratio",
  cl::desc("Number of signma use to calculate the next delay from back annotation"),
  cl::init(0.0f));

Dataflow::BBPtr::BBPtr(VASTSlot *S) : Base(S->getParent(), S->IsSubGrp) {}

Dataflow::Dataflow() : FunctionPass(ID), generation(0) {
  initializeDataflowPass(*PassRegistry::getPassRegistry());
}

float Dataflow::Annotation::calculateDelay() const {
  float num_samples = float(this->num_samples);
  // Caculate the expected value
  float E = sum / num_samples;
  // Caculate the variance
  float D2 = sqr_sum / num_samples - E * E;
  // Do not fail due on the errors in floating point operation.
  float D = sqrtf(std::max<float>(D2, 0.0f));

  // Assume the data follow the Normal distribution.
  float ratio = SignmaRatio;
  float delay = std::max(E + ratio * D, 0.0f);
  assert(delay >= 0.0f && "Unexpected negative delay!");
  return delay;
}

void Dataflow::Annotation::addSample(float d) {
  num_samples += 1;
  sum += d;
  sqr_sum += d * d;
}

void Dataflow::Annotation::reset() {
  sum = sqr_sum = 0.0f;
  num_samples = 0;
}

void Dataflow::Annotation::dump() const {
  float num_samples = float(this->num_samples);
  // Caculate the expected value
  float E = sum / num_samples;
  // Caculate the variance
  float D2 = sqr_sum / num_samples - E * E;
  // Do not fail due on the errors in floating point operation.
  float D = sqrtf(std::max<float>(D2, 0.0f));

  dbgs() << "E: " << E << " D: " << D
         << "\t\nE + D: " << E + D
         << "\t\nE + 2D: " << E + 2 * D
         << "\t\nE + 3D: " << E + 3 * D
         << "\t\nratio: " << float(violation) / float(generation + 1) << '\n';
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
  typedef TimedSrcSet::const_iterator iterator;

  if (I != FlowDeps.end()) {
    const TimedSrcSet &Srcs = I->second;

    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      float &Delay = Set[I->first];
      Delay = std::max(I->second.calculateDelay(), Delay);
    }
  }

  // Also collect the dependence from incoming block, if the dependence's parent
  // basic block *prperly* dominates Inst's parent basic block.
  IncomingMapTy::const_iterator J = Incomings.find(Inst);
  if (J == Incomings.end())
    return;

  typedef IncomingBBMapTy::const_iterator incoming_iterator;
  const IncomingBBMapTy &IncomingDeps = J->second;
  for (incoming_iterator I = IncomingDeps.begin(), E = IncomingDeps.end();
       I != E; ++I) {
    const TimedSrcSet &Srcs = I->second;
    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      DataflowValue V = I->first;
      // Ignore the non-dominance edges.
      if (Instruction *Def = dyn_cast<Instruction>(V))
        if (!DT->properlyDominates(Def->getParent(), Inst->getParent()))
          continue;

      if (BasicBlock *BB = dyn_cast<BasicBlock>(V))
        if (!DT->properlyDominates(BB, Inst->getParent()))
          continue;

      float &Delay = Set[V];
      Delay = std::max(I->second.calculateDelay(), Delay);
    }
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
    Delay = std::max(I->second.calculateDelay(), Delay);
  }
}

Dataflow::TimedSrcSet &Dataflow::getDeps(DataflowInst Inst, BBPtr Parent) {
  if (Inst->getParent() == Parent && !Parent.isSubGrp()) {
    assert(!isa<PHINode>(Inst) && "Unexpected PHI!");
    return FlowDeps[Inst];
  }

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

  return (1.0f - J->second.calculateDelay());
}

float Dataflow::getDelayFromLaunch(Instruction *Inst) const {
  if (!Inst) return 0;

  FlowDepMapTy::const_iterator I = FlowDeps.find(DataflowInst(Inst, false));
  if (I == FlowDeps.end())
    return 0.0f;

  const TimedSrcSet &Srcs = I->second;
  TimedSrcSet::const_iterator J = Srcs.find(DataflowValue(Inst, true));
  if (J == Srcs.end())
    return 0.0f;

  return J->second.calculateDelay();
}

float Dataflow::getDelay(DataflowValue Src, DataflowInst Dst, VASTSlot *S) const {
  BasicBlock *BB = getIncomingBlock(S, Dst, Src);

  if (!isa<PHINode>(Dst) && Dst->getParent() == BB) {
    FlowDepMapTy::const_iterator I = FlowDeps.find(Dst);

    // The scheduler may created new path via CFG folding, do not fail in this
    // case.
    assert(I != FlowDeps.end() && "Dst dose not existed in flow dependence?");

    const TimedSrcSet &Srcs = I->second;

    TimedSrcSet::const_iterator J = Srcs.find(Src);
    return J == Srcs.end() ? 0.0f : J->second.calculateDelay();
  }

  IncomingMapTy::const_iterator I = Incomings.find(Dst);

  // The scheduler may created new path via CFG folding, do not fail in this
  // case.
  if (I == Incomings.end())
    return 0.0f;

  IncomingBBMapTy::const_iterator J = I->second.find(BB);
  if (J == I->second.end())
    return 0.0f;

  const TimedSrcSet &Srcs = J->second;
  TimedSrcSet::const_iterator K = Srcs.find(Src);
  return K == Srcs.end() ? 0.0f : K->second.calculateDelay();
}

Dataflow::BBPtr
Dataflow::getIncomingBlock(VASTSlot *S, Instruction *Inst, Value *Src) const {
  VASTSlot *CurSlot = S;
  BasicBlock *ParentBB = CurSlot->getParent();
  // Use the IsSubGrp flag to identify the intra-bb backedge like:
  // BB:
  //   A <-
  //       |
  //   B --
  // In the above example, there is an back-edge from B to A, due to CFG folding
  // although src and dst of the edge are located in the same BB, but such
  // dependence is not a flow dependence, thereby we should not put such
  // dependence to the FlowDep set..
  bool IsSubGrp = CurSlot->IsSubGrp;

  // Adjust to actual parent BB for the incoming value.
  if (CurSlot->IsSubGrp) {
    CurSlot = CurSlot->getParentGroup();
    if (BasicBlock *BB = CurSlot->getParent())
      ParentBB = BB;
  }

  BasicBlock *DefBB = 0;
  if (Instruction *Def = dyn_cast<Instruction>(Src))
    DefBB = Def->getParent();
  else if (BasicBlock *BB = dyn_cast<BasicBlock>(Src))
    DefBB = BB;

  if (DefBB) {
    // While Src not dominate BB, this is due to CFG folding. We need to get the
    // parent BB of the actual user, this can be done by move up in the subgroup
    // tree until we get a BB that is dominated by Src.
    while (!DT->dominates(DefBB, ParentBB)) {
      CurSlot = CurSlot->getParentGroup();
      ParentBB = CurSlot->getParent();
    }
  }

  // Since terminators are always the last instruction, there wil never be a
  // intra-BB back-edge to terminators.
  if (isa<TerminatorInst>(Inst)) {
    IsSubGrp &= ParentBB != Inst->getParent();

    if ((isa<BranchInst>(Inst) || isa<SwitchInst>(Inst)) && !IsSubGrp) {
      IsSubGrp = true;
      ParentBB = S->getParent();
    }
  }

  return BBPtr(ParentBB, IsSubGrp);
}

void Dataflow::annotateDelay(DataflowInst Inst, VASTSlot *S, DataflowValue V,
                             float delay, unsigned Slack) {
  bool IsTimingViolation = Slack < delay && generation != 0;
  assert(V && "Unexpected VASTSeqValue without underlying llvm Value!");

  TimedSrcSet &Srcs = getDeps(Inst, getIncomingBlock(S, Inst, V));

  Annotation &OldAnnotation = Srcs[V];

  if (IsTimingViolation) {
    if (OldAnnotation.generation != generation)
      ++OldAnnotation.violation;

    // We cannot do anything with the BRAM to register path ...
    DEBUG(if (OldAnnotation.generation == 1 && !isa<BasicBlock>(V)
              && !(isa<LoadInst>(V) && V.getPointer() == Inst.getPointer())) {
      float OldDelay = OldAnnotation.calculateDelay();

      dbgs() << "Potential timing violation: ("
             << unsigned(OldAnnotation.violation)
             << ") "<< Slack << ' ' << delay
             << " Old delay " << OldDelay
             << '(' << ((delay - OldDelay) / delay) << ')' << " \n"
             << "Src: " << V->getName() << '(' << V.IsLauch() << ')'
             << " Dst: " << *Inst << '(' << Inst.IsLauch() << ')' << '\n';
    });
  }

  updateDelay(delay, OldAnnotation);
}

void Dataflow::updateDelay(float NewDelay, Annotation &OldDelay) {
 if (OldDelay.generation == 0 && generation != 0)
   OldDelay.reset();

 OldDelay.addSample(NewDelay);
 OldDelay.generation = generation;
}

DataflowAnnotation::DataflowAnnotation(bool Accumulative)
  : VASTModulePass(ID), Accumulative(Accumulative) {
  initializeDataflowAnnotationPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(DataflowAnnotation,
                      "vast-dataflow-annotation", "Dataflow Annotation",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(Dataflow)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(SimpleSelectorSynthesis)
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
  AU.addRequiredID(SimpleSelectorSynthesisID);
  AU.addRequiredID(DatapathNamerID);

  AU.addRequiredID(STGDistancesID);
  AU.addRequired<TimingNetlist>();

  AU.setPreservesAll();
}

void DataflowAnnotation::annotateDelay(DataflowInst Inst, VASTSlot *S,
                                       VASTSeqValue *SV, float delay) {
  unsigned Slack = Distances->getIntervalFromDef(SV, S);
  DF->annotateDelay(Inst, S, SV, delay, Slack);
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
  VASTSeqValue *SlotEn = Op->getSlot()->getValue();

  for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
    VASTLatch L = Op->getSrc(i);
    VASTSelector *Sel = L.getSelector();
    if (Sel->isTrivialFannin(L))
      continue;

    VASTValPtr FI = L;

    // Extract the delay from the fan-in and the guarding condition.
    TNL.extractDelay(Sel, FI.get(), Srcs);
    TNL.extractDelay(Sel, Cnd.get(), Srcs);
    if (Op->guardedBySlotActive() && SlotEn->getLLVMValue())
      TNL.extractDelay(Sel, SlotEn, Srcs);
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

  DF->dumpToSQL();

  return false;
}

void Dataflow::dumpToSQL() const {
  SmallString<256> SQLPath
    = sys::path::parent_path(getStrValueFromEngine("RTLOutput"));
  sys::path::append(SQLPath, "delay_annotation.sql");

  dbgs() << "Dump dataflow annotation to: "
         << SQLPath << '\n';

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
        violation INTEGER, \
        num_samples INTEGER, \
        sum REAL,\
        sqr_sum REAL\
        );\n";

  typedef FlowDepMapTy::const_iterator iterator;
  typedef TimedSrcSet::const_iterator src_iterator;
  for (iterator I = FlowDeps.begin(), E = FlowDeps.end(); I != E; ++I) {
    DataflowInst Dst = I->first;
    const TimedSrcSet &Srcs = I->second;
    for (src_iterator J = Srcs.begin(), E = Srcs.end(); J != E; ++J) {
      // We cannot do anything with the mux for now.
      if (isa<BasicBlock>(J->first))
        continue;

      // We cannot do anything with the BRAM to register path ...
      if (isa<LoadInst>(J->first) && J->first.getPointer() == Dst.getPointer())
        continue;

      OS << "INSERT INTO flowdeps(src, dst, generation, violation, num_samples, sum, sqr_sum) VALUES(\n"
         << '\'' << J->first.getOpaqueValue() << "', \n"
         << '\'' << Dst.getOpaqueValue() << "', \n"
         << unsigned(J->second.generation) << ", \n"
         << unsigned(J->second.violation) << ", \n"
         << unsigned(J->second.num_samples) << ", \n"
         << J->second.sum << ", \n"
         << J->second.sqr_sum << ");\n";
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
        violation INTEGER, \
        num_samples INTEGER, \
        sum REAL,\
        sqr_sum REAL\
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
        if (isa<BasicBlock>(K->first))
          continue;

        OS << "INSERT INTO incomings(src, bb, dst, generation, violation, num_samples, sum, sqr_sum) VALUES(\n"
           << '\'' << K->first.getOpaqueValue() << "', \n"
           << '\'' << BB->getName() << "', \n"
           << '\'' << Dst.getOpaqueValue() << "', \n"
           << unsigned(K->second.generation) << ", \n"
           << unsigned(K->second.violation) << ", \n"
           << unsigned(K->second.num_samples) << ", \n"
           << K->second.sum << ", \n"
           << K->second.sqr_sum << ");\n";
      }
    }
  }
}

Pass *llvm::createDataflowAnnotationPass() {
  return new DataflowAnnotation();
}
