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

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/VASTSubModules.h"

#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-vast-module-analysis"
#include "llvm/Support/Debug.h"

#include <map>

using namespace llvm;
STATISTIC(NumIPs, "Number of IPs Instantiated");

namespace {
// Note: Create the memory bus builder will add the input/output ports of the
// memory bus implicitly. We should add these ports after function
// "emitFunctionSignature" is called, which add some other ports that need to
// be added before input/output ports of memory bus.
struct MemBusBuilder {
  VASTModule *VM;
  VASTExprBuilder &Builder;
  VFUMemBus *Bus;
  unsigned BusNum;
  VASTWire *MembusEn, *MembusCmd, *MemBusAddr, *MemBusOutData, *MemBusByteEn;
  // Helper class to build the expression.
  VASTExprHelper EnExpr, CmdExpr, AddrExpr, OutDataExpr, BeExpr;

  VASTWire *createOutputPort(const std::string &PortName, unsigned BitWidth,
                             VASTRegister *&LocalEn, VASTExprHelper &Expr) {
    // We need to create multiplexer to allow current module and its submodules
    // share the bus.
    std::string PortReg = PortName + "_r";
    VASTRegister *LocalReg = VM->addRegister(PortReg, BitWidth);
    VASTPort *P = VM->addOutputPort(PortName, BitWidth, VASTModule::Others,
                                    false);

    // Are we creating the enable port?
    if (LocalEn == 0) {
      // Or all enables together to generate the enable output,
      // we use And Inverter Graph here.
      Expr.init(VASTExpr::dpAnd, BitWidth, true);
      // Add the local enable.
      assert(Expr.BuildNot && Expr.Opc == VASTExpr::dpAnd
             && "It is not building an Or Expr!");
      VASTValPtr V = Builder.buildNotExpr(LocalReg->getValue());
      Expr.addOperand(V);
      LocalEn = LocalReg;
    } else {
      Expr.init(VASTExpr::dpMux, BitWidth);
      // Select the local signal if local enable is true.
      Expr.addOperand(LocalEn->getValue());
      Expr.addOperand(LocalReg->getValue());
    }

    return cast<VASTWire>(P->getValue());
  }

  void addSubModuleOutPort(VASTSubModule *SubMod, VASTWire *OutputValue,
                           unsigned BitWidth, VASTWire *&SubModEn,
                           VASTExprHelper &Expr) {
    std::string ConnectedWireName
      = SubMod->getPortName(OutputValue->getName());

    VASTWire *SubModWire = VM->addWire(ConnectedWireName, BitWidth);

    // Are we creating the enable signal from sub module?
    if (SubModEn == 0) {
      // Or all enables together to generate the enable output.
      // we use And Inverter Graph here.
      assert(Expr.BuildNot && Expr.Opc == VASTExpr::dpAnd
             && "It is not building an Or Expr!");
      VASTValPtr V = Builder.buildNotExpr(SubModWire);
      Expr.addOperand(V);
      SubModEn = SubModWire;
    } else {
      // Select the signal from submodule if sub module enable is true.
      Expr.addOperand(SubModEn);
      Expr.addOperand(SubModWire);
    }

    SubMod->addOutPort(OutputValue->getName(), SubModWire);
  }

  void addSubModule(VASTSubModule *SubMod) {
    VASTWire *SubModEn = 0;
    addSubModuleOutPort(SubMod, MembusEn, 1, SubModEn, EnExpr);
    // Output ports.
    addSubModuleOutPort(SubMod, MembusCmd, VFUMemBus::CMDWidth, SubModEn,
                        CmdExpr);
    addSubModuleOutPort(SubMod, MemBusAddr, Bus->getAddrWidth(), SubModEn,
                        AddrExpr);
    addSubModuleOutPort(SubMod, MemBusOutData, Bus->getDataWidth(), SubModEn,
                        OutDataExpr);
    addSubModuleOutPort(SubMod, MemBusByteEn, Bus->getDataWidth()/8, SubModEn,
                        BeExpr);

    // Add the pseudo drivers to input ports.
    SubMod->addInPort(VFUMemBus::getInDataBusName(BusNum), 0);
    SubMod->addInPort(VFUMemBus::getReadyName(BusNum), 0);
  }

  MemBusBuilder(VASTModule *VM, VASTExprBuilder &Builder, unsigned N)
    : VM(VM), Builder(Builder), Bus(getFUDesc<VFUMemBus>()), BusNum(N) {
  }

  void addMemPorts() {
    // Build the ports for current module.
    FuncUnitId ID(VFUs::MemoryBus, BusNum);
    // We need to create multiplexer to allow current module and its submodules
    // share the memory bus.
    VM->setFUPortBegin(ID);
    // The enable signal for local memory bus.
    VASTRegister *LocalEn = 0;
    // Control ports.
    MembusEn =
      createOutputPort(VFUMemBus::getEnableName(BusNum), 1, LocalEn, EnExpr);
    MembusCmd =
      createOutputPort(VFUMemBus::getCmdName(BusNum), VFUMemBus::CMDWidth,
                       LocalEn, CmdExpr);

    // Address port.
    MemBusAddr =
      createOutputPort(VFUMemBus::getAddrBusName(BusNum), Bus->getAddrWidth(),
                       LocalEn, AddrExpr);
    // Data ports.
    VM->addInputPort(VFUMemBus::getInDataBusName(BusNum), Bus->getDataWidth());
    MemBusOutData =
      createOutputPort(VFUMemBus::getOutDataBusName(BusNum),
                       Bus->getDataWidth(), LocalEn, OutDataExpr);
    // Byte enable.
    MemBusByteEn =
      createOutputPort(VFUMemBus::getByteEnableName(BusNum),
                       Bus->getDataWidth() / 8, LocalEn, BeExpr);
    // Bus ready.
    VM->addInputPort(VFUMemBus::getReadyName(BusNum), 1);
  }

  void buildMemBusMux() {
    VM->assign(MembusEn, Builder.buildExpr(EnExpr));
    VM->assign(MembusCmd, Builder.buildExpr(CmdExpr));
    VM->assign(MemBusAddr, Builder.buildExpr(AddrExpr));
    VM->assign(MemBusOutData, Builder.buildExpr(OutDataExpr));
    VM->assign(MemBusByteEn, Builder.buildExpr(BeExpr));
  }
};

struct VASTModuleBuilder : public MinimalDatapathContext,
                           public InstVisitor<VASTModuleBuilder, void> {
  DatapathBuilder Builder;
  VASTModule *VM;
  DataLayout *TD;
  // FIXME: Allocate enough MBBuilder according to the memory bus allocation.
  MemBusBuilder MBBuilder;
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

  //===--------------------------------------------------------------------===//
  void allocateSubModules(CallGraph &CG);
  //===--------------------------------------------------------------------===//
  VASTSeqValue *getOrCreateSeqVal(Value *V, const Twine &Name);

  VASTValPtr getAsOperandImpl(Value *Op, bool GetAsInlineOperand = true);

  VASTWire *createWrapperWire(GlobalVariable *GV);
  //===--------------------------------------------------------------------===//
  void connectEntryState(BasicBlock *EntryBB);

  std::map<BasicBlock*, std::pair<VASTSlot*, VASTSlot*> > BB2SlotMap;
  unsigned NumSlots;
  VASTSlot *getOrCreateLandingSlot(BasicBlock *BB) {
    std::pair<VASTSlot*, VASTSlot*> &Slots = BB2SlotMap[BB];

    if (Slots.first == 0) {
      assert(Slots.second == 0 && "Unexpected Latest slot without landing slot!");
      Slots.first = (Slots.second = VM->createSlot(++NumSlots, BB));
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
    addSuccSlot(CurSlot, Slot, VASTImmediate::True);
    return Slot;
  }

  VASTSlot *advanceToNextSlot(VASTSlot *CurSlot, unsigned NumSlots) {
    VASTSlot *S = CurSlot;
    for (unsigned i = 0; i < NumSlots; ++i)
      S = advanceToNextSlot(S);

    return S;
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                   BasicBlock *DstBB = 0) {
    // If the Br is already exist, simply or the conditions together.
    if (VASTSeqSlotCtrl *SlotBr = S->getBrToSucc(NextSlot)) {
      VASTValPtr Pred = SlotBr->getPred();
      SlotBr->getPred().replaceUseBy(Builder.buildOrExpr(Pred, Cnd, 1));
      if (DstBB) SlotBr->annotateValue(DstBB);
    }

    S->addSuccSlot(NextSlot);
    VASTSeqSlotCtrl *SlotBr = VM->createSlotCtrl(NextSlot->getValue(), S, Cnd,
                                                 VASTSeqSlotCtrl::SlotBr);
    if (DstBB) SlotBr->annotateValue(DstBB);
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
      VM(Module), TD(TD), MBBuilder(Module, Builder, 0), Allocation(Allocation),
      NumSlots(0)  {}
};
}

void VASTModuleBuilder::connectEntryState(BasicBlock *EntryBB) {
  VASTSlot *IdleSlot = VM->getStartSlot();
  VASTValue *StartPort = VM->getPort(VASTModule::Start).getValue();
  addSuccSlot(IdleSlot, IdleSlot, Builder.buildNotExpr(StartPort));
  // Disable the finish port at the idle slot.
  VM->createSlotCtrl(VM->getPort(VASTModule::Finish).getSeqVal(), IdleSlot,
                     VASTImmediate::True, VASTSeqSlotCtrl::Disable);

  SmallVector<VASTValPtr, 1> Cnds(1, StartPort);
  addSuccSlot(IdleSlot, getOrCreateLandingSlot(EntryBB), StartPort);
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
    =  VM->addRegister(SeqValName, BitWidth, 0, VASTNode::Data, 0);
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

    if (ID.getFUType() == VFUs::BRam)
      // FIXIME: Calculate the offset of the GV in the block RAM.
      return indexVASTExpr(GV, getOrCreateImmediate(0, getValueSizeInBits(GV)));

    // If the GV is assigned to the memory port 0, create a wrapper wire for it.
    return indexVASTExpr(GV, createWrapperWire(GV));
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
    // TEMPORARY HACK: Create randome value for UndefValue.
    // TODO: Create 'x' for UndefValue.
    return getOrCreateImmediate(intptr_t(V), SizeInBit);
  }

  llvm_unreachable("Unhandle value!");
}

VASTWire *VASTModuleBuilder::createWrapperWire(GlobalVariable *GV) {
  unsigned SizeInBits = getValueSizeInBits(GV);

  VASTWire *WrapperWire
    = VM->addWire("gv_" + ShangMangle(GV->getName()) + "_wrapper", SizeInBits);

  VASTLLVMValue *ValueOp
    = new (VM->getAllocator()) VASTLLVMValue(GV, SizeInBits);

  VM->assign(WrapperWire, ValueOp);

  return WrapperWire;
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
      VASTRegister *R = VM->addOpRegister(RegName, BitWidth, SubMod->getNum());
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
  if (SubMod) {
    MBBuilder.addSubModule(SubMod);
    return;
  }

  // Only build the following logics if we are building current module instead
  // of the submodule.
  MBBuilder.addMemPorts();

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

void VASTModuleBuilder::allocateSubModules(CallGraph &CG) {
  Function &F = *VM;
  CallGraphNode *CGN = CG[&F];

  typedef CallGraphNode::const_iterator iterator;
  for (iterator I = CGN->begin(), E = CGN->end(); I != E; ++I) {
    Function *Callee = I->second->getFunction();

    // Submodule for external function is ignored now.
    if (Callee->isDeclaration()) continue;

    // Create the submodule.
    const char *SubModName = Callee->getValueName()->getKeyData();

    // Ignore the function that is called more than once in the function.
    if (SubModules.count(SubModName)) continue;

    VASTSubModule *SubMod = VM->addSubmodule(SubModName, SubModules.size());
    emitFunctionSignature(Callee, SubMod);
    SubMod->setIsSimple();
    SubModules.GetOrCreateValue(SubModName, SubMod);
  }

  // Connect the membus for all submodules.
  MBBuilder.buildMemBusMux();

  // Allocate the block RAMs.
  ArrayRef<const GlobalVariable*> GVs = Allocation.getBRAMAllocation(&F);
  for (unsigned i = 0; i < GVs.size(); ++i) {
    const GlobalVariable *GV = GVs[i];
    FuncUnitId ID = Allocation.getMemoryPort(*GV);
    assert(ID.getFUType() == VFUs::BRam && "Bad block RAM allocation!");
    emitBlockRAM(ID.getFUNum(), GV);
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
    //uint64_t InitVal = 0;
    //if (Initializer) {
    //  // Try to retrieve the initialize value of the register, it may be a
    //  // ConstantInt or ConstPointerNull, we can safely ignore the later case
    //  // since the InitVal is default to 0.
    //  const ConstantInt *CI
    //    = dyn_cast_or_null<ConstantInt>(Initializer->getInitializer());
    //  assert((CI || !Initializer->hasInitializer()
    //          || isa<ConstantPointerNull>(Initializer->getInitializer()))
    //         && "Unexpected initialier!");
    //  if (CI) InitVal = CI->getZExtValue();
    //}

    VASTRegister *R = VM->addDataRegister(VFUBRAM::getOutDataBusName(BRAMNum),
                                          ElementSizeInBits, BRAMNum);
    bool Inserted = AllocatedBRAMs.insert(std::make_pair(BRAMNum, R)).second;
    assert(Inserted && "Creating the same BRAM twice?");
    (void) Inserted;
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
    VASTRegister *R = VM->addOpRegister(OpInfo[i].first, OpInfo[i].second, FNNum);
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

  for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI) {
    BasicBlock *SuccBB = *SI;
    VASTSlot *SuccSlot = getOrCreateLandingSlot(SuccBB);

    for (iterator I = SuccBB->begin(), E = SuccBB->getFirstNonPHI(); I != E; ++I) {
      PHINode *PN = cast<PHINode>(I);

      Value *LiveOutedFromBB = PN->DoPHITranslation(SuccBB, BB);
      VASTValPtr LiveOut = getAsOperandImpl(LiveOutedFromBB);
      VASTValPtr Pred = LatestSlot->getSuccCnd(SuccSlot);
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
    VM->lauchInst(CurSlot, VASTImmediate::True, NumOperands, &I,
                  VASTSeqInst::Latch);

  // Assign the return port if necessary.
  if (NumOperands) {
    VASTSeqValue *RetPort = VM->getRetPort().getSeqVal();
    // Please note that we do not need to export the definition of the value
    // on the return port.
    SeqInst->addSrc(getAsOperandImpl(I.getReturnValue()), 0, false, RetPort);
  }

  // Enable the finish port.
  VM->createSlotCtrl(VM->getPort(VASTModule::Finish).getSeqVal(), CurSlot,
                     VASTImmediate::True, VASTSeqSlotCtrl::Enable);

  // Construct the control flow.
  addSuccSlot(CurSlot, VM->getFinishSlot(), VASTImmediate::True);
}

void VASTModuleBuilder::visitUnreachableInst(UnreachableInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  // DIRTYHACK: Simply jump back the start slot.
  // Construct the control flow.
  addSuccSlot(CurSlot, VM->getFinishSlot(), VASTImmediate::True);
}

void VASTModuleBuilder::visitBranchInst(BranchInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  // TODO: Create alias operations.
  if (I.isUnconditional()) {
    BasicBlock *DstBB = I.getSuccessor(0);
    addSuccSlot(CurSlot, getOrCreateLandingSlot(DstBB), VASTImmediate::True,
                DstBB);
    return;
  }

  // Connect the slots according to the condition.
  VASTValPtr Cnd = getAsOperandImpl(I.getCondition());
  BasicBlock *TrueBB = I.getSuccessor(0);
  addSuccSlot(CurSlot, getOrCreateLandingSlot(TrueBB), Cnd, TrueBB);
  BasicBlock *FalseBB = I.getSuccessor(1);
  addSuccSlot(CurSlot, getOrCreateLandingSlot(FalseBB),
              Builder.buildNotExpr(Cnd), FalseBB);
}

void VASTModuleBuilder::visitSwitchInst(SwitchInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());

  VASTValPtr CndVal = getAsOperandImpl(I.getCondition());
  // The predicate for each non-default destination.
  SmallVector<VASTValPtr, 4> CasePreds;

  typedef SwitchInst::CaseIt CaseIt;
  for (CaseIt CI = I.case_begin(), CE = I.case_end(); CI != CE; ++CI) {
    BasicBlock *SuccBB = CI.getCaseSuccessor();
    VASTSlot *SuccSlot = getOrCreateLandingSlot(SuccBB);

    assert(CI.getCaseValueEx().getSize() == 1
           && "Range case is not supported yet!");
    const APInt &CasValInt = CI.getCaseValueEx().getSingleValue(0);
    VASTValPtr CaseVal = getOrCreateImmediate(CasValInt);
    VASTValPtr Pred = Builder.buildEQ(CndVal, CaseVal);
    CasePreds.push_back(Pred);
    addSuccSlot(CurSlot, SuccSlot, Pred);
  }

  // Jump to the default block when all the case value not match, i.e. all case
  // predicate is false.
  VASTValPtr DefaultPred = Builder.buildNotExpr(Builder.buildOrExpr(CasePreds, 1));
  addSuccSlot(CurSlot, getOrCreateLandingSlot(I.getDefaultDest()), DefaultPred);
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
  VASTSeqInst *Op = VM->lauchInst(Slot, VASTImmediate::True, Args.size(), Inst,
                                  VASTSeqInst::Launch);
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
    = VM->lauchInst(Slot, VASTImmediate::True, 2, &I, VASTSeqInst::Launch);
  buildSubModuleOperation(Op, SubMod, Ops);
}

void VASTModuleBuilder::buildSubModuleOperation(VASTSeqInst *Inst,
                                                VASTSubModule *SubMod,
                                                ArrayRef<VASTValPtr> Args) {
  for (unsigned i = 0; i < Args.size(); ++i)
    Inst->addSrc(Args[i], i, false, SubMod->getFanin(i));

  // Enable the start port of the submodule at the current slot.
  VASTSeqValue *Start = SubMod->getStartPort();
  VASTSlot *Slot = Inst->getSlot();
  VM->createSlotCtrl(Start, Slot, VASTImmediate::True, VASTSeqSlotCtrl::Enable);
  // Disable the start port of the submodule at the next slot.
  Slot = advanceToNextSlot(Slot);
  VM->createSlotCtrl(Start, Slot, VASTImmediate::True, VASTSeqSlotCtrl::Disable);
  VM->createSlotCtrl(SubMod->getFinPort(), Slot, VASTImmediate::True,
                     VASTSeqSlotCtrl::WaitReady);

  // Read the return value from the function if there is any.
  if (VASTSeqValue *RetPort = SubMod->getRetPort()) {
    VASTSeqValue *Result
      = getOrCreateSeqVal(Inst->getValue(), Inst->getValue()->getName());
    VM->latchValue(Result, RetPort, Slot, VASTImmediate::True, Inst->getValue());
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

  // Build the logic to start the transaction.
  VASTSeqOp *Op = VM->lauchInst(Slot, VASTImmediate::True, Data ? 4 : 3, &I,
                                VASTSeqInst::Launch);
  unsigned CurOperandIdx = 0;

  // Emit Address.
  std::string RegName = VFUMemBus::getAddrBusName(PortNum) + "_r";
  VASTSeqValue *R = VM->getSymbol<VASTSeqValue>(RegName);
  Op->addSrc(getAsOperandImpl(Addr), CurOperandIdx++, false, R);

  VASTValPtr WEn = VASTImmediate::False;

  if (Data) {
    // Assign store data.
    RegName = VFUMemBus::getOutDataBusName(PortNum) + "_r";
    R = VM->getSymbol<VASTSeqValue>(RegName);
    VASTValPtr ValToStore = getAsOperandImpl(Data);
    assert(ValToStore->getBitWidth() <= R->getBitWidth()
           && "Storing data that exceed the width of databus!");
    Op->addSrc(ValToStore, CurOperandIdx++, false, R);
    // Set write enable to 1.
    WEn = VASTImmediate::True;
  }

  // Assign the write enable.
  RegName = VFUMemBus::getCmdName(PortNum) + "_r";
  R = VM->getSymbol<VASTSeqValue>(RegName);
  Op->addSrc(WEn, CurOperandIdx++, false, R);

  // Compute the byte enable.
  RegName = VFUMemBus::getByteEnableName(PortNum) + "_r";
  R = VM->getSymbol<VASTSeqValue>(RegName);
  VASTValPtr ByteEn
    = Builder.getImmediate(getByteEnable(Addr), R->getBitWidth());
  Op->addSrc(ByteEn, CurOperandIdx++, false, R);

  // Enable the memory bus at the same slot.
  // FIXIME: Use the correct memory port number.
  RegName = VFUMemBus::getEnableName(PortNum) + "_r";
  VASTSeqValue *MemEn = VM->getSymbol<VASTSeqValue>(RegName);
  VM->createSlotCtrl(MemEn, Slot, VASTImmediate::True, VASTSeqSlotCtrl::Enable);
  // Disable the memory bus at the next slot.
  Slot = advanceToNextSlot(Slot);
  VM->createSlotCtrl(MemEn, Slot, VASTImmediate::True, VASTSeqSlotCtrl::Disable);

  // Read the result of the memory transaction.
  if (Data == 0) {
    unsigned Latency = getFUDesc<VFUMemBus>()->getReadLatency();
    // Please note that we had already advance 1 slot after we lauch the
    // load/store to disable the load/store. Now we need only wait Latency - 1
    // slots to get the result.
    Slot = advanceToNextSlot(Slot, Latency - 1);
    // Get the input port from the memory bus.
    RegName = VFUMemBus::getInDataBusName(PortNum);
    R = VM->getSymbol<VASTSeqValue>(RegName);
    VASTSeqValue *Result = getOrCreateSeqVal(&I, I.getName());
    assert(Result->getBitWidth() <= R->getBitWidth()
           && "Loading data that exceed the width of databus!");
    VM->latchValue(Result, R, Slot, VASTImmediate::True, &I);
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
      VASTSeqInst *Op
        = VM->lauchInst(Slot, VASTImmediate::True, 1, &I, VASTSeqInst::Launch);
      Op->addSrc(getAsOperandImpl(Data), 0, false, V);
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
  // DIRTY HACK: Because the Read address are also use as the data ouput port of
  // the block RAM, the block RAM read define its result at the address port.
  Op->addSrc(AddrVal, 0, !IsWrite, AddrPort);
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
    VM->latchValue(Result, AddrPort, Slot, VASTImmediate::True, &I);
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
  Builder.allocateSubModules(getAnalysis<CallGraph>());

  // Build the Submodule.
  Builder.connectEntryState(&F.getEntryBlock());

  // Build the slot for each BB.
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
    Builder.visitBasicBlock(I);

  return false;
}

void VASTModuleAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HLSAllocation>();
  AU.addRequired<CallGraph>();
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
  AU.setPreservesCFG();
}

bool VASTModulePass::runOnFunction(Function &F) {
  return runOnVASTModule(*getAnalysis<VASTModuleAnalysis>());
}

void VASTModulePass::print(raw_ostream &OS) const {

}

// Initialize all passed required by a VASTModulePass.
VASTModulePass::VASTModulePass(char &ID) : FunctionPass(ID) {
  initializeVASTModuleAnalysisPass(*PassRegistry::getPassRegistry());
}
