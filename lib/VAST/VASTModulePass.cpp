//===-- VASTModulePass.cpp - Build the VASTModule on LLVM IR --------------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implement the shang/VASTModulePass pass, which is the container of the
// VASTModule.
//
//===----------------------------------------------------------------------===//
#include "IR2Datapath.h"
#include "MinimalDatapathContext.h"
#include "Allocation.h"

#include "shang/VASTMemoryPort.h"
#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/VASTSubModules.h"

#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-vast-module-analysis"
#include "llvm/Support/Debug.h"

#include <map>

using namespace llvm;
STATISTIC(NumIPs, "Number of IPs Instantiated");
STATISTIC(NumBRam2Reg, "Number of Single Element Block RAM lowered to Register");

namespace {
struct VASTModuleBuilder : public MinimalDatapathContext,
                           public InstVisitor<VASTModuleBuilder, void> {
  DatapathBuilder Builder;
  VASTModule *VM;
  DataLayout *TD;
  HLSAllocation &Allocation;

  //===--------------------------------------------------------------------===//
  void emitFunctionSignature(Function *F, VASTSubModule *SubMod = 0);
  void emitCommonPort(VASTSubModule *SubMod);

  //===--------------------------------------------------------------------===//
  StringMap<VASTSubModule*> SubModules;
  VASTSubModule *getSubModule(StringRef Name) const {
    VASTSubModule *SubMod = SubModules.lookup(Name);
    assert(SubMod && "Submodule not allocated!");
    return SubMod;
  }

  VASTSubModule *emitIPFromTemplate(const char *Name, unsigned ResultSize);
  //===--------------------------------------------------------------------===//
  // TODO: Support putting multiple GVs in a single BRAM.
  std::map<unsigned, VASTNode*> AllocatedBRAMs;

  VASTNode *emitBlockRAM(unsigned BRAMNum, const GlobalVariable *GV);
  VASTNode *getBlockRAM(unsigned BRAMNum) const {
    std::map<unsigned, VASTNode*>::const_iterator at
      = AllocatedBRAMs.find(BRAMNum);
    assert(at != AllocatedBRAMs.end() && "BlockRAM not existed!");
    return at->second;
  }

  std::map<unsigned, VASTMemoryBus*> MemBuses;
  VASTMemoryBus *getOrCreateMemBus(unsigned Num) {
    VASTMemoryBus *&Bus = MemBuses[Num];
    if (Bus == 0) {
      VFUMemBus *Desc = getFUDesc<VFUMemBus>();
      Bus = VM->createMemBus(Num, Desc->getAddrWidth(), Desc->getDataWidth());
    }

    return Bus;
  }

  VASTMemoryBus *getMemBus(unsigned Num) const {
    std::map<unsigned, VASTMemoryBus*>::const_iterator at = MemBuses.find(Num);
    assert(at != MemBuses.end() && "BlockRAM not existed!");
    return at->second;
  }

  //===--------------------------------------------------------------------===//
  void allocateSubModules();
  //===--------------------------------------------------------------------===//
  VASTSeqValue *getOrCreateSeqVal(Value *V, const Twine &Name);

  VASTValPtr getAsOperandImpl(Value *Op, bool GetAsInlineOperand = true);
  //===--------------------------------------------------------------------===//
  void connectEntryState(BasicBlock *EntryBB);

  // Remember the landing slot and the latest slot of a basic block.
  std::map<BasicBlock*, std::pair<VASTSlot*, VASTSlot*> > BB2SlotMap;
  unsigned NumSlots;
  VASTSlot *getOrCreateLandingSlot(BasicBlock *BB) {
    std::pair<VASTSlot*, VASTSlot*> &Slots = BB2SlotMap[BB];

    if (Slots.first == 0) {
      assert(Slots.second == 0 && "Unexpected Latest slot without landing slot!");
      Slots.first
        = (Slots.second = VM->createSlot(++NumSlots, BB));
    }

    return Slots.first;
  }

  VASTSlot *getLatestSlot(BasicBlock *BB) const {
    std::map<BasicBlock*, std::pair<VASTSlot*, VASTSlot*> >::const_iterator at
      = BB2SlotMap.find(BB);
    assert(at != BB2SlotMap.end() && "Latest slot not found!");
    return at->second.second;
  }

  // DIRTYHACK: Allocate enough slots for the read operation.
  VASTSlot *advanceToNextSlot(VASTSlot *CurSlot) {
    BasicBlock *BB = CurSlot->getParent();
    VASTSlot *&Slot = BB2SlotMap[BB].second;
    assert(Slot == CurSlot && "CurSlot not the last slot in the BB!");
    assert(CurSlot->succ_empty() && "CurSlot already have successors!");
    Slot = VM->createSlot(++NumSlots, BB);
    // Connect the slots.
    addSuccSlot(CurSlot, Slot);
    return Slot;
  }

  VASTSlot *advanceToNextSlot(VASTSlot *CurSlot, unsigned NumSlots) {
    VASTSlot *S = CurSlot;
    for (unsigned i = 0; i < NumSlots; ++i)
      S = advanceToNextSlot(S);

    return S;
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot,
                   VASTValPtr Cnd = VASTImmediate::True,
                   TerminatorInst *Inst = 0) {
    // If the Br is already exist, simply or the conditions together.
   assert(!S->hasNextSlot(NextSlot) && "Edge had already existed!");
   assert((S->getParent() == NextSlot->getParent()
           || NextSlot == VM->getFinishSlot())
          && "Cannot change Slot and BB at the same time!");

    S->addSuccSlot(NextSlot);
    VASTSlotCtrl *SlotBr = VM->createSlotCtrl(NextSlot, S, Cnd);
    if (Inst) SlotBr->annotateValue(Inst);
  }

  //===--------------------------------------------------------------------===//
  void visitBasicBlock(BasicBlock *BB);

  // Build the SeqOps from the LLVM Instruction.
  void visitReturnInst(ReturnInst &I);
  void visitBranchInst(BranchInst &I);
  void visitSwitchInst(SwitchInst &I);
  void visitUnreachableInst(UnreachableInst &I);

  void visitCallSite(CallSite CS);
  void visitIntrinsicInst(IntrinsicInst &I);

  void visitBinaryOperator(BinaryOperator &I);

  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);

  void visitInstruction(Instruction &I) {
    I.dump();
  }
  //===--------------------------------------------------------------------===//
  void buildMemoryTransaction(Value *Addr, Value *Data, unsigned PortNum,
                              Instruction &Inst);
  unsigned getByteEnable(Value *Addr) const;

  void buildBRAMTransaction(Value *Addr, Value *Data, unsigned BRAMNum,
                            Instruction &Inst);

  void buildSubModuleOperation(VASTSeqInst *Inst, VASTSubModule *SubMod,
                               ArrayRef<VASTValPtr> Args);
  //===--------------------------------------------------------------------===//
  VASTModuleBuilder(VASTModule *Module, DataLayout *TD, HLSAllocation &Allocation)
    : MinimalDatapathContext(*Module, TD), Builder(*this),
      VM(Module), TD(TD), Allocation(Allocation), NumSlots(0)  {}
};
}

void VASTModuleBuilder::connectEntryState(BasicBlock *EntryBB) {
  VASTSlot *IdleSlot = VM->getStartSlot();

  // Create the virtual slot representing the idle loop.
  VASTValue *StartPort = VM->getPort(VASTModule::Start).getValue();
  VASTSlot *IdleLoopBack = VM->createSlot(++NumSlots, 0,
                                          Builder.buildNotExpr(StartPort),
                                          true);
  IdleSlot->addSuccSlot(IdleLoopBack);
  IdleLoopBack->addSuccSlot(IdleSlot);

  // Create the virtual slot represent the launch of the design.
  VASTSlot *LaunchSlot = VM->createSlot(++NumSlots, EntryBB, StartPort, true);
  IdleSlot->addSuccSlot(LaunchSlot);

  // Connect the launch slot to the landing slot, with a real edge (which
  // represent a state transition)
  addSuccSlot(LaunchSlot, getOrCreateLandingSlot(EntryBB), StartPort);
}

//===----------------------------------------------------------------------===//
VASTSeqValue *VASTModuleBuilder::getOrCreateSeqVal(Value *V, const Twine &Name) {
  assert(!V->getType()->isVoidTy() && "Cannot create SeqVal for Inst!");
  VASTValPtr Val = Builder.lookupExpr(V);

  if (Val) {
    assert(!Val.isInverted() && isa<VASTSeqValue>(Val.get()) && "Bad value type!");
    return cast<VASTSeqValue>(Val.get());
  }

  // Create the SeqVal now.
  unsigned BitWidth = Builder.getValueSizeInBits(V);
  std::string SeqValName = "v_" + Name.str() + "_r";
  SeqValName = ShangMangle(SeqValName);
  VASTRegister *R
    =  VM->addRegister(SeqValName, BitWidth, 0, VASTSeqValue::Data, 0);
  // V = VM->createSeqValue("v" + utostr_32(RegNo) + "r", BitWidth,
  //                        VASTNode::Data, RegNo, 0);

  // Index the value.
  Builder.indexVASTExpr(V, R->getValue());
  return R->getValue();
}


VASTValPtr VASTModuleBuilder::getAsOperandImpl(Value *V, bool GetAsInlineOperand)
{
  if (VASTValPtr Val = lookupExpr(V)) {
    // Try to inline the operand if user ask to.
    if (GetAsInlineOperand) Val = Val.getAsInlineOperand();
    return Val;
  }

  // The VASTValPtr of the instruction should had been created when we trying
  // to get it as operand.
  assert(!isa<Instruction>(V) && "The VASTValPtr for Instruction not found!");

  if (ConstantInt *Int = dyn_cast<ConstantInt>(V))
    return indexVASTExpr(V, getOrCreateImmediate(Int->getValue()));

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
    FuncUnitId ID = Allocation.getMemoryPort(*GV);
    unsigned SizeInBits = getValueSizeInBits(GV);

    if (ID.getFUType() == VFUs::BRam)
      // FIXIME: Calculate the offset of the GV in the block RAM.
      return indexVASTExpr(GV, getOrCreateImmediate(0, SizeInBits));

    assert(ID.getFUType() == VFUs::MemoryBus && "Bad FU type!");
    if (unsigned PortNum = ID.getFUNum()) {
      VASTMemoryBus *Bus = getMemBus(PortNum);
      unsigned StartOffset = Bus->getStartOffset(GV);
      VASTImmediate *Imm = getOrCreateImmediate(StartOffset, SizeInBits);
      VASTWire *W = VM->createWrapperWire(ShangMangle(GV->getName()), SizeInBits);
      W->assign(Imm);
      return indexVASTExpr(GV, W);
    }
    
    // If the GV is assigned to the memory port 0, create a wrapper wire for it.
    return indexVASTExpr(GV, VM->createWrapperWire(GV, SizeInBits));
  }

  if (GEPOperator *GEP = dyn_cast<GEPOperator>(V))
    return indexVASTExpr(V, Builder.visitGEPOperator(*GEP));

  // Try to build the datapath for the constant expression.
  if (ConstantExpr *CExpr = dyn_cast<ConstantExpr>(V)) {
    switch (CExpr->getOpcode()) {
    default:break;
    case Instruction::BitCast: {
      VASTValPtr Operand = getAsOperandImpl(CExpr->getOperand(0));
      assert(getValueSizeInBits(V) == Operand->getBitWidth()
             && "Cast between types with different size found!");
      return Operand;
    }
    }
  }

  if (UndefValue *UDef = dyn_cast<UndefValue>(V)) {
    unsigned SizeInBit = getValueSizeInBits(UDef);
    // TEMPORARY HACK: Create random value for UndefValue.
    // TODO: Create 'x' for UndefValue.
    return VM->createUDef(SizeInBit);
  }

  if (ConstantPointerNull *PtrNull = dyn_cast<ConstantPointerNull>(V)) {
    unsigned SizeInBit = getValueSizeInBits(PtrNull);
    return getOrCreateImmediate(APInt::getNullValue(SizeInBit));
  }

  llvm_unreachable("Unhandle value!");
}

//===----------------------------------------------------------------------===//
void VASTModuleBuilder::emitFunctionSignature(Function *F,
                                              VASTSubModule *SubMod) {
  VASTSlot *StartSlot = VM->getStartSlot();
  SmallVector<VASTSeqValue*, 4> ArgRegs;
  SmallVector<VASTValPtr, 4> ArgPorts;
  SmallVector<Value*, 4> Args;

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E; ++I)
  {
    Argument *Arg = I;
    std::string Name = Arg->getName();
    unsigned BitWidth = TD->getTypeSizeInBits(Arg->getType());
    // Add port declaration.
    if (SubMod) {
      std::string RegName = SubMod->getPortName(Name);
      VASTRegister *R = VM->addIORegister(RegName, BitWidth, SubMod->getNum());
      SubMod->addInPort(Name, R->getValue());
      continue;
    }

    VASTValPtr V
      = VM->addInputPort(Name, BitWidth, VASTModule::ArgPort)->getValue();
    // Remember the expression for the argument input.
    VASTSeqValue *SeqVal = getOrCreateSeqVal(Arg, Name);
    ArgRegs.push_back(SeqVal);
    ArgPorts.push_back(V);
    Args.push_back(Arg);
  }

  Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD->getTypeSizeInBits(RetTy);
    if (SubMod)
      SubMod->createRetPort(VM, BitWidth);
    else
      VM->addOutputPort("return_value", BitWidth, VASTModule::RetPort);
  }

  emitCommonPort(SubMod);
  if (SubMod) return;

  // Create the default memory bus.
  if (F->getName() != "main") {
    bool Inserted
      = MemBuses.insert(std::make_pair(0, VM->createDefaultMemBus())).second;
    assert(Inserted && "Memory bus not inserted!");
    (void) Inserted;
  }

  // Copy the value to the register.
  VASTValue *StartPort = VM->getPort(VASTModule::Start).getValue();
  for (unsigned i = 0, e = ArgRegs.size(); i != e; ++i)
    VM->latchValue(ArgRegs[i], ArgPorts[i], StartSlot, StartPort, Args[i]);
}

void VASTModuleBuilder::emitCommonPort(VASTSubModule *SubMod) {
  if (SubMod) {
    // It is a callee function, emit the signal for the sub module.
    SubMod->createStartPort(VM);
    SubMod->createFinPort(VM);
  } else { // If F is current function.
    VM->addInputPort("clk", 1, VASTModule::Clk);
    VM->addInputPort("rstN", 1, VASTModule::RST);
    VM->addInputPort("start", 1, VASTModule::Start);
    VM->addOutputPort("fin", 1, VASTModule::Finish);
  }
}

void VASTModuleBuilder::allocateSubModules() {
  Function &F = *VM;

  // Allocate the block RAMs.
  ArrayRef<const GlobalVariable*> GVs = Allocation.getBRAMAllocation(&F);
  for (unsigned i = 0; i < GVs.size(); ++i) {
    const GlobalVariable *GV = GVs[i];
    FuncUnitId ID = Allocation.getMemoryPort(*GV);
    assert(ID.getFUType() == VFUs::BRam && "Bad block RAM allocation!");
    emitBlockRAM(ID.getFUNum(), GV);
  }

  typedef Module::global_iterator global_iterator;
  Module *M = F.getParent();
  for (global_iterator I = M->global_begin(), E = M->global_end(); I != E; ++I) {
    GlobalVariable *GV = I;

    FuncUnitId Id = Allocation.getMemoryPort(*GV);
    // Ignore the block rams that is already assign to block RAM.
    if (Id.getFUType() != VFUs::MemoryBus || Id.getFUNum() == 0)
      continue;

    VASTMemoryBus *Bus = getOrCreateMemBus(Id.getFUNum());

    Type *ElemTy = GV->getType()->getElementType();
    unsigned NumElem = 1;

    // Try to expand multi-dimension array to single dimension array.
    while (const ArrayType *AT = dyn_cast<ArrayType>(ElemTy)) {
      ElemTy = AT->getElementType();
      NumElem *= AT->getNumElements();
    }

    unsigned ElementSizeInBytes = TD->getTypeStoreSize(ElemTy);

    Bus->addGlobalVariable(GV, NumElem * ElementSizeInBytes);
  }
}

VASTNode *VASTModuleBuilder::emitBlockRAM(unsigned BRAMNum,
                                          const GlobalVariable *GV) {
  // Count the number of elements and the size of a single element.
  // Assume the Allocated type is a scalar.
  Type *ElemTy = GV->getType()->getElementType();
  unsigned NumElem = 1;

  // Try to expand multi-dimension array to single dimension array.
  while (const ArrayType *AT = dyn_cast<ArrayType>(ElemTy)) {
    ElemTy = AT->getElementType();
    NumElem *= AT->getNumElements();
  }

  unsigned ElementSizeInBits = TD->getTypeStoreSizeInBits(ElemTy);

  // If there is only 1 element, simply replace the block RAM by a register.
  if (NumElem == 1) {
    uint64_t InitVal = 0;
    // Try to retrieve the initialize value of the register, it may be a
    // ConstantInt or ConstPointerNull, we can safely ignore the later case
    // since the InitVal is default to 0.
    const ConstantInt *CI
      = dyn_cast_or_null<ConstantInt>(GV->getInitializer());
    assert((CI || isa<ConstantPointerNull>(GV->getInitializer()))
            && "Unexpected initialier!");
    assert((CI == 0 || CI->getBitWidth() <= 64) && "Initializer not supported!");
    if (CI) InitVal = CI->getZExtValue();

    VASTRegister *R = VM->addRegister(VFUBRAM::getOutDataBusName(BRAMNum),
                                      ElementSizeInBits, InitVal,
                                      VASTSeqValue::StaticRegister, BRAMNum);
    bool Inserted = AllocatedBRAMs.insert(std::make_pair(BRAMNum, R)).second;
    assert(Inserted && "Creating the same BRAM twice?");
    (void) Inserted;
    ++NumBRam2Reg;
    return 0;
  }

  // Create the block RAM object.
  VASTBlockRAM *BRAM = VM->addBlockRAM(BRAMNum, ElementSizeInBits, NumElem, GV);
  bool Inserted = AllocatedBRAMs.insert(std::make_pair(BRAMNum, BRAM)).second;
  assert(Inserted && "Creating the same BRAM twice?");
  (void) Inserted;

  return BRAM;
}

VASTSubModule *
VASTModuleBuilder::emitIPFromTemplate(const char *Name, unsigned ResultSize)
{
  if (VASTSubModule *SubMod = SubModules.lookup(Name))
    return SubMod;

  SmallVector<VFUs::ModOpInfo, 4> OpInfo;
  unsigned Latency = VFUs::getModuleOperands(Name, SubModules.size(), OpInfo);

  // Submodule information not available, create the sequential code.
  if (OpInfo.empty()) {
    //N = VM->addSeqCode(Name);
    return 0;
  }

  // Create and insert the submodule.
  unsigned FNNum = SubModules.size();
  VASTSubModule *SubMod = VM->addSubmodule(Name, FNNum);
  SubMod->setIsSimple(false);
  SubModules.GetOrCreateValue(Name, SubMod);

  SmallVector<VASTValPtr, 4> Ops;
  // Add the fanin registers.
  for (unsigned i = 0, e = OpInfo.size(); i < e; ++i) {
    VASTRegister *R = VM->addIORegister(OpInfo[i].first, OpInfo[i].second, FNNum);
    SubMod->addInPort(OpInfo[i].first, R->getValue());
    Ops.push_back(R->getValue());
  }
  // Add the start register.
  SubMod->createStartPort(VM);
  // Create the finish signal from the submodule.
  SubMod->createFinPort(VM);

  // Dose the submodule have a return port?
  if (ResultSize) SubMod->createRetPort(VM, ResultSize, Latency);

  ++NumIPs;
  return SubMod;
}

//===----------------------------------------------------------------------===//
void VASTModuleBuilder::visitBasicBlock(BasicBlock *BB) {
  // Create the landing slot for this BB.
  (void) getOrCreateLandingSlot(BB);

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    // PHINodes will be handled in somewhere else.
    if (isa<PHINode>(I)) continue;

    // Try to build the datapath expressions.
    if (VASTValPtr V = Builder.visit(I)) {
      Builder.indexVASTExpr(I, V);
      continue;
    }

    // Otherwise build the SeqOp for this operation.
    visit(I);
  }

  // Emit the operation that copy the incoming value of PHIs at the last slot.
  // TODO: Optimize the PHIs in the datapath hoist pass.
  VASTSlot *LatestSlot = getLatestSlot(BB);
  // Please note that the successor blocks enumed by succ_iterator is not unique.
  std::set<BasicBlock*> UniqueSuccs(succ_begin(BB), succ_end(BB));
  typedef std::set<BasicBlock*>::iterator succ_it;
  for (succ_it SI = UniqueSuccs.begin(), SE = UniqueSuccs.end(); SI != SE; ++SI) {
    BasicBlock *SuccBB = *SI;
    VASTSlot *SuccSlot = getOrCreateLandingSlot(SuccBB);

    for (iterator I = SuccBB->begin(), E = SuccBB->getFirstNonPHI(); I != E; ++I) {
      PHINode *PN = cast<PHINode>(I);

      Value *LiveOutedFromBB = PN->DoPHITranslation(SuccBB, BB);
      VASTValPtr LiveOut = getAsOperandImpl(LiveOutedFromBB);
      VASTValPtr Pred;// = LatestSlot->getSuccCnd(SuccSlot);
      llvm_unreachable("Not implemented!");
      VASTSeqValue *PHISeqVal = getOrCreateSeqVal(PN, PN->getName());
      // Latch the incoming value when we are branching to the succ slot.
      VM->latchValue(PHISeqVal, LiveOut, LatestSlot,  Pred, PN);
    }
  }
}

void VASTModuleBuilder::visitReturnInst(ReturnInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  unsigned NumOperands = I.getNumOperands();
  VASTSeqInst *SeqInst =
    VM->lauchInst(CurSlot, VASTImmediate::True, NumOperands + 1, &I,
                  VASTSeqInst::Latch);

  // Assign the return port if necessary.
  if (NumOperands) {
    VASTSeqValue *RetPort = VM->getRetPort().getSeqVal();
    // Please note that we do not need to export the definition of the value
    // on the return port.
    SeqInst->addSrc(getAsOperandImpl(I.getReturnValue()), 0, false, RetPort);
  }

  // Enable the finish port.
  SeqInst->addSrc(VASTImmediate::True, NumOperands, false,
                  VM->getPort(VASTModule::Finish).getSeqVal());

  // Construct the control flow.
  addSuccSlot(CurSlot, VM->getFinishSlot(), VASTImmediate::True, &I);
}

void VASTModuleBuilder::visitUnreachableInst(UnreachableInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  // DIRTYHACK: Simply jump back the start slot.
  // Construct the control flow.
  addSuccSlot(CurSlot, VM->getFinishSlot(), VASTImmediate::True, &I);
}

void VASTModuleBuilder::visitBranchInst(BranchInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  // TODO: Create alias operations.
  if (I.isUnconditional()) {
    BasicBlock *DstBB = I.getSuccessor(0);
    addSuccSlot(CurSlot, getOrCreateLandingSlot(DstBB), VASTImmediate::True, &I);
    return;
  }

  // Connect the slots according to the condition.
  VASTValPtr Cnd = getAsOperandImpl(I.getCondition());
  BasicBlock *TrueBB = I.getSuccessor(0);
  addSuccSlot(CurSlot, getOrCreateLandingSlot(TrueBB), Cnd, &I);
  BasicBlock *FalseBB = I.getSuccessor(1);
  addSuccSlot(CurSlot, getOrCreateLandingSlot(FalseBB),
              Builder.buildNotExpr(Cnd), &I);
}

// Copy from LowerSwitch.cpp.
namespace {
struct CaseRange {
  APInt Low;
  APInt High;
  BasicBlock* BB;

  CaseRange(APInt low = APInt(), APInt high = APInt(), BasicBlock *bb = 0) :
    Low(low), High(high), BB(bb) { }

};

typedef std::vector<CaseRange>           CaseVector;
typedef std::vector<CaseRange>::iterator CaseItr;
}

// Clusterify - Transform simple list of Cases into list of CaseRange's
static unsigned Clusterify(CaseVector& Cases, SwitchInst *SI) {
  IntegersSubsetToBB TheClusterifier;

  // Start with "simple" cases
  for (SwitchInst::CaseIt i = SI->case_begin(), e = SI->case_end();
       i != e; ++i) {
    BasicBlock *SuccBB = i.getCaseSuccessor();
    IntegersSubset CaseRanges = i.getCaseValueEx();
    TheClusterifier.add(CaseRanges, SuccBB);
  }

  TheClusterifier.optimize();

  size_t numCmps = 0;
  for (IntegersSubsetToBB::RangeIterator i = TheClusterifier.begin(),
       e = TheClusterifier.end(); i != e; ++i, ++numCmps) {
    IntegersSubsetToBB::Cluster &C = *i;

    // FIXME: Currently work with ConstantInt based numbers.
    // Changing it to APInt based is a pretty heavy for this commit.
    Cases.push_back(CaseRange(C.first.getLow(), C.first.getHigh(), C.second));
    if (C.first.isSingleNumber())
      // A range counts double, since it requires two compares.
      ++numCmps;
  }

  return numCmps;
}

void VASTModuleBuilder::visitSwitchInst(SwitchInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  VASTValPtr CndVal = getAsOperandImpl(I.getCondition());

  std::map<BasicBlock*, VASTValPtr> CaseMap;

  // Prepare cases vector.
  CaseVector Cases;
  Clusterify(Cases, &I);
  // Build the condition map.
  for (CaseItr CI = Cases.begin(), CE = Cases.end(); CI != CE; ++CI) {
    const CaseRange &Case = *CI;
    // Simple case, test if the CndVal is equal to a specific value.
    if (Case.High == Case.Low) {
      VASTValPtr CaseVal = getOrCreateImmediate(Case.High);
      VASTValPtr Pred = Builder.buildEQ(CndVal, CaseVal);
      VASTValPtr &BBPred = CaseMap[Case.BB];
      if (!BBPred) BBPred = Pred;
      else         Builder.orEqual(BBPred, Pred);

      continue;
    }

    // Test if Low <= CndVal <= High
    VASTValPtr Low = Builder.getImmediate(Case.Low);
    VASTValPtr LowCmp = Builder.buildICmpOrEqExpr(VASTExpr::dpUGT, CndVal, Low);
    VASTValPtr High = Builder.getImmediate(Case.High);
    VASTValPtr HighCmp = Builder.buildICmpOrEqExpr(VASTExpr::dpUGT, High, CndVal);
    VASTValPtr Pred = Builder.buildAndExpr(LowCmp, HighCmp, 1);
    VASTValPtr &BBPred = CaseMap[Case.BB];
    if (!BBPred) BBPred = Pred;
    else         Builder.orEqual(BBPred, Pred);
  }

  // The predicate for each non-default destination.
  SmallVector<VASTValPtr, 4> CasePreds;
  typedef std::map<BasicBlock*, VASTValPtr>::iterator CaseIt;
  for (CaseIt CI = CaseMap.begin(), CE = CaseMap.end(); CI != CE; ++CI) {
    BasicBlock *SuccBB = CI->first;
    VASTSlot *SuccSlot = getOrCreateLandingSlot(SuccBB);
    VASTValPtr Pred = CI->second;
    CasePreds.push_back(Pred);
    addSuccSlot(CurSlot, SuccSlot, Pred, &I);
  }

  // Jump to the default block when all the case value not match, i.e. all case
  // predicate is false.
  VASTValPtr DefaultPred = Builder.buildNotExpr(Builder.buildOrExpr(CasePreds, 1));
  VASTSlot *DefLandingSlot = getOrCreateLandingSlot(I.getDefaultDest());
  addSuccSlot(CurSlot, DefLandingSlot, DefaultPred, &I);
}

void VASTModuleBuilder::visitCallSite(CallSite CS) {
  Function *Callee = CS.getCalledFunction();
  // Ignore the external function.
  if (Callee->isDeclaration()) return;

  assert(!CS.isInvoke() && "Cannot handle invoke at this moment!");
  CallInst *Inst = cast<CallInst>(CS.getInstruction());

  VASTSubModule *SubMod = getSubModule(Callee->getName());
  assert(SubMod && "Submodule not allocated?");
  unsigned NumArgs = CS.arg_size();

  SmallVector<VASTValPtr, 4> Args;
  for (unsigned i = 0; i < NumArgs; ++i)
    Args.push_back(getAsOperandImpl(CS.getArgument(i)));

  BasicBlock *ParentBB = CS->getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);
  VASTSeqInst *Op = VM->lauchInst(Slot, VASTImmediate::True, Args.size() + 1,
                                  Inst, VASTSeqInst::Launch);
  // Build the logic to lauch the module and read the result.
  buildSubModuleOperation(Op, SubMod, Args);
}

void VASTModuleBuilder::visitBinaryOperator(BinaryOperator &I) {
  // The Operator may had already been lowered.
  if (lookupExpr(&I)) return;

  I.dump();

  unsigned SizeInBits = getValueSizeInBits(I);
  VASTSubModule *SubMod = 0;

  switch (I.getOpcode()) {
  default: break;;
  case Instruction::UDiv: {
    static const char *IPNames[] = { "__ip_udiv_i64", "__ip_udiv_i32" };
    SubMod = emitIPFromTemplate(IPNames[6 - Log2_32_Ceil(SizeInBits)], SizeInBits);
    break;
  }
  case Instruction::SDiv: {
    static const char *IPNames[] = { "__ip_sdiv_i64", "__ip_sdiv_i32" };
    SubMod = emitIPFromTemplate(IPNames[6 - Log2_32_Ceil(SizeInBits)], SizeInBits);
    break;
  }
  case Instruction::SRem: {
    static const char *IPNames[] = { "__ip_srem_i64", "__ip_srem_i32" };
    SubMod = emitIPFromTemplate(IPNames[6 - Log2_32_Ceil(SizeInBits)], SizeInBits);
    break;
  }
  }

  if (SubMod == 0) {
    errs() << "Warning: Cannot generate IP to implement instruction:\n";
    I.print(errs());
    return;
  }

  VASTValPtr Ops[] = { getAsOperandImpl(I.getOperand(0)),
                       getAsOperandImpl(I.getOperand(1)) };

  BasicBlock *ParentBB = I.getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);
  VASTSeqInst *Op
    = VM->lauchInst(Slot, VASTImmediate::True, 2 + 1, &I, VASTSeqInst::Launch);
  buildSubModuleOperation(Op, SubMod, Ops);
}

void VASTModuleBuilder::buildSubModuleOperation(VASTSeqInst *Inst,
                                                VASTSubModule *SubMod,
                                                ArrayRef<VASTValPtr> Args) {
  for (unsigned i = 0; i < Args.size(); ++i)
    Inst->addSrc(Args[i], i, false, SubMod->getFanin(i));
  // Assign to the enable port.
  Inst->addSrc(VASTImmediate::True, Args.size(), false, SubMod->getStartPort());

  Value *V = Inst->getValue();
  VASTSlot *Slot = Inst->getSlot();
  // Disable the start port of the submodule at the next slot.
  Slot = advanceToNextSlot(Slot);
  VM->createSlotCtrl(SubMod->getFinPort(), Slot, VASTImmediate::True)
      ->annotateValue(V);

  // Read the return value from the function if there is any.
  if (VASTSeqValue *RetPort = SubMod->getRetPort()) {
    VASTSeqValue *Result
      = getOrCreateSeqVal(Inst->getValue(), Inst->getValue()->getName());
    VM->latchValue(Result, RetPort, Slot, VASTImmediate::True, V, 1);
    // Move the the next slot so that the operation can correctly read the
    // returned value
    advanceToNextSlot(Slot);
  }
}

//===----------------------------------------------------------------------===//

void VASTModuleBuilder::visitIntrinsicInst(IntrinsicInst &I) {
  I.dump();
}

void VASTModuleBuilder::visitLoadInst(LoadInst &I) {
  FuncUnitId FU = Allocation.getMemoryPort(I);
  if (FU.getFUType() == VFUs::MemoryBus) {
    buildMemoryTransaction(I.getPointerOperand(), 0, FU.getFUNum(), I);
    return;
  }

  assert(FU.getFUType() == VFUs::BRam && "Unexpected FU type for memory access!");
  buildBRAMTransaction(I.getPointerOperand(), 0, FU.getFUNum(), I);
}

void VASTModuleBuilder::visitStoreInst(StoreInst &I) {
  FuncUnitId FU = Allocation.getMemoryPort(I);
  if (FU.getFUType() == VFUs::MemoryBus) {
    buildMemoryTransaction(I.getPointerOperand(), I.getValueOperand(),
                           FU.getFUNum(), I);
    return;
  }

  assert(FU.getFUType() == VFUs::BRam && "Unexpected FU type for memory access!");
  buildBRAMTransaction(I.getPointerOperand(), I.getValueOperand(),
                       FU.getFUNum(), I);
}

//===----------------------------------------------------------------------===//
// Memory transaction code building functions.
static unsigned GetByteEnable(unsigned SizeInBytes) {
  return (0x1 << SizeInBytes) - 1;
}

unsigned VASTModuleBuilder::getByteEnable(Value *Addr) const {
  PointerType *AddrTy = cast<PointerType>(Addr->getType());
  Type *DataTy = AddrTy->getElementType();
  return GetByteEnable(TD->getTypeStoreSize(DataTy));
}

void VASTModuleBuilder::buildMemoryTransaction(Value *Addr, Value *Data,
                                               unsigned PortNum, Instruction &I){
  BasicBlock *ParentBB = I.getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);
  VASTMemoryBus *Bus = getMemBus(PortNum);

  // Build the logic to start the transaction.
  VASTSeqOp *Op = VM->lauchInst(Slot, VASTImmediate::True, Data ? 4 : 3, &I,
                                VASTSeqInst::Launch);
  unsigned CurOperandIdx = 0;

  // Emit Address.
  Op->addSrc(getAsOperandImpl(Addr), CurOperandIdx++, false,
             Data ? Bus->getWAddr() : Bus->getRAddr());

  if (Data) {
    // Assign store data.
    VASTValPtr ValToStore = getAsOperandImpl(Data);
    assert(ValToStore->getBitWidth() <= Bus->getDataWidth()
           && "Storing data that exceed the width of databus!");
    Op->addSrc(ValToStore, CurOperandIdx++, false, Bus->getWData());
  }

  // Compute the byte enable.
  VASTValPtr ByteEn
    = Builder.getImmediate(getByteEnable(Addr), Bus->getByteEnWdith());
  Op->addSrc(ByteEn, CurOperandIdx++, false,
             Data ? Bus->getWByteEn() : Bus->getRByteEn());

  // Enable the memory bus at the same slot.
  Op->addSrc(VASTImmediate::True, CurOperandIdx, false,
             Data ? Bus->getWEnable() : Bus->getREnable());

  // Disable the memory bus at the next slot.
  Slot = advanceToNextSlot(Slot);

  // Read the result of the memory transaction.
  if (Data == 0) {
    unsigned Latency = getFUDesc<VFUMemBus>()->getReadLatency();
    // Please note that we had already advance 1 slot after we lauch the
    // load/store to disable the load/store. Now we need only wait Latency - 1
    // slots to get the result.
    Slot = advanceToNextSlot(Slot, Latency - 1);
    // Get the input port from the memory bus.
    VASTSeqValue *Result = getOrCreateSeqVal(&I, I.getName());
    assert(Result->getBitWidth() <= Bus->getDataWidth()
           && "Loading data that exceed the width of databus!");
    VASTValPtr V
      = Builder.buildBitSliceExpr(Bus->getRData(), Result->getBitWidth(), 0);
    VM->latchValue(Result, V, Slot, VASTImmediate::True, &I, Latency);
    // Move the the next slot so that the other operations are not conflict with
    // the current memory operations.
    advanceToNextSlot(Slot);
  }
}

void VASTModuleBuilder::buildBRAMTransaction(Value *Addr, Value *Data,
                                             unsigned BRAMNum, Instruction &I) {
  bool IsWrite = Data != 0;
  VASTNode *Node = getBlockRAM(BRAMNum);

  BasicBlock *ParentBB = I.getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);

  // The block RAM maybe degraded.
  if (VASTRegister *R = dyn_cast<VASTRegister>(Node)) {
    VASTSeqValue *V = R->getValue();
    if (IsWrite) {
      VASTValPtr Src = getAsOperandImpl(Data);
      VASTSeqInst *SeqInst
        = VM->lauchInst(Slot, VASTImmediate::True, 1, &I, VASTSeqInst::Latch);
      // The static registers only be written when the function exit.
      SeqInst->addSrc(Src, 0, false, V);
    } else {
      // Also index the address port as the result of the block RAM read.
      Builder.indexVASTExpr(&I, V);
    }

    return;
  }

  VASTBlockRAM *BRAM = cast<VASTBlockRAM>(Node);
  // Get the address port and build the assignment.
  VASTValPtr AddrVal = getAsOperandImpl(Addr);
  unsigned SizeInBytes = BRAM->getWordSize() / 8;
  unsigned Alignment = Log2_32_Ceil(SizeInBytes);
  AddrVal = Builder.buildBitSliceExpr(AddrVal, AddrVal->getBitWidth(), Alignment);

  VASTSeqOp *Op = VM->lauchInst(Slot, VASTImmediate::True, IsWrite ? 2 : 1, &I,
                                VASTSeqInst::Launch);

  VASTSeqValue *AddrPort = IsWrite ? BRAM->getWAddr(0) : BRAM->getRAddr(0);

  Op->addSrc(AddrVal, 0, false, AddrPort);
  // Also assign the data to write to the dataport of the block RAM.
  if (IsWrite) {
    VASTSeqValue *DataPort = BRAM->getWData(0);
    VASTValPtr DataToStore = getAsOperandImpl(Data);
    assert(DataToStore->getBitWidth() == BRAM->getWordSize()
           && "Write to BRAM data width not match!");
    Op->addSrc(DataToStore, 1, false, DataPort);
  }

  // Wait for 1 cycles and get the result for the read operation.
  if (!IsWrite) {
    Slot = advanceToNextSlot(Slot);
    VASTSeqValue *Result = getOrCreateSeqVal(&I, I.getName());
    assert(Result->getBitWidth() == BRAM->getWordSize()
           && "Read from BRAM data width not match!");
    // Use the the value from address port as the result of the block RAM read.
    VM->latchValue(Result, BRAM->getRData(0), Slot, VASTImmediate::True, &I, 1);
  }
  // Move the the next slot so that the other operations are not conflict with
  // the current memory operations.
  advanceToNextSlot(Slot);
}

//===----------------------------------------------------------------------===//
namespace llvm {
  void initializeVASTModuleAnalysisPass(PassRegistry &Registry);
}

namespace {
struct VASTModuleAnalysis : public FunctionPass {
  VASTModule *VM;

  static char ID;

  VASTModuleAnalysis() : FunctionPass(ID), VM(0) {
    initializeVASTModuleAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
  void releaseMemory();
  void getAnalysisUsage(AnalysisUsage &AU) const;

  operator VASTModule*() const { return VM; }
};
}

INITIALIZE_PASS_BEGIN(VASTModuleAnalysis,
                      "vast-module-builder", "VASTModule Builder",
                      false, true)
  INITIALIZE_AG_DEPENDENCY(HLSAllocation)
  INITIALIZE_PASS_DEPENDENCY(BasicBlockTopOrder)
INITIALIZE_PASS_END(VASTModuleAnalysis,
                    "vast-module-builder", "VASTModule Builder",
                    false, true)

bool VASTModuleAnalysis::runOnFunction(Function &F) {
  assert(VM == 0 && "Module has been already created!");
  VM = new VASTModule(F);

  VASTModuleBuilder Builder(VM, getAnalysisIfAvailable<DataLayout>(),
                            getAnalysis<HLSAllocation>());

  Builder.emitFunctionSignature(&F);

  // Allocate the submodules.
  Builder.allocateSubModules();

  // Build the Submodule.
  Builder.connectEntryState(&F.getEntryBlock());

  // Build the slot for each BB.
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
    Builder.visitBasicBlock(I);

  // Release the dead objects generated during the VM construction.
  VM->gc();

  return false;
}

void VASTModuleAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HLSAllocation>();
  AU.addRequiredID(BasicBlockTopOrderID);
  AU.setPreservesAll();
}

void VASTModuleAnalysis::releaseMemory() {
  if (VM == 0) return;

  delete VM;
  VM = 0;
}

char VASTModuleAnalysis::ID = 0;

//===----------------------------------------------------------------------===//
void VASTModulePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredTransitive<VASTModuleAnalysis>();
  AU.addPreserved<VASTModuleAnalysis>();
  AU.addPreservedID(BasicBlockTopOrderID);
  AU.addPreserved<AliasAnalysis>();
  AU.addPreserved<ScalarEvolution>();
  AU.addPreserved<HLSAllocation>();
  AU.addPreserved<DependenceAnalysis>();
  AU.setPreservesCFG();
}

bool VASTModulePass::runOnFunction(Function &F) {
  VASTModule &VM = *getAnalysis<VASTModuleAnalysis>();
  bool changed = runOnVASTModule(VM);
  if (changed) VM.gc();
  return changed;
}

void VASTModulePass::print(raw_ostream &OS) const {

}

// Initialize all passed required by a VASTModulePass.
VASTModulePass::VASTModulePass(char &ID) : FunctionPass(ID) {
  initializeVASTModuleAnalysisPass(*PassRegistry::getPassRegistry());
}
