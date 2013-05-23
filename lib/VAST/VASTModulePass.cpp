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
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/InstIterator.h"
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
  VASTSeqValue *getOrCreateSeqValImpl(Value *V, const Twine &Name);
  VASTSeqValue *getOrCreateSeqVal(Value *V, const Twine &Name) {
    std::string SeqValName = "v_" + Name.str() + "_r";
    SeqValName = ShangMangle(SeqValName);
    return getOrCreateSeqValImpl(V, SeqValName);
  }

  static inline void intToStr(intptr_t V, SmallString<36> &S) {
    static char encoding_table[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                    'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                    '4', '5', '6', '7', '8', '9'}; //, '+', '/'};
    const unsigned table_size = array_lengthof(encoding_table);

    assert(V && "Cannot convert 0 yet!");
    while (V) {
      unsigned char Digit = V % table_size;
      S += encoding_table[Digit];
      V /= table_size;
    }
  }

  static StringRef translatePtr2Str(void *V, SmallString<36> &S) {
    S.push_back('_');
    intToStr(intptr_t(V), S);
    S.push_back('_');
    return S.str();
  }

  VASTSeqValue *getOrCreateSeqVal(Value *V) {
    SmallString<36> S;
    return getOrCreateSeqValImpl(V, translatePtr2Str(V, S));
  }

  VASTValPtr getAsOperandImpl(Value *Op, bool GetAsInlineOperand = true);

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

  VASTSlot *createSubGroup(BasicBlock *BB, VASTValPtr Cnd, VASTSlot *S) {
    VASTSlot *SubGrp = VM->createSlot(++NumSlots, BB, Cnd, true);
    // The subgroups are not actually the successors of S in the control flow.
    S->addSuccSlot(SubGrp, VASTSlot::SubGrp);
    return SubGrp;
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot,
                   VASTValPtr Cnd = VASTImmediate::True,
                   TerminatorInst *Inst = 0) {
    // If the Br is already exist, simply or the conditions together.
    assert(!S->hasNextSlot(NextSlot) && "Edge had already existed!");
    assert((S->getParent() == NextSlot->getParent()
           || NextSlot == VM->getFinishSlot())
          && "Cannot change Slot and BB at the same time!");
    assert(!NextSlot->IsSubGrp && "Unexpected subgroup!");
    S->addSuccSlot(NextSlot, VASTSlot::Sucessor);
    VASTSlotCtrl *SlotBr = VM->createSlotCtrl(NextSlot, S, Cnd);
    if (Inst) SlotBr->annotateValue(Inst);
  }

  //===--------------------------------------------------------------------===//
  void visitBasicBlock(BasicBlock *BB);
  void visitPHIsInSucc(VASTSlot *S, VASTValPtr Cnd, BasicBlock *CurBB);

  // Build the SeqOps from the LLVM Instruction.
  void visitReturnInst(ReturnInst &I);
  void visitBranchInst(BranchInst &I);

  void buildConditionalTransition(BasicBlock *DstBB, VASTSlot *CurSlot,
                                       VASTValPtr Cnd, TerminatorInst &I);

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

//===----------------------------------------------------------------------===//
VASTSeqValue *VASTModuleBuilder::getOrCreateSeqValImpl(Value *V,
                                                       const Twine &Name) {
  assert(!V->getType()->isVoidTy() && "Cannot create SeqVal for Inst!");
  VASTValPtr Val = Builder.lookupExpr(V);

  if (Val) {
    assert(!Val.isInverted() && isa<VASTSeqValue>(Val.get()) && "Bad value type!");
    return cast<VASTSeqValue>(Val.get());
  }

  // Create the SeqVal now.
  unsigned BitWidth = Builder.getValueSizeInBits(V);
  VASTRegister *R = VM->createRegister(Name, BitWidth, 0);
  VASTSeqValue *SeqVal = VM->createSeqValue(R->getSelector(), 0, V);

  // Index the value.
  Builder.indexVASTExpr(V, SeqVal);
  return SeqVal;
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
    const std::string WrapperName = ShangMangle(GV->getName());
    if (unsigned PortNum = ID.getFUNum()) {
      VASTMemoryBus *Bus = getMemBus(PortNum);
      unsigned StartOffset = Bus->getStartOffset(GV);
      VASTImmediate *Imm = getOrCreateImmediate(StartOffset, SizeInBits);
      // FIXME: Annotate the GV to the Immediate.
      return indexVASTExpr(GV, Imm);
    }
    
    // If the GV is assigned to the memory port 0, create a wrapper wire for it.
    return indexVASTExpr(GV, VM->addWire(WrapperName, SizeInBits, GV));
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
      return indexVASTExpr(V, Operand);
    }
    }
  }

  if (UndefValue *UDef = dyn_cast<UndefValue>(V)) {
    unsigned SizeInBits = getValueSizeInBits(UDef);
    SmallString<36> S;
    return indexVASTExpr(V, VM->addWire(translatePtr2Str(V, S), SizeInBits, UDef));
  }

  if (ConstantPointerNull *PtrNull = dyn_cast<ConstantPointerNull>(V)) {
    unsigned SizeInBit = getValueSizeInBits(PtrNull);
    return indexVASTExpr(V, getOrCreateImmediate(APInt::getNullValue(SizeInBit)));
  }

  llvm_unreachable("Unhandle value!");
}

//===----------------------------------------------------------------------===//
void VASTModuleBuilder::emitFunctionSignature(Function *F,
                                              VASTSubModule *SubMod) {
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
      VASTRegister *R = VM->createRegister(RegName, BitWidth);
      SubMod->addFanin(R->getSelector());
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

  VASTSlot *IdleSlot = VM->getStartSlot();

  // Create the virtual slot representing the idle loop.
  VASTValue *StartPort
    = cast<VASTInPort>(VM->getPort(VASTModule::Start)).getValue();
  VASTSlot *IdleSlotGrp
    = createSubGroup(0, Builder.buildNotExpr(StartPort), IdleSlot);
  addSuccSlot(IdleSlotGrp, IdleSlot, Builder.buildNotExpr(StartPort));

  // Create the virtual slot represent the entry of the CFG.
  BasicBlock *EntryBB = &F->getEntryBlock();
  VASTSlot *EntryGrp = createSubGroup(EntryBB, StartPort, IdleSlot);

  // Connect the launch slot to the landing slot, with a real edge (which
  // represent a state transition)
  addSuccSlot(EntryGrp, getOrCreateLandingSlot(EntryBB), StartPort);

  // Copy the value to the register.
  for (unsigned i = 0, e = ArgRegs.size(); i != e; ++i)
    VM->latchValue(ArgRegs[i], ArgPorts[i], EntryGrp, StartPort, Args[i]);
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
  Function &F = VM->getLLVMFunction();

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

    VASTRegister *R = VM->createRegister(VFUBRAM::getOutDataBusName(BRAMNum),
                                         ElementSizeInBits, InitVal);
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

  // Add the fanin registers.
  for (unsigned i = 0, e = OpInfo.size(); i < e; ++i) {
    VASTRegister *Reg = VM->createRegister(OpInfo[i].first, OpInfo[i].second);
    SubMod->addFanin(Reg->getSelector());
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
}

void VASTModuleBuilder::visitPHIsInSucc(VASTSlot *S, VASTValPtr Cnd,
                                        BasicBlock *CurBB) {
  BasicBlock *BB = S->getParent();
  assert(BB && "Unexpected null BB!");

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->getFirstNonPHI(); I != E; ++I) {
    PHINode *PN = cast<PHINode>(I);

    Value *LiveOutedFromBB = PN->DoPHITranslation(BB, CurBB);
    VASTValPtr LiveOut = getAsOperandImpl(LiveOutedFromBB);

    VASTSeqValue *PHISeqVal = getOrCreateSeqVal(PN);
    // Latch the incoming value when we are branching to the succ slot.
    VM->latchValue(PHISeqVal, LiveOut, S,  Cnd, PN);
  }
}


void VASTModuleBuilder::buildConditionalTransition(BasicBlock *DstBB,
                                                  VASTSlot *CurSlot,
                                                  VASTValPtr Cnd,
                                                  TerminatorInst &I) {
  // Create the virtual slot represent the launch of the design.
  VASTSlot *SubGrp = createSubGroup(DstBB, Cnd, CurSlot);
  // Build the branch operation before building the PHIs, make sure the PHIs
  // are placed after the branch operation targeting the same BB with PHIs.
  addSuccSlot(SubGrp, getOrCreateLandingSlot(DstBB), Cnd, &I);
  visitPHIsInSucc(SubGrp, Cnd, CurSlot->getParent());
}

void VASTModuleBuilder::visitReturnInst(ReturnInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  unsigned NumOperands = I.getNumOperands();
  VASTSeqInst *SeqInst =
    VM->lauchInst(CurSlot, VASTImmediate::True, NumOperands + 1, &I,
                  VASTSeqInst::Latch);

  // Assign the return port if necessary.
  if (NumOperands) {
    VASTSelector *RetPort = cast<VASTOutPort>(VM->getRetPort()).getSelector();
    // Please note that we do not need to export the definition of the value
    // on the return port.
    SeqInst->addSrc(getAsOperandImpl(I.getReturnValue()), 0, RetPort);
  }

  // Enable the finish port.
  VASTSelector *FinPort
    = cast<VASTOutPort>(VM->getPort(VASTModule::Finish)).getSelector();
  SeqInst->addSrc(VASTImmediate::True, NumOperands, FinPort);

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

    buildConditionalTransition(DstBB, CurSlot, VASTImmediate::True, I);
    return;
  }

  // Connect the slots according to the condition.
  VASTValPtr Cnd = getAsOperandImpl(I.getCondition());
  BasicBlock *TrueBB = I.getSuccessor(0);

  buildConditionalTransition(TrueBB, CurSlot, Cnd, I);

  BasicBlock *FalseBB = I.getSuccessor(1);
  buildConditionalTransition(FalseBB, CurSlot, Builder.buildNotExpr(Cnd), I);
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
    VASTValPtr Pred = CI->second;
    CasePreds.push_back(Pred);

    buildConditionalTransition(SuccBB, CurSlot, Pred, I);
  }

  // Jump to the default block when all the case value not match, i.e. all case
  // predicate is false.
  VASTValPtr DefaultPred = Builder.buildNotExpr(Builder.buildOrExpr(CasePreds, 1));
  BasicBlock *DefBB = I.getDefaultDest();

  buildConditionalTransition(DefBB, CurSlot, DefaultPred, I);
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
    Inst->addSrc(Args[i], i, SubMod->getFanin(i));
  // Assign to the enable port.
  Inst->addSrc(VASTImmediate::True, Args.size(), SubMod->getStartPort());

  Value *V = Inst->getValue();
  VASTSlot *Slot = Inst->getSlot();
  // Disable the start port of the submodule at the next slot.
  Slot = advanceToNextSlot(Slot);
  VM->createSlotCtrl(SubMod->getFinPort(), Slot, VASTImmediate::True)
      ->annotateValue(V);

  // Read the return value from the function if there is any.
  if (VASTWire *RetPort = SubMod->getRetPort()) {
    VASTSeqValue *Result = getOrCreateSeqVal(Inst->getValue());
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
  unsigned CurSrcIdx = 0;

  // Emit Address.
  Op->addSrc(getAsOperandImpl(Addr), CurSrcIdx++, 
             Data ? Bus->getWAddr() : Bus->getRAddr());

  if (Data) {
    // Assign store data.
    VASTValPtr ValToStore = getAsOperandImpl(Data);
    assert(ValToStore->getBitWidth() <= Bus->getDataWidth()
           && "Storing data that exceed the width of databus!");
    ValToStore = Builder.buildZExtExprOrSelf(ValToStore, Bus->getDataWidth());
    Op->addSrc(ValToStore, CurSrcIdx++, Bus->getWData());
  }

  // Compute the byte enable.
  VASTValPtr ByteEn
    = Builder.getImmediate(getByteEnable(Addr), Bus->getByteEnWdith());
  Op->addSrc(ByteEn, CurSrcIdx++, Data ? Bus->getWByteEn() : Bus->getRByteEn());

  // Enable the memory bus at the same slot.
  Op->addSrc(VASTImmediate::True, CurSrcIdx,
             Data ? Bus->getWEnable() : Bus->getREnable());

  // Read the result of the memory transaction.
  if (Data == 0) {
    unsigned Latency = getFUDesc<VFUMemBus>()->getReadLatency();
    // TODO: Enable each pipeline stage individually.
    // Please note that we had already advance 1 slot after we lauch the
    // load/store to disable the load/store. Now we need only wait Latency - 1
    // slots to get the result.
    Slot = advanceToNextSlot(Slot, Latency);
    // Get the input port from the memory bus.
    VASTSeqValue *Result = getOrCreateSeqVal(&I);
    assert(Result->getBitWidth() <= Bus->getDataWidth()
           && "Loading data that exceed the width of databus!");
    VASTValPtr V
      = Builder.buildBitSliceExpr(Bus->getRData(), Result->getBitWidth(), 0);
    VM->latchValue(Result, V, Slot, VASTImmediate::True, &I, Latency);
  }

  // Move the the next slot so that the other operations are not conflict with
  // the current memory operations.
  advanceToNextSlot(Slot);
}

void VASTModuleBuilder::buildBRAMTransaction(Value *Addr, Value *Data,
                                             unsigned BRAMNum, Instruction &I) {
  bool IsWrite = Data != 0;
  VASTNode *Node = getBlockRAM(BRAMNum);

  BasicBlock *ParentBB = I.getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);

  // The block RAM maybe degraded.
  if (VASTRegister *R = dyn_cast<VASTRegister>(Node)) {
    if (IsWrite) {
      VASTValPtr Src = getAsOperandImpl(Data);
      VASTSeqInst *SeqInst
        = VM->lauchInst(Slot, VASTImmediate::True, 1, &I, VASTSeqInst::Latch);
      // The static registers only be written when the function exit.
      SeqInst->addSrc(Src, 0, R->getSelector());
    } else {
      // Also index the address port as the result of the block RAM read.
      Builder.indexVASTExpr(&I, R->getSelector()->getSSAValue());
    }

    return;
  }

  VASTBlockRAM *BRAM = cast<VASTBlockRAM>(Node);
  // Get the address port and build the assignment.
  VASTValPtr AddrVal = getAsOperandImpl(Addr);
  unsigned SizeInBytes = BRAM->getWordSize() / 8;
  unsigned Alignment = Log2_32_Ceil(SizeInBytes);
  unsigned AddrWidth = BRAM->getAddrWidth();
  AddrVal = Builder.buildBitSliceExpr(AddrVal, AddrWidth + Alignment, Alignment);

  VASTSeqOp *Op = VM->lauchInst(Slot, VASTImmediate::True, IsWrite ? 2 : 1, &I,
                                VASTSeqInst::Launch);

  VASTSelector *AddrPort = IsWrite ? BRAM->getWAddr(0) : BRAM->getRAddr(0);

  Op->addSrc(AddrVal, 0, AddrPort);
  // Also assign the data to write to the dataport of the block RAM.
  if (IsWrite) {
    VASTSelector *DataPort = BRAM->getWData(0);
    VASTValPtr DataToStore = getAsOperandImpl(Data);
    assert(DataToStore->getBitWidth() == BRAM->getWordSize()
           && "Write to BRAM data width not match!");
    Op->addSrc(DataToStore, 1, DataPort);
  }

  // Wait for 1 cycles and get the result for the read operation.
  if (!IsWrite) {
    Slot = advanceToNextSlot(Slot);
    VASTSeqValue *Result = getOrCreateSeqVal(&I);
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
