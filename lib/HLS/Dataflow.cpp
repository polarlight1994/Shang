//===------- Dataflow.cpp - Dataflow Analysis on LLVM IR --------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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
#include "vast/Passes.h"
#include "vast/Dataflow.h"
#include "vast/TimingAnalysis.h"
#include "vast/VASTModule.h"
#include "vast/STGDistances.h"
#include "vast/Strash.h"
#include "vast/LuaI.h"

#include "llvm/Analysis/Dominators.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-dataflow"
#include "llvm/Support/Debug.h"

using namespace llvm;

Dataflow::BBPtr::BBPtr(VASTSlot *S) : Base(S->getParent(), S->IsSubGrp) {}

Dataflow::Dataflow() : FunctionPass(ID), generation(0) {
  initializeDataflowPass(*PassRegistry::getPassRegistry());
}

Dataflow::delay_type::delay_type(const Annotation &Ann) {
  float num_samples = float(Ann.num_samples);
  // Calculate the expected value
  total_delay = Ann.delay_sum / num_samples;
  ic_delay = Ann.ic_delay_sum / num_samples;
}

void Dataflow::delay_type::reduce_max(const delay_type &RHS) {
  if (RHS.total_delay > total_delay) {
    total_delay = RHS.total_delay;
    ic_delay = RHS.ic_delay;
    return;
  }
}

float Dataflow::delay_type::expected() const {
  return total_delay;
}

float Dataflow::delay_type::expected_ic_delay() const {
  return ic_delay;
}

void Dataflow::Annotation::addSample(float delay, float ic_delay) {
  num_samples += 1;
  delay_sum += delay;
  delay_sqr_sum += delay * delay;
  ic_delay_sum += ic_delay;
  ic_delay_sqr_sum += ic_delay * ic_delay;
}

void Dataflow::Annotation::reset() {
  delay_sum = delay_sqr_sum = ic_delay_sum = ic_delay_sqr_sum = 0.0f;
  num_samples = 0;
}

void Dataflow::Annotation::dump() const {
  float num_samples = float(this->num_samples);
  // Caculate the expected value
  float E = delay_sum / num_samples;
  // Caculate the variance
  float D2 = delay_sqr_sum / num_samples - E * E;
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
char &vast::DataflowID = Dataflow::ID;

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

    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
      Set[I->first].reduce_max(I->second);
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

      Set[V].reduce_max(I->second);
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
  for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    Set[I->first].reduce_max(I->second);
}

Dataflow::TimedSrcSet &Dataflow::getDeps(DataflowInst Inst, BBPtr Parent) {
  if (Inst->getParent() == Parent && !Parent.isSubGrp()) {
    assert(!isa<PHINode>(Inst) && "Unexpected PHI!");
    return FlowDeps[Inst];
  }

  return Incomings[Inst][Parent];
}

Dataflow::delay_type Dataflow::getSlackFromLaunch(Instruction *Inst) const {
  if (!Inst) return delay_type(0.0f);

  FlowDepMapTy::const_iterator I = FlowDeps.find(DataflowInst(Inst, false));
  if (I == FlowDeps.end())
    return delay_type(0.0f);

  const TimedSrcSet &Srcs = I->second;
  TimedSrcSet::const_iterator J = Srcs.find(DataflowValue(Inst, true));
  if (J == Srcs.end())
    return delay_type(0.0f);

  delay_type delay(J->second);
  return delay_type(1.0 - delay.total_delay, 0);
}

Dataflow::delay_type Dataflow::getDelayFromLaunch(Instruction *Inst) const {
  if (!Inst) return delay_type(0.0f);

  FlowDepMapTy::const_iterator I = FlowDeps.find(DataflowInst(Inst, false));
  if (I == FlowDeps.end())
    return delay_type(0.0f);

  const TimedSrcSet &Srcs = I->second;
  TimedSrcSet::const_iterator J = Srcs.find(DataflowValue(Inst, true));
  if (J == Srcs.end())
    return delay_type(0.0f);

  return J->second;
}

Dataflow::delay_type
Dataflow::getDelay(DataflowValue Src, DataflowInst Dst, VASTSlot *S) const {
  BBPtr BB = getIncomingBlock(S, Dst, Src);

  if (!BB.isSubGrp() && Dst->getParent() == BB) {
    FlowDepMapTy::const_iterator I = FlowDeps.find(Dst);

    // The scheduler may created new path via CFG folding, do not fail in this
    // case.
    assert(I != FlowDeps.end() && "Dst dose not existed in flow dependence?");

    const TimedSrcSet &Srcs = I->second;

    TimedSrcSet::const_iterator J = Srcs.find(Src);
    return J == Srcs.end() ? delay_type(0.0f) : J->second;
  }

  IncomingMapTy::const_iterator I = Incomings.find(Dst);

  // The scheduler may created new path via CFG folding, do not fail in this
  // case.
  if (I == Incomings.end())
    return delay_type(0.0f);

  IncomingBBMapTy::const_iterator J = I->second.find(BB);
  if (J == I->second.end())
    return delay_type(0.0f);

  const TimedSrcSet &Srcs = J->second;
  TimedSrcSet::const_iterator K = Srcs.find(Src);
  return K == Srcs.end() ? delay_type(0.0f) : K->second;
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
                             float delay, float ic_delay, unsigned Slack) {
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
      dbgs() << "Potential timing violation: ("
             << unsigned(OldAnnotation.violation)
             << ") "<< Slack << ' ' << delay
             << "Src: " << V->getName() << '(' << V.IsLauch() << ')'
             << " Dst: " << *Inst << '(' << Inst.IsLauch() << ')' << '\n';
      OldAnnotation.dump();
    });
  }

  updateDelay(delay, ic_delay, OldAnnotation, IsTimingViolation);
}

void Dataflow::updateDelay(float NewDelay, float NewICDelay, Annotation &OldDelay,
                           bool IsTimingViolation) {
 if (OldDelay.generation == 0 && generation != 0)
   OldDelay.reset();

 OldDelay.addSample(NewDelay, NewICDelay);
 OldDelay.generation = generation;
}

namespace {
class DataflowAnnotation : public VASTModulePass {
  Dataflow *DF;
  STGDistances *Distances;
  const bool Accumulative;

  void annotateDelay(VASTModule &VM);

  typedef TimingAnalysis::PhysicalDelay PhysicalDelay;
  void annotateDelay(DataflowInst Inst, VASTSlot *S, VASTSeqValue *V,
                     float delay, float ic_delay);
public:
  typedef Dataflow::delay_type delay_type;

  static char ID;
  explicit DataflowAnnotation(bool Accumulative = false);

  typedef Dataflow::SrcSet SrcSet;
  void getFlowDep(DataflowInst Inst, SrcSet &Set) const {
    DF->getFlowDep(Inst, Set);
  }

  void getIncomingFrom(DataflowInst Inst, BasicBlock *BB, SrcSet &Set) const {
    DF->getIncomingFrom(Inst, BB, Set);
  }

  delay_type getSlackFromLaunch(Instruction *Inst) const {
    return DF->getSlackFromLaunch(Inst);
  }

  delay_type getDelayFromLaunch(Instruction *Inst) const {
    return DF->getDelayFromLaunch(Inst);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
};
}

DataflowAnnotation::DataflowAnnotation(bool Accumulative)
  : VASTModulePass(ID), Accumulative(Accumulative) {
  initializeDataflowAnnotationPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(DataflowAnnotation,
                      "vast-dataflow-annotation", "Dataflow Annotation",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(Dataflow)
  INITIALIZE_AG_DEPENDENCY(TimingAnalysis)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
INITIALIZE_PASS_END(DataflowAnnotation,
                    "vast-dataflow-annotation", "Dataflow Annotation",
                    false, true)

char DataflowAnnotation::ID = 0;

void DataflowAnnotation::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<Dataflow>();

  AU.addRequired<TimingAnalysis>();
  AU.addRequiredID(STGDistancesID);

  AU.setPreservesAll();
}

void DataflowAnnotation::annotateDelay(DataflowInst Inst, VASTSlot *S,
                                       VASTSeqValue *SV, float delay,
                                       float ic_delay) {
  unsigned Slack = Distances->getIntervalFromDef(SV, S);
  DF->annotateDelay(Inst, S, SV, delay, ic_delay, Slack);
}

void DataflowAnnotation::annotateDelay(VASTModule &VM) {
  TimingAnalysis &TA = getAnalysis<TimingAnalysis>();

  typedef VASTModule::seqop_iterator iterator;
  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;

    Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());

    // Nothing to do if Op does not have an underlying instruction.
    if (!Inst)
      continue;

    typedef std::map<VASTSeqValue*, TimingAnalysis::PhysicalDelay>
            ArrivalInfo;
    std::map<VASTSeqValue*, TimingAnalysis::PhysicalDelay> Srcs;
    VASTValPtr Guard = Op->getGuard();
    VASTSeqValue *SlotValue = Op->getSlot()->getValue();

    for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
      VASTLatch L = Op->getSrc(i);
      VASTSelector *Sel = L.getSelector();
      if (Sel->isTrivialFannin(L))
        continue;

      // Extract the delay from the fan-in and the guarding condition.
      VASTValPtr FI = L;
      TA.extractDelay(L, SlotValue, Srcs);
      TA.extractDelay(L, Guard.get(), Srcs);
      TA.extractDelay(L, FI.get(), Srcs);
    }

    typedef ArrivalInfo::iterator src_iterator;
    for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *Src = I->first;
      float delay = I->second.TotalDelay,
        ic_delay = delay - I->second.CellDelay;

      if (Src->isSlot()) {
        if (Src->getLLVMValue() == 0)
          continue;
      }

      annotateDelay(Op, Op->getSlot(), Src, delay, ic_delay);
    }
  }

  // Annotate the unreachable blocks.
  typedef Function::iterator bb_iterator;
  Function &F = VM.getLLVMFunction();
  for (bb_iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    if (TA.isBasicBlockUnreachable(BB))
      DF->addUnreachableBlocks(BB);
  }
}

bool DataflowAnnotation::runOnVASTModule(VASTModule &VM) {
  DF = &getAnalysis<Dataflow>();
  Distances = &getAnalysis<STGDistances>();

  // Force release the context if the annotatio is not accumulative.
  if (!Accumulative)
    DF->releaseMemory();

  annotateDelay(VM);
  DF->increaseGeneration();

  DEBUG(DF->dumpToSQL());

  return false;
}

void Dataflow::dumpToSQL() const {
  SmallString<256> SQLPath
    = sys::path::parent_path(LuaI::GetString("RTLOutput"));
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
        sqr_sum REAL,\
        ic_sum REAL,\
        ic_sqr_sum REAL\
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

      OS << "INSERT INTO flowdeps(src, dst, generation, violation, num_samples, sum, sqr_sum, ic_sum, ic_sqr_sum) VALUES(\n"
         << '\'' << J->first.getOpaqueValue() << "', \n"
         << '\'' << Dst.getOpaqueValue() << "', \n"
         << unsigned(J->second.generation) << ", \n"
         << unsigned(J->second.violation) << ", \n"
         << unsigned(J->second.num_samples) << ", \n"
         << J->second.delay_sum << ", \n"
         << J->second.delay_sqr_sum  << ", \n"
         << J->second.ic_delay_sum << ", \n"
         << J->second.ic_delay_sqr_sum << ");\n";
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
        sqr_sum REAL,\
        ic_sum REAL,\
        ic_sqr_sum REAL\
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

        OS << "INSERT INTO incomings(src, bb, dst, generation, violation, num_samples, sum, sqr_sum, ic_sum, ic_sqr_sum) VALUES(\n"
           << '\'' << K->first.getOpaqueValue() << "', \n"
           << '\'' << BB->getName() << "', \n"
           << '\'' << Dst.getOpaqueValue() << "', \n"
           << unsigned(K->second.generation) << ", \n"
           << unsigned(K->second.violation) << ", \n"
           << unsigned(K->second.num_samples) << ", \n"
           << K->second.delay_sum << ", \n"
           << K->second.delay_sqr_sum << ", \n"
           << K->second.ic_delay_sum << ", \n"
           << K->second.ic_delay_sqr_sum << ");\n";
      }
    }
  }
}

Pass *vast::createDataflowAnnotationPass(bool Accumulative) {
  return new DataflowAnnotation(Accumulative);
}

char &llvm::DataflowAnnotationID = DataflowAnnotation::ID;
