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

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/VASTSubModules.h"

#include "shang/Passes.h"

#include "llvm/Support/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include <map>

using namespace llvm;

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

  //===--------------------------------------------------------------------===//
  void emitFunctionSignature(Function *F, VASTSubModule *SubMod = 0);
  void emitCommonPort(VASTSubModule *SubMod);

  void allocateSubModules();
  //===--------------------------------------------------------------------===//
  VASTSeqValue *getOrCreateSeqVal(Value *V, const Twine &Name);


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

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd) {
    Builder.orEqual(S->getOrCreateSuccCnd(NextSlot), Cnd);
  }

  //===--------------------------------------------------------------------===//
  void visitBasicBlock(BasicBlock *BB);

  // Build the SeqOps from the LLVM Instruction.
  void visitReturnInst(ReturnInst &I);
  void visitBranchInst(BranchInst &I);

  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);

  //===--------------------------------------------------------------------===//
  void buildMemoryTransaction(Value *Addr, Value *Data, unsigned PortNum,
                              Instruction &Inst);
  unsigned getByteEnable(Value *Addr) const;

  void visitInstruction(Instruction &I) {
    I.dump();
    llvm_unreachable("Unhandled instruction!");
  }

  //===--------------------------------------------------------------------===//
  VASTModuleBuilder(VASTModule *Module, DataLayout *TD)
    : MinimalDatapathContext(*Module, TD), Builder(*this),
      VM(Module), TD(TD), MBBuilder(Module, Builder, 0), NumSlots(0)  {}
};
}

void VASTModuleBuilder::connectEntryState(BasicBlock *EntryBB) {
  VASTSlot *IdleSlot = VM->getStartSlot();
  VASTValue *StartPort = VM->getPort(VASTModule::Start).getValue();
  addSuccSlot(IdleSlot, IdleSlot, Builder.buildNotExpr(StartPort));

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
    // Also connedt the memory bus.
    MBBuilder.addSubModule(SubMod);
  } else { // If F is current function.
    VM->addInputPort("clk", 1, VASTModule::Clk);
    VM->addInputPort("rstN", 1, VASTModule::RST);
    VM->addInputPort("start", 1, VASTModule::Start);
    VM->addOutputPort("fin", 1, VASTModule::Finish);
  }
}

void VASTModuleBuilder::allocateSubModules() {
  // Connect the membus for all submodules.
  MBBuilder.buildMemBusMux();
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

  // Construct the control flow.
  addSuccSlot(CurSlot, VM->getFinishSlot(), VASTImmediate::True);
}

void VASTModuleBuilder::visitBranchInst(BranchInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  // TODO: Create alias operations.
  if (I.isUnconditional()) {
    addSuccSlot(CurSlot, getOrCreateLandingSlot(I.getSuccessor(0)),
                VASTImmediate::True);
    return;
  }

  // Connect the slots according to the condition.
  VASTValPtr Cnd = getAsOperandImpl(I.getCondition());
  addSuccSlot(CurSlot, getOrCreateLandingSlot(I.getSuccessor(0)), Cnd);
  addSuccSlot(CurSlot, getOrCreateLandingSlot(I.getSuccessor(1)), Cnd.invert());
}

void VASTModuleBuilder::visitLoadInst(LoadInst &I) {
  buildMemoryTransaction(I.getPointerOperand(), 0, 0, I);
}

void VASTModuleBuilder::visitStoreInst(StoreInst &I) {
  buildMemoryTransaction(I.getPointerOperand(), I.getValueOperand(), 0, I);
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
    // Please note that the data are not present when we are performing a load.
    Op->addSrc(getAsOperandImpl(Data), CurOperandIdx++, false, R);
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
    = Builder.getOrCreateImmediate(getByteEnable(Addr), R->getBitWidth());
  Op->addSrc(ByteEn, CurOperandIdx++, false, R);

  // Enable the memory bus at the same slot.
  // Disable the memory bus at the next slot.

  // Read the result of the memory transaction.
  if (Data == 0) {
    Slot = advanceToNextSlot(Slot, getFUDesc<VFUMemBus>()->getReadLatency());
    // Get the input port from the memory bus.
    RegName = VFUMemBus::getInDataBusName(PortNum);
    R = VM->getSymbol<VASTSeqValue>(RegName);
    VASTSeqValue *Result = getOrCreateSeqVal(&I, I.getName());
    VM->latchValue(Result, R, Slot, VASTImmediate::True, &I);
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
    initializeBasicBlockTopOrderPass(*PassRegistry::getPassRegistry());
    initializeVASTModuleAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
  void releaseMemory();
  void getAnalysisUsage(AnalysisUsage &AU) const;

  operator VASTModule*() const { return VM; }
};
}

INITIALIZE_PASS(VASTModuleAnalysis, "vast-module-builder", "VASTModule Builder",
                false, true)

bool VASTModuleAnalysis::runOnFunction(Function &F) {
  assert(VM == 0 && "Module has been already created!");
  VM = new VASTModule(F);

  VASTModuleBuilder Builder(VM, getAnalysisIfAvailable<DataLayout>());

  Builder.emitFunctionSignature(&F);

  // Allocate the submodules.
  Builder.allocateSubModules();

  // Build the Submodule.
  Builder.connectEntryState(&F.getEntryBlock());

  // Build the slot for each BB.
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
    Builder.visitBasicBlock(I);

  return false;
}

void VASTModuleAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
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
}

bool VASTModulePass::runOnFunction(Function &F) {
  return runOnVASTModule(*getAnalysis<VASTModuleAnalysis>());
}

void VASTModulePass::printVASTModule(raw_ostream &OS) const {

}

// Initialize all passed required by a VASTModulePass.
VASTModulePass::VASTModulePass(char &ID) : FunctionPass(ID) {
  initializeVASTModuleAnalysisPass(*PassRegistry::getPassRegistry());
}
