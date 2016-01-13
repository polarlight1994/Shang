#include "sir/SIRScheduling.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ErrorHandling.h"

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
  SIRSlot *ExitSlot = SM->getLatestSlot(BB);
  SIRSlot *EntrySlot = SM->getLandingSlot(BB);

  ArrayRef<SIRSchedUnit *> SUsInBB = G->getSUsInBB(BB);

  /// First constraint the Branch SUnits and PHI SUnits into the
  /// last step of this BB.
  ArrayRef<SIRSchedUnit *> SUsInExitSlot = G->lookupSUs(ExitSlot);

  for (unsigned i = 0; i < SUsInExitSlot.size(); ++i) {
    SIRSchedUnit *SU = SUsInExitSlot[i];

    if (!SU->isSlotTransition() && !SU->isExitSlotPack())
      continue;

    for (unsigned j = 0; j < SUsInBB.size(); ++j) {
      SIRSchedUnit *DepSU = SUsInBB[j];

      // No need to constraint to itself.
      if (DepSU == SU) continue;

      SU->addDep(DepSU, SIRDep::CreateSyncDep());
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
  // as the SeqVal.
  if (!Inst->getNumOperands()) return;
  ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(Inst->getOperand(0));

  // The ExitSUnit is depended on these SUnits.
  SIRSchedUnit *Exit = G->getExit();
  for (unsigned i = 0; i < SUs.size(); i++)
    Exit->addDep(SUs[i], SIRDep::CreateSyncDep());
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
  SIRTimingAnalysis::ArrivalMap Arrivals;

// Set to multi-cycle-chain mode.
//   if (U->isCombSU()) {
//     Instruction *CombOp = U->getCombOp();
//     assert(CombOp && "Unexpected NULL CombOp!");
// 
//     TA->extractArrivals(TD, CombOp, Arrivals);
//   } else {
//     SIRSeqOp *SeqOp = U->getSeqOp();
//     assert(SeqOp && "Unexpected NULL SeqOp!");
// 
//     TA->extractArrivals(TD, SeqOp, Arrivals);
//   }

  SIRSeqOp *SeqOp = U->getSeqOp();
  assert(SeqOp && "Unexpected NULL SeqOp!");

  TA->extractArrivals(SM, SeqOp, Arrivals);

  typedef SIRTimingAnalysis::ArrivalMap::iterator iterator;
  for (iterator I = Arrivals.begin(), E = Arrivals.end(); I != E; ++I) {
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

      if ((SrcSU->isPHI() || SrcSU->isPHIPack() || SrcSU->isExitSlotPack()) &&
          SrcSU->getParentBB() == U->getParentBB())
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

  // Get the source slot.
  SIRSlot *SrcSlot = SST->getSrcSlot();
  // Get the destination slot.
  SIRSlot *DstSlot = SST->getDstSlot();

  // The SlotTransitions in DstSlot are depended on all SUnits in SrcSlot.
  ArrayRef<SIRSchedUnit *> SUsInSrcSlot = G->lookupSUs(SrcSlot);

  // All SUnits in destination slot are depended on this SlotTransition.
  ArrayRef<SIRSchedUnit *> SUsInDstSlot = G->lookupSUs(DstSlot);

  // If the fist SUnit in destination slot is a BBEntry, that means we
  // are transiting to successor BB. In this circumstance, we can just
  // constraint the BBEntry, since other SUnits is constrained by the
  // BBEntry already. By doing this, we can ensure that all control
  // edges between BBs are ended on the BBEntry which is easy to handle
  // later especially when it is a back-edge.
  SIRSchedUnit *FirstSUInDstSlot = SUsInDstSlot[0];
  if (FirstSUInDstSlot->isBBEntry() || FirstSUInDstSlot->isEntry()) {
    FirstSUInDstSlot->addDep(U, SIRDep::CreateCtrlDep(0));

    // If we are transiting back to the beginning of this BB, then
    // we are handling a loop BB. So index the loop BB and this
    // corresponding loop SU here.
    if (FirstSUInDstSlot->isBBEntry() && DstSlot->getParent() == ParentBB)
      G->indexLoopSU2LoopBB(U, ParentBB);
  }
  // Or we are transition to the next slot in same BB. In this circumstance,
  // all SUnit in next slot is depended on the SlotTransition.
  else {
    for (unsigned i = 0; i < SUsInDstSlot.size(); i++) {
      SIRSchedUnit *SU = SUsInDstSlot[i];

//       // This will be handled later.
//       if (SU->isSlotTransition()) continue;

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

        AssignToResultSU->addDep(SU, SIRDep::CreateMemDep(Bank->getReadLatency()));
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

        int Latency = 0;
        if (isa<LoadInst>(MemInst) && isa<LoadInst>(Inst))
          Latency = 2;
        if (isa<LoadInst>(MemInst) && isa<StoreInst>(Inst))
          Latency = 2;
        if (isa<StoreInst>(MemInst) && isa<LoadInst>(Inst))
          Latency = 1;
        if (isa<StoreInst>(MemInst) && isa<StoreInst>(Inst))
          Latency = 1;

        for (unsigned j = 0; j < SeqOps.size(); ++j) {
          SIRSeqOp *SeqOp = SeqOps[j];
          SIRSchedUnit *SU = G->lookupSU(SeqOp);

          for (unsigned k = 0; k < OtherSeqOps.size(); ++k) {
            SIRSeqOp *OtherSeqOp = OtherSeqOps[k];
            SIRSchedUnit *OtherSU = G->lookupSU(OtherSeqOp);

              SU->addDep(OtherSU, SIRDep::CreateMemDep(Latency));
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

    // Ignore the LoopBB since we will try to pipeline it later.
    if (G->hasLoopSU(BB))
      continue;

    SmallVector<SIRSeqOp *, 4> ExitSlotSeqOpsPack;

    ArrayRef<SIRSchedUnit *> SUs = G->getSUsInBB(BB);
    for (unsigned i = 0; i < SUs.size(); ++i) {
      SIRSchedUnit *SU = SUs[i];

      if (SU->isBBEntry())
        continue;

      if (SU->isCombSU())
        continue;

      // Constraint the PHINodes necessary into the last step.
      if (SU->isPHI()) {
        ExitSlotSeqOpsPack.push_back(SU->getSeqOp());
        continue;
      }

      // All SlotTransition in ExitSlot should be scheduled to last step.
      if (SU->isSlotTransition() &&
          SU->getSeqOp()->getSlot() == SM->getLatestSlot(BB)) {
        ExitSlotSeqOpsPack.push_back(SU->getSeqOp());
        continue;
      }

      // All SeqOps assign to Output Register should be scheduled to last step.
      if (SU->getSeqOp()->getDst()->isOutPort())
        ExitSlotSeqOpsPack.push_back(SU->getSeqOp());
    }

    if (ExitSlotSeqOpsPack.size()) {
      SIRSchedUnit *ExitSlotPack
        = buildSchedulingUnitsPack(BB, ExitSlotSeqOpsPack, SIRSchedUnit::ExitSlotPack);
    }
  }

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    typedef BasicBlock::iterator iterator;
    for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
      Instruction *Inst = I;
      if (!isLoadStore(Inst)) continue;

      SIRMemoryBank *Bank;
      ArrayRef<SIRSeqOp *> SeqOps = SM->lookupMemSeqOps(Inst);
      SmallVector<SIRSeqOp *, 4> SeqOpsPack;

      if (LoadInst *LI = dyn_cast<LoadInst>(Inst)) {
        Bank = SA->getMemoryBank(*LI);

        for (unsigned i = 0; i < SeqOps.size() - 1; ++i)
          SeqOpsPack.push_back(SeqOps[i]);

        assert(SeqOps.size() && "Unexpected empty SeqOps!");

        SIRSchedUnit *LoadPackSU
          = buildSchedulingUnitsPack(BB, SeqOpsPack, SIRSchedUnit::MemoryPack);

        // Index the LoadPackSU and its corresponding MemoryBank.
        G->indexMemoryBank2SUnit(Bank, LoadPackSU);
      }
      else if (StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
        Bank = SA->getMemoryBank(*SI);

        for (unsigned i = 0; i < SeqOps.size(); ++i)
          SeqOpsPack.push_back(SeqOps[i]);

        assert(SeqOps.size() && "Unexpected empty SeqOps!");

        SIRSchedUnit *StorePackSU
          = buildSchedulingUnitsPack(BB, SeqOpsPack, SIRSchedUnit::MemoryPack);

        // Index the StorePackSU and its corresponding MemoryBank.
        G->indexMemoryBank2SUnit(Bank, StorePackSU);
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

void SIRScheduling::buildSchedulingUnitsForCombOp(Instruction *CombOp) {
  BasicBlock *BB = CombOp->getParent();

  SIRSchedUnit *U = G->createSUnit(BB, SIRSchedUnit::CombSU, CombOp);

  // Index the SUnit to the LLVM IR.
  G->indexSU2IR(U, CombOp);
}

void SIRScheduling::finishBuildingSchedGraph() {
  // GC: delete all useless SUnit.
  G->gc();

  // Handle the Exit.
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
    BasicBlock *BB = I;

    // Ignore the Loop BB since we will try to pipeline it.
    if (G->hasLoopSU(BB))
      continue;

    constraintTerminators(BB);
  }
}

void SIRScheduling::buildSchedulingGraph() {
  SIRSchedUnit *Entry = G->getEntry();
  SIRSlot *StartSlot = SM->slot_begin();

  // Index the Entry SUnit to the StartSlot.
  G->indexSU2Slot(Entry, StartSlot);

  // Build the Scheduling Units for SeqOps.
  {
    ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >
      RPO(SM->slot_begin());
    typedef
      ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >::rpo_iterator
      slot_iterator;

    // Visit the SIRSlots in reverse post order so that the building order of
    // SUnits is topological generally to avoid creating a dependency to a
    // SUnit which is not created yet when building dependencies.
    for (slot_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
      buildSchedulingUnitsForSeqOp(*I);
  }

//   // Build the Scheduling Units for CombOps.
//   {
//     Function *F = SM->getFunction();
// 
//     typedef Function::iterator bb_iterator;
//     for (bb_iterator BBI = F->begin(), BBE = F->end(); BBI != BBE; ++BBI) {
//       BasicBlock *BB = BBI;
// 
//       typedef BasicBlock::iterator inst_iterator;
//       for (inst_iterator InstI = BB->begin(), InstE = BB->end();
//            InstI != InstE; ++InstI) {
//         Instruction *Inst = InstI;
// 
//         if (!isa<IntrinsicInst>(Inst) && !isa<IntToPtrInst>(Inst) &&
//             !isa<PtrToIntInst>(Inst) && !isa<BitCastInst>(Inst))
//           continue;
// 
//         if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst))
//           if (II->getIntrinsicID() == Intrinsic::shang_reg_assign)
//             continue;
// 
//         buildSchedulingUnitsForCombOp(Inst);
//       }
//     }
//   }

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
    case SIRSchedUnit::CombSU: {
      Instruction *CombOp = U->getCombOp();
      Output << "CombSU    ";
      Output << "Operation [";
      CombOp->print(Output);
      Output << "]\n";

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
    case SIRSchedUnit::ExitSlotPack: {
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
      errs() << "Pipelined BB " << BB->getName() << " in II of "
             << IMS.getMII() << "\n";

      SM.IndexPipelinedBB2MII(BB, IMS.getMII());
    }
    // If we fail to pipeline the BB, then we need to constraint the PHI
    // and OutPort SeqOps like normal BB to make sure they are scheduled
    // to last step.
    else {
      errs() << "Failed to pipeline BB " << BB->getName() << "\n";

      SmallVector<SIRSeqOp *, 4> ExitSlotSeqOpsPack;

      ArrayRef<SIRSchedUnit *> SUs = G->getSUsInBB(BB);
      for (unsigned i = 0; i < SUs.size(); ++i) {
        SIRSchedUnit *SU = SUs[i];

        if (SU->isBBEntry())
          continue;

        if (SU->isCombSU())
          continue;

        // Constraint the PHINodes necessary into the last step.
        if (SU->isPHI()) {
          ExitSlotSeqOpsPack.push_back(SU->getSeqOp());
          continue;
        }

        // All SlotTransition in ExitSlot should be scheduled to last step.
        if (SU->isSlotTransition() &&
            SU->getSeqOp()->getSlot() == SM.getLatestSlot(BB)) {
          ExitSlotSeqOpsPack.push_back(SU->getSeqOp());
          continue;
        }

        // All SeqOps assign to Output Register should be scheduled to last step.
        if (SU->getSeqOps().size() == 1 && SU->getSeqOp()->getDst()->isOutPort())
          ExitSlotSeqOpsPack.push_back(SU->getSeqOp());
      }

      if (ExitSlotSeqOpsPack.size()) {
        SIRSchedUnit *ExitSlotPack
          = buildSchedulingUnitsPack(BB, ExitSlotSeqOpsPack, SIRSchedUnit::ExitSlotPack);
      }

      G->gc();

      // Also need to constraint the terminators.
      constraintTerminators(BB);

      // Also we need to re-sort the SUnits in SchedGraph.
      G->topologicalSortSUs();
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
  float EntrySUSchedule = SUs[0]->getSchedule();
  assert(EntrySUSchedule - int(EntrySUSchedule) == 0 && "Should not be a real float!");

  // Calculate the CriticalPathLength.
  unsigned CriticalPathLength = 0;
  for (unsigned i = 0; i < SUs.size(); ++i) {
    SIRSchedUnit *SU = SUs[i];

    // Ignore the CombSU here, since the critical path
    // will decided by all SeqSU. The schedule result
    // of CombSU may not be correct to calculate the
    // critical path.
    if (SU->isCombSU()) continue;

    float ScheduleResult = SU->getSchedule();

    assert(ScheduleResult >= EntrySUSchedule && "Wrong Schedule Result!");
    unsigned PathLength = floor(ScheduleResult - EntrySUSchedule);

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

    if (SU->isSlotTransition() || SU->isCombSU())
      continue;

    unsigned TargetStep = floor(SU->getSchedule() - EntrySUSchedule);

    // We should make sure the PHI and OutPort SeqOps are scheduled to the ExitSlot.
    if (SU->isPHI() || SU->isPHIPack() || SU->isExitSlotPack() ||
        (SU->getSeqOps().size() == 1 && SU->getSeqOp()->getDst()->isOutPort()))
      assert(TargetStep == CriticalPathLength || SM->getMIIOfPipelinedBB(BB) && "Unexpected TargetStep!");

    SIRSlot *TargetSlot = NewSlots[TargetStep];

    ArrayRef<SIRSeqOp *> SeqOps = SU->getSeqOps();
    for (unsigned j = 0; j < SeqOps.size(); ++j) {
      SIRSeqOp *SeqOp = SeqOps[j];

      // Ignore the SlotTransition SeqOp hided in Pack.
      if (SeqOp->isSlotTransition())
        continue;

      SeqOp->setSlot(TargetSlot);
      TargetSlot->addSeqOp(SeqOp);
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

      for (unsigned i = 0; i < CriticalPathLength - II + 1; ++i) {
        SIRRegister *CndReg = C_Builder.createRegister(Cnd->getName().data() + utostr_32(i), Cnd->getType(), BB);
        SIRSlot *AssignSlot = NewSlots[II - 1 + i];

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

  // Pipeline the data-path in SIR.
  //pipelineDataPath();
}

void SIRScheduleEmitter::pipelineDataPath() {
//    std::string PipelineRegInfo = LuaI::GetString("PipelineRegInfo");
//    std::string Error;
//    raw_fd_ostream Output(PipelineRegInfo.c_str(), Error);
//
//    // Pipeline the argument value of module.
//    Function *F = SM->getFunction();
//    for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
//         I != E; ++I) {
//      Argument *Arg = I;
//      StringRef Name = Arg->getName();
//      Type *Ty = Arg->getType();
//      bool IsPointer = Ty->isPointerTy();
//      unsigned BitWidth = TD.getTypeSizeInBits(Ty);
// 
//      // Create a register to hold its value.
//      Value *Start = cast<SIRInPort>(SM->getPort(SIRPort::Start))->getValue();
//      SIRRegister *ArgReg = C_Builder.createRegister(Name.str()+ "_reg", C_Builder.createIntegerType(BitWidth));
//      C_Builder.assignToReg(SM->slot_begin(), Start, Arg, ArgReg);
// 
//      // Now pipeline the argument register.
//      Value *NewArgVal = ArgReg->getLLVMValue();
// 
//      // If it is pointer value, remember to change type.
//      if (IsPointer) {
//        Value *InsertPosition = SM->getPositionAtBackOfModule();
//        NewArgVal = D_Builder.createIntToPtrInst(NewArgVal, Ty, InsertPosition, true);
//      }
//      Arg->replaceAllUsesWith(NewArgVal);
// 
//      // Pipeline level of this argument.
//      unsigned PipelineDepth = 0;
//      // Pipeline registers of this argument.
//      std::vector<SIRRegister *> PipelineRegs;
//      // Map between pipelined register and corresponding use instruction.
//      std::map<unsigned, std::set<Instruction *> > PipelineReg2UseInst;
// 
//      typedef Value::use_iterator use_iterator;
//      for (use_iterator UI = NewArgVal->use_begin(), UE = NewArgVal->use_end(); UI != UE; ++UI) {
//        Value *UseVal = dyn_cast<Instruction>(*UI);
// 
//        if (Instruction *UseInst = dyn_cast<Instruction>(UseVal)) {
//          if (isa<IntToPtrInst>(UseVal) || isa<PtrToIntInst>(UseVal) ||
//              isa<BitCastInst>(UseVal) || isa<IntrinsicInst>(UseVal)) {
//            // Only pipeline the data-path.
//            if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(UseInst))
//              if (II->getIntrinsicID() == Intrinsic::shang_reg_assign)
//                continue;
// 
//            ArrayRef<SIRSchedUnit *> UseSUnits = G.lookupSUs(UseInst);
//            assert(UseSUnits.size() == 1 && "Unexpected SUnits size for this UseInst?");
//            SIRSchedUnit *UseSUnit = UseSUnits.front();
// 
//            int UseSchedResult = int(floor(UseSUnit->getSchedule()));
// 
//            assert(UseSchedResult > 0 && "Unexpected schedule result!");
//            unsigned PipelineLevel = UseSchedResult - 1;
//            /*PipelineLevel = PipelineLevel > 10 ? 10 : PipelineLevel;*/
// 
//            // Index the pipelined level of this UseInst.
//            if (PipelineLevel) {
//              bool Success = PipelineReg2UseInst[PipelineLevel].insert(UseInst).second;
//              assert(Success && "Unexpected failed!");
// 
//              PipelineDepth = PipelineDepth > PipelineLevel ? PipelineDepth : PipelineLevel;
//            }
//          }
//        }
//      }
// 
//      // Create the pipeline registers to pipeline the value in data-path.
//      Value *Val = ArgReg->getLLVMValue();
//      for (int i = 0; i < PipelineDepth; ++i) {
//        std::string PipelineRegName = Val->getName().str()+ "_pipeline_" + utostr_32(i);
//        SIRRegister *PipelineReg = C_Builder.createRegister(PipelineRegName, Val->getType(),
//                                                            0, 0, SIRRegister::Pipeline);
//        PipelineRegs.push_back(PipelineReg);
// 
//        // Value passed between pipeline registers in order.
//        C_Builder.assignToReg(0, C_Builder.createIntegerValue(1, 1), Val, PipelineReg);
//        Val = PipelineReg->getLLVMValue();
//      }
// 
//      // Reconstruct the data-path to make sure the UseInst use the correct pipeline register.
//      typedef std::map<unsigned, std::set<Instruction *> >::iterator map_iterator;
//      for (map_iterator MI = PipelineReg2UseInst.begin(), ME = PipelineReg2UseInst.end(); MI != ME; ++MI) {
//        unsigned PipelineLevel = MI->first;
//        SIRRegister *PipelineReg = PipelineRegs[PipelineLevel - 1];
// 
//        std::set<Instruction *> UseInsts = MI->second;
//        typedef std::set<Instruction *>::iterator inst_iterator;
//        for (inst_iterator II = UseInsts.begin(), IE = UseInsts.end(); II != IE; ++II) {
//          Instruction *UseInst = *II;
// 
//          unsigned OperandNum = UINT32_MAX;
//          for (int i = 0; i < UseInst->getNumOperands(); ++i) {
//            Value *Operand = UseInst->getOperand(i);
//            if (Operand == NewArgVal)
//              OperandNum = i;
//          }
//          assert(OperandNum != UINT32_MAX && "CombOp not used in UseInst?");
// 
//          // Replace the operand with pipeline register. And if the origin argument is pointer,
//          // remember to change type.
//          Value *PipelineRegVal = PipelineReg->getLLVMValue();
//          if (IsPointer) {
//            Value *InsertPosition = SM->getPositionAtBackOfModule();
//            PipelineRegVal = D_Builder.createIntToPtrInst(PipelineRegVal, Ty, InsertPosition, true);
//          }
//          UseInst->setOperand(OperandNum, PipelineRegVal);
//        }
//      }
// 
//        // Print debug information for these pipeline registers.
//        Output << "The Value pipelined is [" << NewArgVal->getName() << "], the pipeline depth is" << PipelineDepth << "\n";
//        for (map_iterator MI = PipelineReg2UseInst.begin(), ME = PipelineReg2UseInst.end(); MI != ME; ++MI) {
//          unsigned PipelineLevel = MI->first;
//          SIRRegister *PipelineReg = PipelineRegs[PipelineLevel - 1];
//  
//          Output << "In Level #" << PipelineLevel << ", pipeline register is " << PipelineReg->getName() << "\n";
//          Output << "The UseInsts is\n";
//  
//          std::set<Instruction *> UseInsts = MI->second;
//          typedef std::set<Instruction *>::iterator inst_iterator;
//          for (inst_iterator II = UseInsts.begin(), IE = UseInsts.end(); II != IE; ++II) {
//            Instruction *UseInst = *II;
//  
//            Output << "    " << UseInst->getName() << "\n";
//          }
//        }
//        Output << "\n";
//      }

  {
    typedef SIRSchedGraph::iterator iterator;
    for (iterator I = G.begin(), E= G.end(); I != E; ++I) {
      SIRSchedUnit *SU = I;

      // Only pipeline the data-path here.
      if (!SU->isCombSU()) continue;

      // Pipeline level of this data-path instruction.
      unsigned PipelineDepth = 0;
      // Pipeline registers of this data-path instruction.
      std::vector<SIRRegister *> PipelineRegs;
      // Map between pipelined register and corresponding use instruction.
      std::map<unsigned, std::set<Instruction *> > PipelineReg2UseInst;

      Instruction *CombOp = SU->getCombOp();
      int SchedResult = int(floor(SU->getSchedule()));

      typedef Instruction::use_iterator use_iterator;
      for (use_iterator UI = CombOp->use_begin(), UE = CombOp->use_end(); UI != UE; ++UI) {
        Value *UseVal = dyn_cast<Instruction>(*UI);

        if (Instruction *UseInst = dyn_cast<Instruction>(UseVal)) {
          if (isa<IntToPtrInst>(UseVal) || isa<PtrToIntInst>(UseVal) ||
              isa<BitCastInst>(UseVal) || isa<IntrinsicInst>(UseVal)) {
            // Only pipeline the data-path.
            if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(UseInst))
              if (II->getIntrinsicID() == Intrinsic::shang_reg_assign)
                continue;

            ArrayRef<SIRSchedUnit *> UseSUnits = G.lookupSUs(UseInst);
            assert(UseSUnits.size() == 1 && "Unexpected SUnits size for this UseInst?");
            SIRSchedUnit *UseSUnit = UseSUnits.front();

            int UseSchedResult = int(floor(UseSUnit->getSchedule()));

            /*assert(UseSchedResult >= SchedResult && "Unexpected schedule result!");*/
            if (UseSchedResult <= SchedResult)
              continue;

            unsigned PipelineLevel = UseSchedResult - SchedResult;

            // Index the pipelined level of this UseInst.
            if (PipelineLevel) {
              bool Success = PipelineReg2UseInst[PipelineLevel].insert(UseInst).second;
              //assert(Success && "Unexpected failed!");

              PipelineDepth = PipelineDepth > PipelineLevel ? PipelineDepth : PipelineLevel;
            }
          }
        }
      }

      // Create the pipeline registers to pipeline the value in data-path.
      Value *Val = CombOp;
      for (unsigned i = 0; i < PipelineDepth; ++i) {
        std::string PipelineRegName = Val->getName().str()+ "_pipeline_" + utostr_32(i);
        SIRRegister *PipelineReg = C_Builder.createRegister(PipelineRegName, Val->getType(),
                                                            CombOp->getParent(), 0,
                                                            SIRRegister::Pipeline);
        PipelineRegs.push_back(PipelineReg);

        // Value passed between pipeline registers in order.
        C_Builder.assignToReg(0, C_Builder.createIntegerValue(1, 1), Val, PipelineReg);
        Val = PipelineReg->getLLVMValue();
      }

      // Reconstruct the data-path to make sure the UseInst use the correct pipeline register.
      typedef std::map<unsigned, std::set<Instruction *> >::iterator map_iterator;
      for (map_iterator MI = PipelineReg2UseInst.begin(), ME = PipelineReg2UseInst.end(); MI != ME; ++MI) {
        unsigned PipelineLevel = MI->first;
        SIRRegister *PipelineReg = PipelineRegs[PipelineLevel - 1];

        std::set<Instruction *> UseInsts = MI->second;
        typedef std::set<Instruction *>::iterator inst_iterator;
        for (inst_iterator II = UseInsts.begin(), IE = UseInsts.end(); II != IE; ++II) {
          Instruction *UseInst = *II;

          unsigned OperandNum = UINT32_MAX;
          for (unsigned i = 0; i < UseInst->getNumOperands(); ++i) {
            Value *Operand = UseInst->getOperand(i);
            if (Operand == CombOp)
              OperandNum = i;
          }
          assert(OperandNum != UINT32_MAX && "CombOp not used in UseInst?");

          // Replace the operand with pipeline register.
          UseInst->setOperand(OperandNum, PipelineReg->getLLVMValue());
        }
      }
// 
//       if (PipelineDepth) {
//         // Print debug information for these pipeline registers.
//         Output << "The Value pipelined is [" << CombOp->getName() << "], the pipeline depth is " << PipelineDepth << "\n";
//         for (map_iterator MI = PipelineReg2UseInst.begin(), ME = PipelineReg2UseInst.end(); MI != ME; ++MI) {
//           unsigned PipelineLevel = MI->first;
//           SIRRegister *PipelineReg = PipelineRegs[PipelineLevel - 1];
// 
//           Output << "In Level #" << PipelineLevel << ", pipeline register is " << PipelineReg->getName() << "\n";
//           Output << "The UseInsts is\n";
// 
//           std::set<Instruction *> UseInsts = MI->second;
//           typedef std::set<Instruction *>::iterator inst_iterator;
//           for (inst_iterator II = UseInsts.begin(), IE = UseInsts.end(); II != IE; ++II) {
//             Instruction *UseInst = *II;
// 
//             Output << "    " << UseInst->getName() << "\n";
//           }
//         }
//         Output << "\n";
//       }
    }
  }
}

void SIRScheduleEmitter::gc() {
  // Delete all the useless Slot.
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

  typedef SIR::seqop_iterator seqop_iterator;
  for (seqop_iterator SI = SM->seqop_begin(), SE = SM->seqop_end(); SI != SE;) {
    SIRSeqOp *SeqOp = SI++;

    if (SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(SeqOp)) {
      SIRSlot *SrcSlot = SST->getSrcSlot();
      SIRSlot *DstSlot = SST->getDstSlot();

      if (!SrcSlot->hasNextSlot(DstSlot))
        SM->deleteUselessSeqOp(SeqOp);
    }
  }

  // Visit the SIRSlots in reverse post order to re-name them in order.
  SIRSlot *StartSlot = SM->slot_begin();
  unsigned SlotNum = 0;
  ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> > RPO(StartSlot);
  typedef
    ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >::rpo_iterator
    slot_iterator;
  for (slot_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    SIRSlot *S = *I;
    S->resetNum(SlotNum++);
  }
}