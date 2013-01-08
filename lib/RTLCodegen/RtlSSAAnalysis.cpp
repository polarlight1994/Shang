//===- RtlSSAAnalysis.cpp - Analyse the dependency between registers - C++ --=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass collect the slots information of register and map them into a map
// vector. then it will analyse dependency between registers.
//
//
//===----------------------------------------------------------------------===//

#include "RtlSSAAnalysis.h"
#include "vtm/Passes.h"
#include "vtm/VFInfo.h"
#include "vtm/VASTModule.h"
#include "vtm/VerilogModuleAnalysis.h"
#include "vtm/VerilogBackendMCTargetDesc.h"

#include "llvm/Target/TargetData.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vtm-rtl-ssa"
#include "llvm/Support/Debug.h"

#include <map>

using namespace llvm;

namespace llvm {
template<>
struct DOTGraphTraits<RtlSSAAnalysis*> : public DefaultDOTGraphTraits{
  typedef VASTSlot NodeTy;
  typedef RtlSSAAnalysis GraphTy;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  std::string getNodeLabel(const NodeTy *Node, const GraphTy *Graph) {
    std::string Str;
    raw_string_ostream ss(Str);
    ss << Node->getName();
    DEBUG(
      SlotInfo * SI = Graph->getSlotInfo(Node);
      SI->print(ss););

    return ss.str();
  }

  static std::string getNodeAttributes(const NodeTy *Node,
                                       const GraphTy *Graph) {
      return "shape=Mrecord";
  }
};
}

void RtlSSAAnalysis::viewGraph() {
  ViewGraph(this, "CompatibilityGraph" + utostr_32(ID));
}

// Helper class
struct VASDepBuilder {
  RtlSSAAnalysis &A;
  ValueAtSlot *DstVAS;
  VASDepBuilder(RtlSSAAnalysis &RtlSSA, ValueAtSlot *V) : A(RtlSSA), DstVAS(V) {}

  void operator() (ArrayRef<VASTValue*> PathArray) {
    VASTValue *SrcUse = PathArray.back();
    if (VASTSeqValue *Src = dyn_cast_or_null<VASTSeqValue>(SrcUse))
      A.addVASDep(DstVAS, Src);
  }
};

std::string ValueAtSlot::getName() const {
  std::string Name = std::string(getValue()->getName())
                     + "@" + utostr_32(getSlot()->SlotNum);

  if (MachineBasicBlock *ParentBB = getSlot()->getParentBB())
    Name += "#" + utostr_32(ParentBB->getNumber());

  return Name;
}

void ValueAtSlot::print(raw_ostream &OS, unsigned Ind) const {
  OS.indent(Ind) << getName() << "\t <= {";

  typedef VASCycMapTy::const_iterator it;
  for (it I = DepVAS.begin(), E = DepVAS.end(); I != E; ++I) {
    OS.indent(Ind + 2) << '[' << I->second.getCycles() << ']';
    if (Ind > 2) OS << I->first->getName() << ',';
    else {
      OS  << "{\n";
      I->first->print(OS, Ind + 4);
      OS.indent(Ind + 2) << "}\n";
    }
  }

  OS.indent(Ind) << "}\n";

  if (DefMI) OS.indent(Ind + 2) << *DefMI << '\n';
}

void ValueAtSlot::verify() const {
  typedef VASCycMapTy::const_iterator it;
  VASTSlot *UseSlot = getSlot();
  for (it I = DepVAS.begin(), E = DepVAS.end(); I != E; ++I) {
    VASTSlot *DefSlot = I->first->getSlot();
    LiveInInfo LI = I->second;
    if (DefSlot->getParentBB() == UseSlot->getParentBB() &&
        UseSlot->hasAliasSlot() && LI.getCycles() > DefSlot->alias_ii()) {
      if (const MachineInstr *MI = I->first->getDefMI()) {
        // The value comes from others BB, it is loop-invariant.
        if (MI->getOpcode() == VTM::VOpMvPhi
            && MI->getOperand(2).getMBB() != MI->getParent())
          continue;
      }

      llvm_unreachable("Broken RTL dependence!");
    }
  }
}

void ValueAtSlot::dump() const {
  print(dbgs());
}

void SlotInfo::dump() const {
  print(dbgs());
}

void SlotInfo::print(raw_ostream &OS) const {
  OS << S->getName() << "\nGen:\n";
  for (gen_iterator I = gen_begin(), E = gen_end(); I != E; ++I) {
    ValueAtSlot *VAS = *I;
    OS.indent(2) << VAS->getName() << "\n";
  }

  OS << "\n\nIn:\n";
  for (VASCycMapTy::const_iterator I = in_begin(), E = in_end(); I != E; ++I) {
    ValueAtSlot *VAS = I->first;
    OS.indent(2) << VAS->getName() << '[' << I->second.getCycles() << "]\n";
  }

  OS << "\n\n";
}

// Any VAS whose value is overwritten at this slot is killed.
bool SlotInfo::isVASKilled(const ValueAtSlot *VAS) const {
  return OverWrittenValue.count(VAS->getValue());
}

void SlotInfo::initOutSet() {
  // Build the initial out set ignoring the kill set.
  for (gen_iterator I = gen_begin(), E = gen_end(); I != E; ++I)
    SlotOut.insert(std::make_pair(*I, ValueAtSlot::LiveInInfo()));
}

RtlSSAAnalysis::RtlSSAAnalysis() : MachineFunctionPass(ID), VM(0) {
  initializeRtlSSAAnalysisPass(*PassRegistry::getPassRegistry());
}


void RtlSSAAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
  AU.addRequired<VerilogModuleAnalysis>();
  AU.setPreservesAll();
}

bool RtlSSAAnalysis::runOnMachineFunction(MachineFunction &MF) {
  VM = getAnalysis<VerilogModuleAnalysis>().getModule();

  // Push back all the slot into the SlotVec for the purpose of view graph.
  typedef VASTModule::slot_iterator slot_it;
  for (slot_it I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I) {
    VASTSlot *S = *I;
    // If the VASTslot is void, abandon it.
    if (!S) continue;

    SlotVec.push_back(S);
    // Create a new SlotInfo if it is not defined before.
    SlotInfo *SI = new (Allocator) SlotInfo(S);
    bool inserted = SlotInfos.insert(std::make_pair(S, SI)).second;
    assert(inserted && "SlotInfo inserted?");
    (void) inserted;
  }

  // Define the VAS.
  buildAllVAS();

  ComputeReachingDefinition();

  DEBUG(viewGraph());

  return false;
}

ValueAtSlot *RtlSSAAnalysis::getValueASlot(VASTSeqValue *V, VASTSlot *S){
  ValueAtSlot *VAS = UniqueVASs.lookup(std::make_pair(V, S));
  assert(VAS && "VAS not exist!");
  return VAS;
}

SlotInfo *RtlSSAAnalysis::getSlotInfo(const VASTSlot *S) const {
  slotinfo_it It = SlotInfos.find(S);
  assert(It != SlotInfos.end() && "SlotInfo not exist!");
  return It->second;
}

void RtlSSAAnalysis::addVASDep(ValueAtSlot *VAS, VASTSeqValue *DepVal) {
  VASTSlot *UseSlot = VAS->getSlot();
  SlotInfo *UseSI = getSlotInfo(UseSlot);
  assert(UseSI && "SlotInfo missed!");

  for (vn_itertor I = DepVal->begin(), E = DepVal->end(); I != E; ++I) {
    VASTSlot *DefSlot = I->first.getSlot();
    ValueAtSlot *DefVAS = getValueASlot(DepVal, DefSlot);

    ValueAtSlot::LiveInInfo LI = UseSI->getLiveIn(DefVAS);

    // VAS is only depends on DefVAS if it can reach this slot.
    if (LI.getCycles())
      VAS->addDepVAS(DefVAS, LI);
  }
}

void RtlSSAAnalysis::buildAllVAS() {
  typedef VASTModule::seqval_iterator it;
  for (it I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I){
    VASTSeqValue *V = *I;

    for (vn_itertor I = V->begin(), E = V->end(); I != E; ++I){
      VASTSlot *S = I->first.getSlot();
      MachineInstr *DefMI = I->first.getDefMI();
      // Create the origin VAS.
      ValueAtSlot *VAS = new (Allocator) ValueAtSlot(V, S, DefMI);
      UniqueVASs.insert(std::make_pair(std::make_pair(V, S), VAS));
    }
  }
}

void RtlSSAAnalysis::verifyRTLDependences() const {
  for (const_vas_iterator I = vas_begin(), E = vas_end(); I != E; ++I)
    (*I)->verify();
}

void RtlSSAAnalysis::buildVASGraph() {
  typedef VASTModule::seqval_iterator it;
  for (it I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I) {
    VASTSeqValue *V = *I;

    for (vn_itertor I = V->begin(), E = V->end(); I != E; ++I) {
      VASTSlot *S = I->first.getSlot();
      // Create the origin VAS.
      ValueAtSlot *VAS = getValueASlot(V, S);
      // Build dependence for conditions
      visitDepTree(I->first.getAsLValue<VASTValue>(), VAS);
      // Build dependence for the assigning value.
      visitDepTree(I->second->getAsLValue<VASTValue>(), VAS);
    }
  }
}

// Helper functions
// Traverse the use tree to get the registers.
template<typename VisitPathFunc>
static void DepthFirstTraverseDepTree(VASTValue *DepTree, VisitPathFunc VisitPath) {
  typedef VASTValue::dp_dep_it ChildIt;
  // Use seperate node and iterator stack, so we can get the path vector.
  typedef SmallVector<VASTValue*, 16> NodeStackTy;
  typedef SmallVector<ChildIt, 16> ItStackTy;
  NodeStackTy NodeWorkStack;
  ItStackTy ItWorkStack;
  // Remember what we had visited.
  std::set<VASTValue*> VisitedUses;

  // Put the root.
  NodeWorkStack.push_back(DepTree);
  ItWorkStack.push_back(VASTValue::dp_dep_begin(DepTree));

  while (!ItWorkStack.empty()) {
    VASTValue *Node = NodeWorkStack.back();

    ChildIt It = ItWorkStack.back();

    // Do we reach the leaf?
    if (VASTValue::is_dp_leaf(Node)) {
      VisitPath(NodeWorkStack);
      NodeWorkStack.pop_back();
      ItWorkStack.pop_back();
      continue;
    }

    // All sources of this node is visited.
    if (It == VASTValue::dp_dep_end(Node)) {
      NodeWorkStack.pop_back();
      ItWorkStack.pop_back();
      continue;
    }

    // Depth first traverse the child of current node.
    VASTValue *ChildNode = (*It).get().get();
    ++ItWorkStack.back();

    // Had we visited this node? If the Use slots are same, the same subtree
    // will lead to a same slack, and we do not need to compute the slack agian.
    if (!VisitedUses.insert(ChildNode).second) continue;

    // If ChildNode is not visit, go on visit it and its childrens.
    NodeWorkStack.push_back(ChildNode);
    ItWorkStack.push_back(VASTValue::dp_dep_begin(ChildNode));
  }
}

void RtlSSAAnalysis::visitDepTree(VASTValue *DepTree, ValueAtSlot *VAS){
  VASTValue *DefValue = DepTree;

  // If Define Value is immediate or symbol, skip it.
  if (!DefValue) return;

  // If the define Value is register, add the dependent VAS to the
  // dependentVAS.
  if (VASTSeqValue *DepVal = dyn_cast<VASTSeqValue>(DefValue)){
    addVASDep(VAS, DepVal);
    return;
  }

  VASDepBuilder B(*this, VAS);
  // If the define Value is wire, traverse the use tree to get the
  // ultimate registers.
  DepthFirstTraverseDepTree(DepTree, B);
}

bool RtlSSAAnalysis::addLiveIns(SlotInfo *From, SlotInfo *To,
                                bool FromAliasSlot) {
  bool Changed = false;
  typedef SlotInfo::vascyc_iterator it;
  // Store the slot numbers in signed integer, we will perform subtraction on
  // them and may produce negative result.
  int FromSlotNum = From->getSlot()->SlotNum,
      ToSlotNum = To->getSlot()->SlotNum;
  MachineBasicBlock *ToBB = To->getSlot()->getParentBB();
  MachineBasicBlock *FromBB = From->getSlot()->getParentBB();
  bool FromLaterAliasSlot = FromAliasSlot && FromSlotNum > ToSlotNum;

  for (it I = From->out_begin(), E = From->out_end(); I != E; ++I) {
    ValueAtSlot *PredOut = I->first;
    VASTSeqValue *V = PredOut->getValue();

    VASTSlot *DefSlot = PredOut->getSlot();
    ValueAtSlot::LiveInInfo LI = I->second;

    bool IsDefSlot = LI.getCycles() == 0;
    // Increase the cycles by 1 after the value lives to next slot.
    LI.incCycles();

    bool LiveInToSlot = !FromLaterAliasSlot || V->isTimingUndef();
    bool LiveOutToSlot = !To->isVASKilled(PredOut);

    // Check if the register is killed according MachineFunction level liveness
    // information.
    switch (V->getValType()) {
    case VASTSeqValue::Data: {
      unsigned RegNum = V->getDataRegNum();
      // The registers are not propagate from the slot to its alias slot.
      if (RegNum && FromAliasSlot) LiveInToSlot = false;

      const MachineInstr *DefMI = PredOut->getDefMI();
      // Do not add live out if data register R not live in the new BB.
      if (RegNum && DefMI) {
        if (DefMI->getOpcode() == VTM::VOpMvPhi && IsDefSlot) {
          // The register is defined by the VOpMvPhi at FromSlot.
          // However, the copy maybe disabled when jumping to ToBB.
          // Then this define is even not live-in ToSlot, we can simply skip the
          // rest of the code.
          if (DefMI->getOperand(2).getMBB() != ToBB)
            continue;
        }

        // If the FromSlot is bigger than the ToSlot, then we are looping back.
        bool ToNewBB = FromBB != ToBB || FromSlotNum >= ToSlotNum;

        // The register may be defined at the first slot of current BB, which
        // slot is alias with the last slot of Current BB's predecessor. In this
        // case we should not believe the live-ins information of the BB.
        // And if the register are not defined at the current slot (the last
        // slot of current BB) then we can trust the live-ins information.
        bool ShouldTrustBBLI = ToNewBB
                               && (!IsDefSlot || DefMI->getParent() == FromBB);

        if (ShouldTrustBBLI && !ToBB->isLiveIn(RegNum))
          LiveOutToSlot = false;
      }
      break;
    }
    case VASTSeqValue::Slot:
      // Ignore the assignment that reset the slot enable register, even the
      // signal may take more than one cycles to propagation, the shortest path
      // should be the path that propagating the "1" value of the enable
      // register.
      if (V->getSlotNum() == DefSlot->SlotNum && LI.getCycles() > 1)
        LiveOutToSlot = false;
      break;
    default: break;
    }

    if (!LiveInToSlot) continue;

    Changed |= To->insertIn(PredOut, LI);

    if (!LiveOutToSlot) continue;

    Changed |= To->insertOut(PredOut, LI);
  }

  return Changed;
}

bool RtlSSAAnalysis::addLiveInFromAliasSlots(VASTSlot *From, SlotInfo *To) {
  bool Changed = false;
  unsigned FromSlotNum = From->SlotNum;

  for (unsigned i = From->alias_start(), e = From->alias_end(),
       ii = From->alias_ii(); i < e; i += ii) {
    if (i == FromSlotNum) continue;

    SlotInfo * PredSI = getSlotInfo(VM->getSlot(i));

    // From the view of signals with undefined timing, all alias slot is the
    // same slot.
    Changed |= addLiveIns(PredSI, To, true);
  }

  return Changed;
}

void RtlSSAAnalysis::ComputeReachingDefinition() {
  ComputeGenAndKill();
  // TODO: Simplify the data-flow, some slot may neither define new VAS nor
  // kill any VAS.

  bool Changed = false;

  do {
    Changed = false;

    for (slot_vec_it I = SlotVec.begin(), E = SlotVec.end(); I != E; ++I) {
      VASTSlot *S =*I;
      assert(S && "Unexpected null slot!");

      SlotInfo *CurSI = getSlotInfo(S);

      // Compute the out set.
      typedef VASTSlot::pred_it pred_it;
      for (pred_it PI = S->pred_begin(), PE = S->pred_end(); PI != PE; ++PI) {
        VASTSlot *PredSlot = *PI;

        // No need to update the out set of Slot 0 according its incoming value.
        // It is the first slot of the FSM.
        if (S->SlotNum == 0 && PredSlot->SlotNum != 0) continue;

        SlotInfo *PredSI = getSlotInfo(PredSlot);

        Changed |= addLiveIns(PredSI, CurSI, false);

        if (PredSlot->getParentBB() == S->getParentBB() &&
            PredSlot->hasAliasSlot())
          Changed |= addLiveInFromAliasSlots(PredSlot, CurSI);
      }
    }
  } while (Changed);
}

void RtlSSAAnalysis::ComputeGenAndKill() {
  // Collect the generated statements to the SlotGenMap.
  for (vas_iterator I = vas_begin(), E = vas_end(); I != E; ++I) {
    ValueAtSlot *VAS = *I;
    VASTSlot *S = VAS->getSlot();
    SlotInfo *SI = getSlotInfo(S);
    SI->insertGen(VAS);

    // Values are overwritten by the alias slots of its defining slot.
    if (!S->hasAliasSlot()) continue;

    unsigned CurSlotNum = S->SlotNum;
    VASTSeqValue *V = VAS->getValue();
    bool IsLoopingBackPHIMove = false;
    if (const MachineInstr *MI = VAS->getDefMI())
      IsLoopingBackPHIMove = MI->getOpcode() == VTM::VOpMvPhi
                             && MI->getOperand(2).getMBB() == MI->getParent();

    for (unsigned i = S->alias_start(), e = S->alias_end(), ii = S->alias_ii();
         i < e; i += ii) {
       if (i == CurSlotNum) continue;

       SlotInfo *AliasSlot = getSlotInfo(VM->getSlot(i));

       // From the view of signals with undefined timing, all alias slot is the
       // same slot, otherwise, the signal is only overwritten by its following
       // alias slot.
       if (i > CurSlotNum || V->isTimingUndef()) AliasSlot->insertOvewritten(V);

       if (i == CurSlotNum - ii && IsLoopingBackPHIMove) {
         // The definition of PHIMove can reach its previous alias slot with
         // distance II.
         AliasSlot->insertIn(VAS, ValueAtSlot::LiveInInfo(ii));
         // The definition is actually for the previous stage.
         AliasSlot->insertGen(VAS);
         // The definition of looping-back PHIMove is not for the current stage.
         SI->SlotGen.erase(VAS);
       }
    }
  }

  // Build the Out set from Gen set.
  for (slot_vec_it I = SlotVec.begin(), E = SlotVec.end(); I != E; ++I) {
    VASTSlot *S =*I;
    assert(S && "Unexpected null slot!");
    SlotInfo *SI = getSlotInfo(S);
    SI->initOutSet();
  }
}

char RtlSSAAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(RtlSSAAnalysis, "RtlSSAAnalysis",
                      "RtlSSAAnalysis", false, false)
  INITIALIZE_PASS_DEPENDENCY(VerilogModuleAnalysis);
INITIALIZE_PASS_END(RtlSSAAnalysis, "RtlSSAAnalysis",
                    "RtlSSAAnalysis", false, false)

Pass *llvm::createRtlSSAAnalysisPass() {
  return new RtlSSAAnalysis();
}
