#include "sir/SIRScheduling.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

using namespace llvm;
using namespace vast;
using namespace std;

char SIRScheduling::ID = 0;
char &llvm::SIRSchedulingID = SIRScheduling::ID;

SIRScheduling::SIRScheduling() : SIRPass(ID) {
  initializeSIRSchedulingPass(*PassRegistry::getPassRegistry());
}

void SIRScheduling::getAnalysisUsage(AnalysisUsage &AU) const {
  SIRPass::getAnalysisUsage(AU);
  AU.addRequired<SIRInit>();
  AU.addRequired<SIRTimingAnalysis>();
  AU.addRequired<AliasAnalysis>();
  AU.addRequired<DominatorTree>();
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

INITIALIZE_PASS_BEGIN(SIRScheduling,
                      "sir-scheduling", "Perform Scheduling on the SIR",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(SIRTimingAnalysis)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRScheduling,
                    "sir-scheduling", "Perform Scheduling on the SIR",
                    false, true)

Pass *llvm::createSIRSchedulingPass() {
  return new SIRScheduling();
}

SIRSchedUnit *SIRScheduling::getOrCreateBBEntry(BasicBlock *BB) {
  // Take the Entry SUnit as the pseudo BBEntry of these SUnit in
  // Slot0r which do not have a real parent BB.
  if (!BB) return G->getEntry();

  // Simply return the BBEntry if it had already existed.
  if (G->hasSU(BB)) {
    ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(BB);
    assert(SUs.size() == 1 && "Unexpected mutil-SUnits!");

    return SUs.front();
  }

  // Or we have to create the BBEntry.
  SIRSchedUnit *Entry = G->createSUnit(BB, SIRSchedUnit::BlockEntry);

  // If the BB has no Preds, which means it's a Entry BB.
  // The Entry SUnit of Entry BB should have a SIRDep
  // coming from the Entry of Scheduling Graph.
  if (pred_begin(BB) == pred_end(BB))
    Entry->addDep(G->getEntry(), SIRDep::CreateCtrlDep(0));

  // The BBEntry should be indexed into EntrySlot.
  SIRSlot *EntrySlot = SM->getLandingSlot(BB);
  G->indexSU2Slot(Entry, EntrySlot);

  // Save the mapping between the SUnit with the Value.
  G->indexSU2IR(Entry, BB);

  return Entry;
}

void SIRScheduling::constraintTerminators(BasicBlock *BB) {
  // Get the terminator of this BB and check if it is a Ret instruction.
  TerminatorInst *Inst = BB->getTerminator();
  if (!isa<ReturnInst>(Inst)) return;

  // Get the corresponding SUnits of the Ret instruction. To be noted
  // that the SUnits is not indexed to the Ret instruction but to the
  // operand of the Ret instruction because in SIRBuilder we replace
  // the original operand of Ret into this pseudo instruction to act
  // as the SeqVal.
  ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(Inst->getOperand(0));

  // The ExitSUnit is depended on these SUnits.
  SIRSchedUnit *Exit = G->getExit();
  for (int i = 0; i < SUs.size(); i++)
    Exit->addDep(SUs[i], SIRDep::CreateCtrlDep(0));
}

void SIRScheduling::buildDependencies() {
  // The dependencies need to be built includes
  // 1) data dependency
  // 2) control dependency
  // 3) memory dependency

  // Visit all SUnits to build data & control dependency.
  typedef SIRSchedGraph::iterator iterator;
  for (iterator I = G->begin(), E = G->end(); I != E; I++) {
    SIRSchedUnit *SU = I;

    buildDataDependencies(SU);
    buildControlDependencies(SU);
  }

  // Visit all BBs to build the memory dependencies.
  Function &F = G->getFunction();
  ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;
  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    buildMemoryDependencies(*I);
}

void SIRScheduling::buildDataDependencies(SIRSchedUnit *U) {
  // Entry/Exit/BBEntry do not have any data dependencies.
  if (U->isEntry() || U->isExit() || U->isBBEntry()) return;

  SIRTimingAnalysis::ArrivalMap Arrivals;

  if (U->isCombSU()) {
    Instruction *CombOp = U->getCombOp();
    assert(CombOp && "Unexpected NULL CombOp!");

    TA->extractArrivals(TD, CombOp, Arrivals);
  } else {
    SIRSeqOp *SeqOp = U->getSeqOp();
    assert(SeqOp && "Unexpected NULL SeqOp!");

    TA->extractArrivals(TD, SeqOp, Arrivals);
  }

  typedef SIRTimingAnalysis::ArrivalMap::iterator iterator;
  for (iterator I = Arrivals.begin(), E = Arrivals.end(); I != E; ++I) {
    Value *SrcVal = I->first;
    float Delay = I->second;

    // The memrdata Value will have no corresponding SUnit since the
    // assign operation is not implemented in SIR.
    if (SIRRegister *Reg = SM->lookupSIRReg(SrcVal))
      if (Reg->isFUOutput())
        continue;

    ArrayRef<SIRSchedUnit *> SrcSUs = getDataFlowSU(SrcVal);
    assert(SrcSUs.size() && "Unexpected NULL SrcSUs!");

    for (int i = 0; i < SrcSUs.size(); i++)
      U->addDep(SrcSUs[i], SIRDep::CreateValDep(Delay));
  }
}

void SIRScheduling::buildControlDependencies(SIRSchedUnit *U) {
  // Entry do not have any control dependencies except the
  // dependency to the slot transition which will be handled
  // later.
  if (U->isEntry()) return;

  // The first kind of control dependency is that all SUnits
  // is depending on the EntrySU of this BB.
  BasicBlock *ParentBB = U->getParentBB();

  SIRSchedUnit *EntrySU = getOrCreateBBEntry(ParentBB);
  // Do not add self-loop.
  if (!U->isBBEntry())
    U->addDep(EntrySU, SIRDep::CreateCtrlDep(0));

  // Other two kinds of control dependency is:
  // 1) transition to next slot in current basic block.
  // 2) transition to successor basic block.
  // and they are all associated with the SlotTransition.
  if (!U->isSlotTransition()) return;

  // Get the SlotTransition.
  SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(U->getSeqOp());
  assert (SST && "Unexpected NULL SIRSlotTransition!");

  // Get the destination slot.
  SIRSlot *DstSlot = SST->getDstSlot();

  // All SUnits in destination slot are depended on this SlotTransition.
  ArrayRef<SIRSchedUnit *> SUsInDstSlot = G->lookupSUs(DstSlot);

  // If the fist SUnit in destination slot is a BBEntry, that means we
  // are transiting to successor BB. In this circumstance, we can just
  // constraint the BBEntry, since other SUnits is constrained by the
  // BBEntry already. By doing this, we can ensure that all control
  // edges between BBs are ended on the BBEntry which is easy to handle
  // later especially when it is a back-edge.
  SIRSchedUnit *FirstSUsInDstSlot = SUsInDstSlot[0];
  if (FirstSUsInDstSlot->isBBEntry() || FirstSUsInDstSlot->isEntry())
    FirstSUsInDstSlot->addDep(U, SIRDep::CreateCtrlDep(0));
  // Or we are transition to the next slot in same BB. In this circumstance,
  // all SUnit in next slot is depended on the SlotTransition.
  else
    for (int i = 0; i < SUsInDstSlot.size(); i++)
      SUsInDstSlot[i]->addDep(U, SIRDep::CreateCtrlDep(0));

  // Constraint the non-dep SUnit to the Entry.
  if (U->dep_empty())
    U->addDep(G->getEntry(), SIRDep::CreateCtrlDep(0));
}

void SIRScheduling::buildMemoryDependency(Instruction *SrcInst, Instruction *DstInst) {
  // If either of them are call instruction, we need a dependencies,
  // because we are not sure the memory locations accessed by the call.
  if (!isCall(SrcInst) && !isCall(DstInst)) {
    // Ignore the RAR dependencies
    if (!SrcInst->mayWriteToMemory() && !DstInst->mayWriteToMemory())
      return;

    // There is no dependencies if the memory locations do not alias each other.
    if (isNoAlias(SrcInst, DstInst, AA)) return;
  }

  ArrayRef<SIRSchedUnit *> SrcUs = G->lookupSUs(SrcInst), DstUs = G->lookupSUs(DstInst);
  // Only one SUnit expected here.
  assert((SrcUs.size() == DstUs.size() == 1) && "Unexpected mutil-SUnits!");
  SIRSchedUnit *SrcU = SrcUs.front(), *DstU = DstUs.front();

  unsigned Latency = 1;

  // If the DstInst is call instruction, then the real dependency is from the callee
  // instruction to Src. So we can minus the delay by 1.
  if (isa<CallInst>(DstInst)) {
    Latency = 0;
  }

  DstU->addDep(SrcU, SIRDep::CreateMemDep(Latency, 0));
}

void SIRScheduling::buildMemoryDependencies(BasicBlock *BB) {
  typedef BasicBlock::iterator iterator;
  SmallVector<Instruction *, 16> PiorMemInsts;

  for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    Instruction *Inst = I;

    if (!isLoadStore(Inst) && !isCall(Inst))
      continue;

    if (!G->hasSU(Inst))
      continue;

    for (unsigned I = 0, E = PiorMemInsts.size(); I < E; ++I)
      buildMemoryDependency(PiorMemInsts[I], Inst);

    PiorMemInsts.push_back(Inst);
  }
}

ArrayRef<SIRSchedUnit *> SIRScheduling::getDataFlowSU(Value *V) {
  // If we are getting the corresponding SUnit of the argument,
  // then we can just return the Entry SUnit of the SchedGraph.
  if (isa<Argument>(V)) return G->getEntry();

  assert(G->hasSU(V) && "Flow dependencies missed!");

  // We should get the corresponding SUnit of this LLVM IR.
  // To be noted that, if we are passing in BB as Value here,
  // then we will get the Entry SUnit of the BB.
  ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(V);

  if (isa<BasicBlock>(V))
    assert(SUs.size() == 1 && "Unexpected multi-SUnits!");

  return SUs;
}

void SIRScheduling::buildSchedulingUnitsForSeqOp(SIRSlot *S) {
  BasicBlock *BB = S->getParent();

  // Before we create the SUnit for all SeqOps in this Slot,
  // we should create the BBEntry for the ParentBB, since
  // we are not visit by BB, so we need to getParentBB from
  // the Slot and if it is already created in previous slot
  // then return it, and if we are handling Slot0r,
  // then the BB is NULL so we return the Entry SUnit.
  SIRSchedUnit *BBEntry = getOrCreateBBEntry(BB);

  // Collect all SeqOps in this slot and create SUnits for them.
  std::vector<SIRSeqOp *> Ops;
  Ops.insert(Ops.end(), S->op_begin(), S->op_end());

  typedef std::vector<SIRSeqOp *>::iterator op_iterator;
  for (op_iterator OI = Ops.begin(), OE = Ops.end(); OI != OE; ++OI) {
    SIRSeqOp *Op = *OI;

    Instruction *Inst = dyn_cast<Instruction>(Op->getLLVMValue());

    // Detect whether the type of SIRSchedUnit according to the DstReg.
    SIRSchedUnit::Type Ty;
    SIRRegister *DstReg = Op->getDst();
    if (DstReg->isPHI())			  Ty = SIRSchedUnit::PHI;
    else if (DstReg->isSlot())	Ty = SIRSchedUnit::SlotTransition;
    else                        Ty = SIRSchedUnit::SeqSU;

    SIRSchedUnit *U = G->createSUnit(BB, Ty, Op);

    // Index the SUnit to the Slot and the LLVM IR.
    G->indexSU2Slot(U, S);
    G->indexSU2IR(U, Inst);

    continue;
  }
}

void SIRScheduling::buildSchedulingUnitsForCombOp(Instruction *CombOp) {
  BasicBlock *BB = CombOp->getParent();

  SIRSchedUnit *U = G->createSUnit(BB, CombOp);

  // Index the SUnit to the LLVM IR.
  G->indexSU2IR(U, CombOp);
}

void SIRScheduling::finishBuildingSchedGraph() {
  SIRSchedUnit *Exit = G->getExit();

  typedef SIRSchedGraph::iterator iterator;
  for (iterator I = llvm::next(G->begin()), E = Exit; I != E; ++I) {
    SIRSchedUnit *U = I;

    // Ignore the BBEntry.
    if (U->isBBEntry()) continue;

    // Constraint the non-use SUnit to the Exit.
    if(U->use_empty())
      Exit->addDep(U, SIRDep::CreateCtrlDep(0));
  }

  // Handle the Terminators.
  Function &F = G->getFunction();
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    constraintTerminators(I);
  }
}

void SIRScheduling::buildSchedulingGraph() {
  SIRSchedUnit *Entry = G->getEntry();
  SIRSlot *StartSlot = SM->getStartSlot();

  // Index the Entry SUnit to the StartSlot.
  G->indexSU2Slot(Entry, StartSlot);

  // Build the Scheduling Units for SeqOps.
  {
    ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >
      RPO(SM->getStartSlot());
    typedef
      ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >::rpo_iterator
      slot_iterator;

    // Visit the SIRSlots in reverse post order so that the building order of
    // SUnits is topological generally to avoid creating a dependency to a
    // SUnit which is not created yet when building dependencies.
    for (slot_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
      buildSchedulingUnitsForSeqOp(*I);
  }

  // Build the Scheduling Units for CombOps.
  {
    Function *F = SM->getFunction();

    typedef Function::iterator bb_iterator;
    for (bb_iterator BBI = F->begin(), BBE = F->end(); BBI != BBE; ++BBI) {
      BasicBlock *BB = BBI;

      typedef BasicBlock::iterator inst_iterator;
      for (inst_iterator InstI = BB->begin(), InstE = BB->end();
           InstI != InstE; ++InstI) {
        Instruction *Inst = InstI;

        if (!isa<IntrinsicInst>(Inst) && !isa<IntToPtrInst>(Inst) &&
          !isa<PtrToIntInst>(Inst) && !isa<BitCastInst>(Inst))
          continue;

        if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst))
          if (II->getIntrinsicID() == Intrinsic::shang_reg_assign)
            continue;

        buildSchedulingUnitsForCombOp(Inst);
      }
    }
  }

  // Build dependencies.
  buildDependencies();

  // Constraint all nodes that do not have a user by adding SIRDep to
  // the terminator in its parent BB.
  finishBuildingSchedGraph();

  // Sort all SUnits according to the dependencies and reassign the
  // index based on the result, so we can easily recognize the back-edge
  // according to the index.
  G->topologicalSortSUs();

  /// Debug Code.
  std::string SchedGraph = LuaI::GetString("SchedGraph");
  std::string Error;
  raw_fd_ostream Output(SchedGraph.c_str(), Error);

  typedef SIRSchedGraph::iterator iterator;
  for (iterator I = G->begin(), E = G->end(); I != E; ++I) {
    SIRSchedUnit *U = I;

    Output << "#" << U->getIdx() << ":\t";

    Output << "Depends on: ";

    typedef SIRSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E; ++I) {
      SIRSchedUnit *DepSU = *I;

      Output << "#" << DepSU->getIdx() << "; ";
    }

    Output << "\t";

    switch (U->getType()) {
    case SIRSchedUnit::Entry:
      Output << "Entry\n";
      break;
    case SIRSchedUnit::Exit:
      Output << "Exit\n";
      break;
    case SIRSchedUnit::BlockEntry:
      Output << "BBEntry of ";
      Output << U->getParentBB()->getName() << "\n";
      break;
    case SIRSchedUnit::PHI:
      Output << "PHI of ";
      Output << U->getSeqOp()->getDst()->getName() << "\n";
      break;
    case SIRSchedUnit::SlotTransition: {
      SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(U->getSeqOp());
      Output << "SlotTransition    ";
      Output << "Slot#" << SST->getSrcSlot()->getSlotNum()
        << " -- Slot#" << SST->getDstSlot()->getSlotNum() << "\n";
      break;
                                       }
    case SIRSchedUnit::SeqSU: {
      SIRSeqOp *SeqOp = U->getSeqOp();
      Output << "SeqSU    ";
      Output << "assign Value [";
      SeqOp->getSrc()->print(Output);
      Output << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
        << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
      break;
                              }
    case SIRSchedUnit::CombSU:
      Output << "CombOp    ";
      Output << "CombOp contained: [";
      U->getCombOp()->print(Output);
      Output << "]\n";
      break;
    default:
      llvm_unreachable("Unexpected SUnit Type!");
    }
  }
}


void SIRScheduling::schedule() {
  ListScheduler LS(*G, G->getEntry()->getSchedule());

  LS.schedule();
  //	SIRSDCScheduler SDC(*G, G->getEntry()->getSchedule());
  //
  //	SDC.schedule();
}

void SIRScheduling::emitSchedule() {
  SIRScheduleEmitter SSE(*TD, SM, *G);

  SSE.emitSchedule();
}

bool SIRScheduling::runOnSIR(SIR &SM) {
  // Initialize the SIR and corresponding LLVM Function.
  this->SM = &SM;
  this->TA = &getAnalysis<SIRTimingAnalysis>();
  this->AA = &getAnalysis<AliasAnalysis>();
  this->DT = &getAnalysis<DominatorTree>();
  this->TD = &getAnalysis<DataLayout>();
  Function &F = *SM.getFunction();

  OwningPtr<SIRSchedGraph> GPtr(new SIRSchedGraph(F));
  G = GPtr.get();

  // Build the Scheduling Graph and all the dependencies
  // between the SUnits.
  buildSchedulingGraph();

  schedule();

  emitSchedule();

  return true;
}

namespace {
  bool SUnitLess(SIRSchedUnit *LHS, SIRSchedUnit *RHS) {
    return LHS->getSchedule() < RHS->getSchedule();
  }
}

void SIRScheduleEmitter::insertSlotBefore(SIRSlot *S, SIRSlot *DstS,
                                          SIRSlot::EdgeType T, Value *Cnd) {
  SmallVector<SIRSlot::EdgePtr, 4> Preds;
  for (SIRSlot::pred_iterator I = DstS->pred_begin(), E = DstS->pred_end();
       I != E; I++) {
    Preds.push_back(*I);
  }

  typedef SmallVector<SIRSlot::EdgePtr, 4>::iterator iterator;
  for (iterator I = Preds.begin(), E = Preds.end(); I != E; I++) {
    SIRSlot *Pred = I->getSlot();

    // Unlink the edge from Pred to DstS.
    Pred->unlinkSucc(DstS);

    // Link the edge from Pred to S.
    C_Builder.createStateTransition(Pred, S, I->getCnd());
  }

  // Link the edge from S to DstS.
  C_Builder.createStateTransition(S, DstS, Cnd);
}

void SIRScheduleEmitter::emitSUsInBB(MutableArrayRef<SIRSchedUnit *> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry must be placed at the beginning!");

  BasicBlock *BB = SUs[0]->getParentBB();
  SIRSlot *EntrySlot = SM->getLandingSlot(BB);

  assert(EntrySlot && "Landing Slot not created?");
  assert(EntrySlot->getStepInLocalBB() == 0 && "Unexpected local step!");

  // The global schedule result of the Entry SUnit.
  unsigned EntrySUSchedule = SUs[0]->getSchedule();

  std::vector<SIRSchedUnit *> NewSUs(SUs.begin() +1, SUs.end());
  // Sort the SUs to make sure they are ranged by schedule in ascending order.
  std::sort(NewSUs.begin(), NewSUs.end(), SUnitLess);

  assert(SUs[0]->isBBEntry() && "BBEntry must be placed at the beginning!");

  for (unsigned i = 0; i < NewSUs.size(); ++i) {
    SIRSchedUnit *CurSU = NewSUs[i];

    if (CurSU->isCombSU()) continue;

    SIRSeqOp *SeqOp = CurSU->getSeqOp();
    SIRSlot *EmitSlot = SeqOp->getSlot();

    // The local step of specified slot.
    unsigned EmitSlotStep = EmitSlot->getStepInLocalBB();
    // The target local step of the SUnit. Since we may specify the target
    // slot in SIRBuild pass, so we must consider the constraint it brings.
    unsigned TargetStep = CurSU->getSchedule() - EntrySUSchedule;
    TargetStep = max(TargetStep, EmitSlotStep);

    // The numbers of slots need to allocate to meet the
    // target local step of the SUnit.
    unsigned SlotsNeedToAlloca = TargetStep - EmitSlotStep;

    // Set the schedule of the EmitSlot to the target step
    // since we will allocate enough slots before it.
    EmitSlot->setStep(TargetStep);

    while(SlotsNeedToAlloca--) {
      SIRSlot *NewSlot = C_Builder.createSlot(BB, TargetStep--);

      // Insert the NewSlot from bottom to up before the EmitSlot.
      insertSlotBefore(NewSlot, EmitSlot, SIRSlot::Sucessor,
                       D_Builder.createIntegerValue(1, 1));

      EmitSlot = NewSlot;
    }
  }
}

void SIRScheduleEmitter::emitSchedule() {
  // Get some basic information.
  Function &F = *SM->getFunction();

  // Visit the basic block in topological order to emit all SUnits in BB.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;
  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    if (!G.isBBReachable(BB))
      continue;

    MutableArrayRef<SIRSchedUnit *> SUs(G.getSUsInBB(BB));
    emitSUsInBB(SUs);
  }

  // After ScheduleEmit, lots of SlotTransitions will be replaced by new ones
  // and become useless, so we remove them here.
  typedef SIR::seqop_iterator seqop_iterator;
  for (seqop_iterator I = SM->seqop_begin(), E = SM->seqop_end(); I != E;) {
    // We must move forward the iterator here to avoid the error caused by
    // iterator erase function called below.
    SIRSeqOp *SeqOp = I++;

    if (SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(SeqOp)) {
      SIRSlot *SrcSlot = SST->getSrcSlot();
      SIRSlot *DstSlot = SST->getDstSlot();

      if (!SrcSlot->hasNextSlot(DstSlot))
        SM->deleteUselessSeqOp(SeqOp);
    }
  }
}