#include "sir/SIRScheduling.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace vast;

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
  /// First constraint the Branch SUnits and PHI SUnits into the
  /// last step of this BB.
  SIRSlot *ExitSlot = SM->getLatestSlot(BB);
  ArrayRef<SIRSchedUnit *> SUsInExitSlot = G->lookupSUs(ExitSlot);

  ArrayRef<SIRSchedUnit *> SUsInBB = G->getSUsInBB(BB);

  if (ExitSlot != SM->getLandingSlot(BB)) {
    for (int i = 0; i < SUsInExitSlot.size(); ++i) {
      SIRSchedUnit *SU = SUsInExitSlot[i];

      for (int j = 0; j < SUsInBB.size(); ++j) {
        SIRSchedUnit *DepSU = SUsInBB[j];

        if (DepSU->isSlotTransition() || DepSU->isPHI() || DepSU->isPHIPack()) continue;

        SU->addDep(DepSU, SIRDep::CreateCtrlDep(0));
      }
    }
  }

  /// Second constraint the Return SUnits into the last step of
  /// whole module.
  TerminatorInst *Inst = BB->getTerminator();
  if (!isa<ReturnInst>(Inst)) return;

  // Get the corresponding SUnits of the Ret instruction. To be noted
  // that the SUnits is not indexed to the Ret instruction but to the
  // operand of the Ret instruction because in SIRBuilder we replace
  // the original operand of Ret into this pseudo instruction to act
  // as the SeqVal. But we should ignore the instruction like "ret
  // void".
  if (!Inst->getNumOperands()) return;
  ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(Inst->getOperand(0));

  // The ExitSUnit is depended on these SUnits.
  SIRSchedUnit *Exit = G->getExit();
  for (unsigned i = 0; i < SUs.size(); i++)
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

  Function &F = G->getFunction();

  // Visit all BBs to build the memory dependencies.
  ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;
  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    buildMemoryDependencies(*I);
}

void SIRScheduling::buildDataDependencies(SIRSchedUnit *U) {
  // Entry/Exit/BBEntry do not have any data dependencies.
  if (U->isEntry() || U->isExit() || U->isBBEntry()) return;

  // Construct the data flow dependencies according
  // to the Timing Analysis result.
  SIRTimingAnalysis::ArrivalMap AT;
  // Extract all the dependencies coming from
  // the Src value of current SIRSeqOp.
  TA->extractArrivals(SM, U->getSeqOp(), AT);

  typedef SIRTimingAnalysis::ArrivalMap::iterator iterator;
  for (iterator I = AT.begin(), E = AT.end(); I != E; ++I) {
    Value *SrcVal = I->first;
    float Delay = I->second;

    if (SIRRegister *Reg = SM->lookupSIRReg(SrcVal))
      // If we are data depended to the output of memrdata, then
      // this SUnit must be the AssignToResult SeqOp in MemInst,
      // and the dependency will be handled in buildMemoryDependency.
      if (Reg->isFUOutput())
        continue;

    ArrayRef<SIRSchedUnit *> SrcSUs = getDataFlowSU(SrcVal);
    assert(SrcSUs.size() && "Unexpected NULL SrcSUs!");

    for (unsigned i = 0; i < SrcSUs.size(); i++) {
      SIRSchedUnit *SrcSU = SrcSUs[i];

      unsigned Distance = 0;

      if (SrcSU->isPHI() && SrcSU->getParentBB() == U->getParentBB())
        Distance = 1;
      else
        Distance = 0;

      U->addDep(SrcSUs[i], SIRDep::CreateValDep(Delay, Distance));
    }
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

  // Get the src slot.
  SIRSlot *SrcSlot = SST->getSrcSlot();

  // The SlotTransitions in DstSlot are depended on all SUnits in SrcSlot.
  ArrayRef<SIRSchedUnit *> SUsInSrcSlot = G->lookupSUs(SrcSlot);

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
  if (FirstSUsInDstSlot->isBBEntry() || FirstSUsInDstSlot->isEntry()) {
    FirstSUsInDstSlot->addDep(U, SIRDep::CreateCtrlDep(0));

    // If we are transiting back to the beginning of this BB, then
    // we are handling a loop BB. So index the loop BB and this
    // corresponding loop SU here.
    if (FirstSUsInDstSlot->isBBEntry() && DstSlot->getParent() == ParentBB)
      G->indexLoopSU2LoopBB(U, ParentBB);
  }
  // Or we are transition to the next slot in same BB. In this circumstance,
  // all SUnit in next slot is depended on the SlotTransition.
  else {
    for (unsigned i = 0; i < SUsInDstSlot.size(); i++) {
      SIRSchedUnit *SU = SUsInDstSlot[i];

      // This will be handled later.
      if (SU->isSlotTransition()) continue;

      SUsInDstSlot[i]->addDep(U, SIRDep::CreateCtrlDep(0));
    }

    // Constraint the non-dep SUnit to the Entry.
    if (U->dep_empty())
      U->addDep(G->getEntry(), SIRDep::CreateCtrlDep(0));
  }
}

void SIRScheduling::buildMemoryDependencies(BasicBlock *BB) {
  // The map between the Bank and all MemInsts visit this Bank.
  std::map<SIRMemoryBank *, SmallVector<Value *, 4> > Bank2MemInsts;

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    Instruction *Inst = I;
    if (!isLoadStore(Inst)) continue;

    ArrayRef<SIRSeqOp *> SeqOps = SM->lookupMemSeqOps(Inst);
    SIRMemoryBank *Bank;

    if (isa<LoadInst>(Inst)) {
      Bank = SA->getMemoryBank(*dyn_cast<LoadInst>(Inst));

      SIRSeqOp *AssignToResult = SeqOps.back();
      SIRSchedUnit *AssignToResultSU = G->lookupSU(AssignToResult);

      for (unsigned i = 0; i < SeqOps.size() - 1; i++) {
        SIRSchedUnit *SU = G->lookupSU(SeqOps[i]);

        AssignToResultSU->addDep(SU, SIRDep::CreateMemDep(2));
      }
    }
    else
      Bank = SA->getMemoryBank(*dyn_cast<StoreInst>(Inst));

    if (!Bank2MemInsts.count(Bank)) {
      SmallVector<Value *, 4> MemInsts;
      MemInsts.push_back(Inst);

      Bank2MemInsts.insert(std::make_pair(Bank, MemInsts));
    } else {
      // Get all other MemInsts that visit the same bank.
      ArrayRef<Value *> OtherMemInsts = Bank2MemInsts[Bank];

      // Create the memory dependency between the origin
      // SeqOps and the SeqOps of other MemInsts that
      // visit the same bank.
      for (unsigned i = 0; i < OtherMemInsts.size(); ++i) {
        Value *MemInst = OtherMemInsts[i];

        // Get all SeqOps created for this MemInst.
        ArrayRef<SIRSeqOp *> OtherSeqOps = SM->lookupMemSeqOps(MemInst);

        for (unsigned j = 0; j < SeqOps.size(); ++j) {
          SIRSeqOp *SeqOp = SeqOps[j];
          SIRSchedUnit *SU = G->lookupSU(SeqOp);

          for (unsigned k = 0; k < OtherSeqOps.size(); ++k) {
            SIRSeqOp *OtherSeqOp = OtherSeqOps[k];
            SIRSchedUnit *OtherSU = G->lookupSU(OtherSeqOp);

            SU->addDep(OtherSU, SIRDep::CreateMemDep(0));
          }
        }
      }

      // Remember to collect this MemInst into the map.
      Bank2MemInsts[Bank].push_back(Inst);
    }
  }
}

void SIRScheduling::packSUnits() {
  Function &F = G->getFunction();
  // Pack all SUnits that handled the MemoryBank Input Register.
  ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    SmallVector<SIRSeqOp *, 4> OutputSeqOpsPack;
    SmallVector<SIRSeqOp *, 4> PHISeqOpsPack;

    ArrayRef<SIRSchedUnit *> SUs = G->getSUsInBB(BB);
    for (unsigned i = 0; i < SUs.size(); ++i) {
      SIRSchedUnit *SU = SUs[i];

      if (SU->isBBEntry())
        continue;

      if (SU->isPHI()) {
        PHISeqOpsPack.push_back(SU->getSeqOp());
        continue;
      }

      SIRSeqOp *SeqOp = SU->getSeqOp();
      SIRRegister *Reg = SeqOp->getDst();

      // Pack all SUnits assign to Output Register.
      if (Reg->isOutPort())
        OutputSeqOpsPack.push_back(SeqOp);
    }
    if (PHISeqOpsPack.size()) {
      SIRSchedUnit *PHIPack = buildSchedulingUnitsPack(BB, PHISeqOpsPack, SIRSchedUnit::PHIPack);
    }
    if (OutputSeqOpsPack.size())
      buildSchedulingUnitsPack(BB, OutputSeqOpsPack, SIRSchedUnit::OutputPack);
  }

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    typedef BasicBlock::iterator iterator;
    for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
      Instruction *Inst = I;
      if (!isLoadStore(Inst)) continue;

      ArrayRef<SIRSeqOp *> SeqOps = SM->lookupMemSeqOps(Inst);
      SmallVector<SIRSeqOp *, 4> SeqOpsPack;

      if (isa<LoadInst>(Inst)) {
        for (unsigned i = 0; i < SeqOps.size() - 1; ++i)
          SeqOpsPack.push_back(SeqOps[i]);

        if (SeqOpsPack.size())
          buildSchedulingUnitsPack(BB, SeqOpsPack, SIRSchedUnit::MemoryPack);
      }
      else if (isa<StoreInst>(Inst)) {
        for (unsigned i = 0; i < SeqOps.size(); ++i)
          SeqOpsPack.push_back(SeqOps[i]);

        if (SeqOpsPack.size())
          buildSchedulingUnitsPack(BB, SeqOpsPack, SIRSchedUnit::MemoryPack);
      }
    }
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

SIRSchedUnit *SIRScheduling::buildSchedulingUnitsPack(BasicBlock *BB,
                                                      SmallVector<SIRSeqOp *, 4> SeqOps,
                                                      SIRSchedUnit::Type T) {
  // Create a New SUnit.
  SIRSchedUnit *PackSU = G->createSUnit(BB, T, SeqOps);

  // Inherit all dependencies.
  typedef SmallVector<SIRSeqOp *, 4>::iterator iterator;
  for (iterator I = SeqOps.begin(), E = SeqOps.end(); I != E; ++I) {
    SIRSeqOp *SeqOp = *I;

    SIRSchedUnit *SU = G->lookupSU(SeqOp);
    G->replaceAllUseWith(SU, PackSU);
  }

  return PackSU;
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
  SIRSlot *StartSlot = SM->slot_begin();

  // Index the Entry SUnit to the StartSlot.
  G->indexSU2Slot(Entry, StartSlot);

  // Build the Scheduling Units for SeqOps.
  ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> > RPO(StartSlot);
  typedef
    ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >::rpo_iterator
    slot_iterator;

  // Visit the SIRSlots in reverse post order so that the building order of
  // SUnits is topological generally to avoid creating a dependency to a
  // SUnit which is not created yet when building dependencies.
  for (slot_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    buildSchedulingUnitsForSeqOp(*I);

  // Build dependencies.
  buildDependencies();

  // Pack SUnits that need to be scheduled together.
  packSUnits();

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
      ArrayRef<SIRSeqOp *> SeqOps = U->getSeqOps();
      for (unsigned i = 0; i < SeqOps.size(); ++i) {
        SIRSeqOp *SeqOp = SeqOps[i];

        Output << "SeqSU    ";
        Output << "assign Value [";
        SeqOp->getSrc()->print(Output);
        Output << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
          << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
      }
      break;
    }
    case SIRSchedUnit::MemoryPack: {
      ArrayRef<SIRSeqOp *> SeqOps = U->getSeqOps();
      for (unsigned i = 0; i < SeqOps.size(); ++i) {
        SIRSeqOp *SeqOp = SeqOps[i];

        Output << "SeqSU    ";
        Output << "assign Value [";
        SeqOp->getSrc()->print(Output);
        Output << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
          << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
      }
      break;
    }
    case SIRSchedUnit::PHIPack: {
      ArrayRef<SIRSeqOp *> SeqOps = U->getSeqOps();
      for (unsigned i = 0; i < SeqOps.size(); ++i) {
        SIRSeqOp *SeqOp = SeqOps[i];

        Output << "SeqSU    ";
        Output << "assign Value [";
        SeqOp->getSrc()->print(Output);
        Output << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
          << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
      }
      break;
    }
    case SIRSchedUnit::OutputPack: {
      ArrayRef<SIRSeqOp *> SeqOps = U->getSeqOps();
      for (unsigned i = 0; i < SeqOps.size(); ++i) {
        SIRSeqOp *SeqOp = SeqOps[i];

        Output << "SeqSU    ";
        Output << "assign Value [";
        SeqOp->getSrc()->print(Output);
        Output << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
          << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
      }
      break;
    }
    default:
      llvm_unreachable("Unexpected SUnit Type!");
    }
  }
}

void SIRScheduling::schedule() {
//   ListScheduler LS(*G, G->getEntry()->getSchedule());
//
//   LS.schedule();
	SIRSDCScheduler SDC(*G, G->getEntry()->getSchedule());

	SDC.schedule();
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
  this->SA = &getAnalysis<SIRAllocation>();
  Function &F = *SM.getFunction();

  OwningPtr<SIRSchedGraph> GPtr(new SIRSchedGraph(F));
  G = GPtr.get();

  // Build the Scheduling Graph and all the dependencies
  // between the SUnits.
  buildSchedulingGraph();

  // Use the IMSScheduler on all loop BBs.
  typedef SIRSchedGraph::const_loopbb_iterator iterator;
  for (iterator I = G->loopbb_begin(), E = G->loopbb_end(); I != E; ++I) {
    BasicBlock *BB = I->first;

    SIRIMSScheduler IMS(&SM, TD, *G, BB);

    // If we pipeline the BB successfully, index it.
    if (IMS.schedule() == SIRIMSScheduler::Success) {
      errs() << "Pipelined BB " << BB->getName() << "in II of "
             << IMS.getMII() << "\n";

      SM.IndexPipelinedBB2MII(BB, IMS.getMII());
    }
  }

  schedule();

  emitSchedule();

//   std::string LoopBBInfo = LuaI::GetString("LoopBBInfo");
//   std::string Error;
//   raw_fd_ostream Output(LoopBBInfo.c_str(), Error);
//
//   typedef SIRSchedGraph::const_loopbb_iterator iterator;
//   for (iterator I = G->loopbb_begin(), E = G->loopbb_end(); I != E; ++I) {
//     BasicBlock *BB = I->first;
//
//     SIRSlot *StartSlot = SM.getLandingSlot(BB);
//     SIRSlot *LatestSlot = SM.getLatestSlot(BB);
//
//     Output << BB->getName() << " starts from Slot#" << StartSlot->getSlotNum()
//            << " and ends at Slot#" << LatestSlot->getSlotNum() << "\n";
//   }

  return true;
}

void SIRScheduleEmitter::emitSUsInBB(ArrayRef<SIRSchedUnit *> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry must be placed at the beginning!");

  BasicBlock *BB = SUs[0]->getParentBB();
  SIRSlot *EntrySlot = SM->getLandingSlot(BB);
  SIRSlot *ExitSlot = SM->getLatestSlot(BB);

  assert(EntrySlot && "Landing Slot not created?");
  assert(EntrySlot->getStepInLocalBB() == 0 && "Unexpected local step!");

  // The global schedule result of the Entry SUnit.
  unsigned EntrySUSchedule = SUs[0]->getSchedule();

  // Calculate the CriticalPathLength.
  unsigned CriticalPathLength = 0;
  for (unsigned i = 0; i < SUs.size(); ++i) {
    SIRSchedUnit *SU = SUs[i];
    unsigned ScheduleResult = SU->getSchedule();

    assert(ScheduleResult >= EntrySUSchedule && "Wrong Schedule Result!");
    unsigned PathLength = ScheduleResult - EntrySUSchedule;

    CriticalPathLength = (std::max)(CriticalPathLength, PathLength);
  }

  // Create New Slots for each step and collect them in order.
  SmallVector<SIRSlot *, 4> NewSlots;
  for (unsigned i = 0; i <= CriticalPathLength; ++i) {
    SIRSlot *NewSlot = C_Builder.createSlot(BB, i);

    NewSlots.push_back(NewSlot);
  }

  // Emit the SeqOps into the target Slot.
  for (unsigned i = 0; i < SUs.size(); ++i) {
    SIRSchedUnit *SU = SUs[i];

    if (SU->isSlotTransition())
      continue;

    unsigned TargetStep;

    // We should make sure the PHI is scheduled
    // to the ExitSlot.
    if (SU->isPHI() || SU->isPHIPack())
      TargetStep = CriticalPathLength;
    else
      TargetStep = SU->getSchedule() - EntrySUSchedule;

    SIRSlot *TargetSlot = NewSlots[TargetStep];

    ArrayRef<SIRSeqOp *> SeqOps = SU->getSeqOps();
    for (unsigned j = 0; j < SeqOps.size(); ++j) {
      SIRSeqOp *SeqOp = SeqOps[j];

      SeqOp->setSlot(TargetSlot);
    }
  }

  // Inherit the Prevs of the origin EntrySlot.
  SmallVector<SIRSlot *, 4> UnlinkPreds;
  typedef SIRSlot::pred_iterator pred_iterator;
  for (pred_iterator I = EntrySlot->pred_begin(), E = EntrySlot->pred_end();
       I != E; ++I) {
    SIRSlot *Pred = I->getSlot();

    // The edge is coming from current BB that means this is the loop edge.
    // we should handle it specially.
    if (Pred->getParent() == BB) {
      assert(Pred == ExitSlot && "Unexpected Loop Edge!");

      if (SM->IsBBPipelined(BB)) {
        unsigned II = SM->getMIIOfPipelinedBB(BB);

        Value *LoopCnd = G.getLoopSU(BB)->getSeqOp()->getGuard();
        SIRSlot *SlotBeforeNextIteration = C_Builder.advanceToNextSlot(NewSlots.front(), II - 1);
        C_Builder.createStateTransition(SlotBeforeNextIteration, NewSlots.front(), LoopCnd);
      } else {
        // We should inherit the condition of this loop edge.
        C_Builder.createStateTransition(NewSlots.back(), NewSlots.front(),
                                        I->getCnd());
      }

      // Collect the Preds should be unlink.
      UnlinkPreds.push_back(Pred);
      continue;
    }

    // Link the edge from the Pred to NewEntrySlot.
    C_Builder.createStateTransition(Pred, NewSlots.front(), I->getCnd());
    // Collect the Preds should be unlink.
    UnlinkPreds.push_back(Pred);
  }
  // Unlink the origin edge.
  for (unsigned i = 0; i < UnlinkPreds.size(); ++i)
    UnlinkPreds[i]->unlinkSucc(EntrySlot);

  // Create the Slot Transition inside the New Slots.
  for (unsigned i = 0; i < NewSlots.size() - 1; ++i) {
    // Create the slot transition from last slot to this slot.
    SIRSlot *S = NewSlots[i];
    SIRSlot *SuccSlot = NewSlots[i + 1];

    C_Builder.createStateTransition(S, SuccSlot,
                                    C_Builder.createIntegerValue(1, 1));
  }

  // Inherit the Succs of the origin ExitSlot.
  SmallVector<SIRSlot *, 4> UnlinkSuccs;
  typedef SIRSlot::succ_iterator succ_iterator;
  for (succ_iterator I = ExitSlot->succ_begin(), E = ExitSlot->succ_end();
       I != E; ++I) {
    SIRSlot *Succ = I->getSlot();
    Value *Cnd = I->getCnd();

    // Ignore the slot transition inside this Loop BB since this will only
    // happen when we transition from ExitSlot to EntrySlot, which we already
    // handled in previous step.
    if (Succ->getParent() == BB)
      continue;

    if (SM->IsBBPipelined(BB)) {
      unsigned II = SM->getMIIOfPipelinedBB(BB);

      SIRSlot *EntryS = NewSlots.front();
      SIRSlot *ExitS = NewSlots.back();

      for (int i = 0; i < CriticalPathLength - II + 1; ++i) {
        SIRRegister *CndReg = C_Builder.createRegister(Cnd->getName().data() + utostr_32(i), Cnd->getType(), BB);
        SIRSlot *AssignSlot = C_Builder.advanceToNextSlot(EntryS, II - 1);

        C_Builder.assignToReg(AssignSlot, C_Builder.createIntegerValue(1, 1), Cnd, CndReg);
        Cnd = CndReg->getLLVMValue();
      }
    }

    // Link the edge from the NewExitSlot to Succ.
    C_Builder.createStateTransition(NewSlots.back(), Succ, Cnd);
    // Collect the Succs should be unlink.
    UnlinkSuccs.push_back(Succ);
  }
  // Unlink the origin edge.
  for (unsigned i = 0; i < UnlinkSuccs.size(); ++i)
    ExitSlot->unlinkSucc(UnlinkSuccs[i]);

  // Index the new EntrySlot/ExitSlot of this BB.
  SM->IndexBB2Slots(BB, NewSlots.front(), NewSlots.back());
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

    emitSUsInBB(G.getSUsInBB(BB));
  }

  // Delete all useless components and re-name Slots in order.
  gc();
}

void SIRScheduleEmitter::gc() {
  // Delete all the old & useless Slot.
  bool Changed = true;
  while (Changed) {
    Changed = false;

    typedef SIR::slot_iterator slot_iterator;
    for (slot_iterator I = SM->slot_begin(), E = SM->slot_end(); I != E;) {
      SIRSlot *S = I++;

      if (S->pred_size() == 0) {
        S->unlinkSuccs();
        SM->deleteUselessSlot(S);

        Changed = true;
      }
    }
  }

  SIRSlot *StartSlot = SM->slot_begin();
  unsigned SlotNum = 0;
  ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> > RPO(StartSlot);
  typedef
    ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >::rpo_iterator
    slot_iterator;

  // Visit the SIRSlots in reverse post order to re-name them in order.
  for (slot_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    SIRSlot *S = *I;
    S->resetNum(SlotNum++);
  }
}