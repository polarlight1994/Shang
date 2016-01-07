//===----- SIRTimingScriptGen.cpp - Generate Timing Script -----*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the SIRTimingScriptGen class which generates timing
// scripts to direct the logic synthesis tool.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRPass.h"
#include "sir/SIRTimingAnalysis.h"
#include "sir/SIRSTGDistance.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/SetOperations.h"

#include <set>

using namespace llvm;
using namespace vast;

namespace {
struct ConeWithTimingInfo {
  Value *V;
  SIR *SM;
  SIRSTGDistance *STGDist;

  typedef std::map<SIRRegister *, unsigned> TimingIntervalMapTy;
  TimingIntervalMapTy TimingInterval;

  typedef std::set<SIRRegister *> LeafRegSetTy;
  typedef std::map<Instruction *, LeafRegSetTy> Reg2ValPathMapTy;
  typedef std::map<Instruction *, TimingIntervalMapTy> Reg2ValPathIntervalMapTy;
  Reg2ValPathIntervalMapTy Reg2ValPathInterval;

  typedef std::map<SIRRegister *, Instruction *> ViaValueMapTy;
  ViaValueMapTy ViaValueMap;

  ConeWithTimingInfo(Value *V, SIR *SM, SIRSTGDistance *STGDist)
    : V(V), SM(SM), STGDist(STGDist) {}

  void extractLeafReg(Instruction *Inst, Reg2ValPathMapTy &Reg2ValPath);
  void extractTimingPaths(Instruction *Root, SIRRegister *DstReg,
                          ArrayRef<SIRSlot *> ReadSlots);

  void addIntervalFromSrc(SIRRegister *SrcReg, unsigned Interval);

  void generateConstraintEntries(raw_ostream &OS);
};

struct SIRTimingScriptGen : public SIRPass {
  SIR *SM;
  SIRSTGDistance *STGDist;
  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<SIRSTGDistance>();
    AU.setPreservesAll();
  }

  SIRTimingScriptGen() : SIRPass(ID) {
    initializeSIRTimingScriptGenPass(*PassRegistry::getPassRegistry());
  }

  void extractTimingPaths(ConeWithTimingInfo &Cone, SIRRegister *Reg,
                          Value *V, ArrayRef<SIRSlot *> ReadSlots);
  void generateConstraints(SIRRegister *Reg, raw_ostream &OS);

  bool runOnSIR(SIR &SM);
};
}

void ConeWithTimingInfo::extractLeafReg(Instruction *Inst, Reg2ValPathMapTy &Reg2ValPath) {
  LeafRegSetTy &Set = Reg2ValPath[Inst];

  typedef Instruction::op_iterator iterator;
  for (iterator I = Inst->op_begin(), E = Inst->op_end(); I != E; ++I) {
    Instruction *SubInst = dyn_cast<Instruction>(*I);
    if (!SubInst) continue;

    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(SubInst))
      if (II->getIntrinsicID() == Intrinsic::shang_reg_assign)
        continue;

    Reg2ValPathMapTy::iterator at = Reg2ValPath.find(SubInst);

    if(at != Reg2ValPath.end())
      set_union(Set, at->second);
  }
}

void ConeWithTimingInfo::extractTimingPaths(Instruction *Root, SIRRegister *DstReg,
                                            ArrayRef<SIRSlot *> ReadSlots) {
  assert(Root && "Unexpected Instruction!");

  Reg2ValPathMapTy Reg2ValPath;

  typedef Instruction::op_iterator ChildIt;
  std::vector<std::pair<Instruction *, ChildIt> > VisitStack;
  VisitStack.push_back(std::make_pair(Root, Root->op_begin()));

  while (!VisitStack.empty()) {
    Instruction *Inst = VisitStack.back().first;
    ChildIt &It = VisitStack.back().second;

    // Ignore the useless operand end iterator.
    ChildIt EndIt;
    if (isa<IntToPtrInst>(Inst) || isa<PtrToIntInst>(Inst) || isa<BitCastInst>(Inst))
      EndIt = Inst->op_end() - 1;
    else if (isa<CallInst>(Inst))
      EndIt = Inst->op_end() - 2;
    else
      EndIt = Inst->op_end();

    if (It == EndIt) {
      if (!isa<Instruction>(*It)) {
        VisitStack.pop_back();

        extractLeafReg(Inst, Reg2ValPath);

        continue;
      }

      if (Instruction *ChildInst = dyn_cast<Instruction>(*It)) {
        if (Reg2ValPath.count(ChildInst)) {
          VisitStack.pop_back();

          extractLeafReg(Inst, Reg2ValPath);

          continue;
        }
      }

      if (IntrinsicInst *ChildInst = dyn_cast<IntrinsicInst>(*It)) {
        if (ChildInst->getIntrinsicID() == Intrinsic::shang_reg_assign) {
          VisitStack.pop_back();

          SIRRegister *Reg = SM->lookupSIRReg(ChildInst);
          bool Success = Reg2ValPath[Inst].insert(Reg).second;
          assert(Success && "Already existed?");

          ViaValueMap.insert(std::make_pair(Reg, Inst));

          extractLeafReg(Inst, Reg2ValPath);

          continue;
        }
      }
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *It;
    Instruction *ChildInst = dyn_cast<Instruction>(ChildVal);

    if (It != EndIt)
      ++It;

    if (!ChildInst) continue;

    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(ChildInst)) {
      if (II->getIntrinsicID() == Intrinsic::shang_reg_assign) {
        SIRRegister *Reg = SM->lookupSIRReg(ChildInst);

        Reg2ValPath[Inst].insert(Reg);
        continue;
      }
    }

    // Do not visit a node twice.
    if (Reg2ValPath.count(ChildInst)) continue;

    VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
  }

  Reg2ValPathMapTy::iterator I = Reg2ValPath.find(Root);
  //assert(I != Reg2ValPath.end() && "Path not existed?");

  if (I == Reg2ValPath.end())
    return;

  const LeafRegSetTy &Leaves = I->second;
  typedef LeafRegSetTy::iterator iterator;
  for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
    SIRRegister *Leaf = *I;

    // If already exist, ignore it.
    if (TimingInterval.count(Leaf))
      continue;

    unsigned Distance = STGDist->getIntervalFromSrc(Leaf, DstReg, ReadSlots);
    if (Distance < UINT16_MAX) {
      addIntervalFromSrc(Leaf, Distance);
    }
  }
}

void ConeWithTimingInfo::addIntervalFromSrc(SIRRegister *SrcReg, unsigned Interval) {
  assert(Interval && "Unexpected Interval!");

  unsigned &OldInterval = TimingInterval[SrcReg];
  // DIRTY HACK: Wrap the OldCycles, so that we can insert the new Cycles
  // with the min function even OldCycles is 0.
  OldInterval = std::min(OldInterval - 1, Interval - 1) + 1;
}

void ConeWithTimingInfo::generateConstraintEntries(raw_ostream &OS) {
  SIRRegister *DstReg = SM->lookupSIRReg(V);
  assert(DstReg && "Unexpected NULL Register!");

  SM->indexKeepReg(DstReg);

  typedef TimingIntervalMapTy::iterator iterator;
  for (iterator I = TimingInterval.begin(), E = TimingInterval.end(); I != E; ++I) {
    if (I->second == 1)
      continue;

    SM->indexKeepReg(I->first);

    OS << "set src [get_keepers " << Mangle(I->first->getName()) << "*]\n";
    OS << "set dst [get_keepers " << Mangle(DstReg->getName()) << "*]\n";
    OS << "if {[get_collection_size $src] && [get_collection_size $dst]} {\n";

    Instruction *ViaValue = ViaValueMap[I->first];
//     if  (ViaValue) {
//       OS << "  set thu [get_nets " << Mangle(ViaValue->getName()) << "*]\n";
//       OS << "  if {[get_collection_size $thu]} {\n";
//       OS << "    set_multicycle_path -from $src -through $thu -to $dst -setup -end " << I->second << "\n";
//       OS << "  }\n";
//     } else {
      OS << "  set_multicycle_path -from $src -to $dst -setup -end " << I->second << "\n";
/*    }*/

    OS << "}\n\n";
  }
}

void SIRTimingScriptGen::extractTimingPaths(ConeWithTimingInfo &Cone, SIRRegister *Reg,
                                            Value *V, ArrayRef<SIRSlot *> ReadSlots) {
  // Trivial case: register to register path.
  if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(V)) {
    if (II->getIntrinsicID() == Intrinsic::shang_reg_assign) {
      SIRRegister *SrcReg = SM->lookupSIRReg(V);
      unsigned Interval = STGDist->getIntervalFromSrc(SrcReg, Reg, ReadSlots);
      Cone.addIntervalFromSrc(SrcReg, Interval);

      return;
    }
  }

  Instruction *Inst = dyn_cast<Instruction>(V);

  // If the value is not a instruction, just skip it.
  if (!Inst) return;

  // Or build the combination path cone and get the intervals.
  Cone.extractTimingPaths(Inst, Reg, ReadSlots);
}

void SIRTimingScriptGen::generateConstraints(SIRRegister *Reg, raw_ostream &OS) {
  ConeWithTimingInfo Cone(Reg->getLLVMValue(), SM, STGDist);

  SmallVector<SIRSlot *, 8> ReadSlots;
  typedef SIRRegister::faninslots_iterator iterator;
  for (iterator I = Reg->faninslots_begin(), E = Reg->faninslots_end(); I != E; ++I) {
    unsigned Num = (*I)->getSlotNum();
    ReadSlots.push_back(*I);
  }

  extractTimingPaths(Cone, Reg, Reg->getRegVal(), ReadSlots);
  extractTimingPaths(Cone, Reg, Reg->getRegGuard(), ReadSlots);

  Cone.generateConstraintEntries(OS);
}

bool SIRTimingScriptGen::runOnSIR(SIR &SM) {
  SIRSTGDistance &STGDist = getAnalysis<SIRSTGDistance>();

  this->SM = &SM;
  this->STGDist = &STGDist;

  std::string TimingScript = LuaI::GetString("SDCFile");
  std::string Error;
  raw_fd_ostream Output(TimingScript.c_str(), Error);

  // Print the clk settings.
  Output << "create_clock -name \"clk\" -period 10ns [get_ports {clk}]\n";
  Output << "derive_pll_clocks -create_base_clocks\n";
  Output << "set_multicycle_path -from [get_clocks {clk}] -to [get_clocks {clk}] -hold -end 0";
  Output << "\n\n";

  typedef SIR::register_iterator reg_iterator;
  for (reg_iterator I = SM.registers_begin(), E = SM.registers_end(); I != E; ++I) {
    SIRRegister *Reg = I;

    if (Reg->fanin_empty() || (Reg->getLLVMValue()->use_empty() && !Reg->isFUInput()))
      continue;

    generateConstraints(Reg, Output);
  }

  return false;
}

char SIRTimingScriptGen::ID = 0;
char &llvm::SIRTimingScriptGenID = SIRTimingScriptGen::ID;

INITIALIZE_PASS_BEGIN(SIRTimingScriptGen, "sir-timing-script-generate",
                      "Generate timing script to direct the logic synthesis tool",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(SIRSTGDistance)
INITIALIZE_PASS_END(SIRTimingScriptGen, "sir-timing-script-generate",
                    "Generate timing script to direct the logic synthesis tool",
                    false, true)