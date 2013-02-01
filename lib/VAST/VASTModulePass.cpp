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

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/VASTSubModules.h"

#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"

#include <map>

using namespace llvm;


namespace llvm {
// FIXME: Move to the VAST headers.
// Wrapper for the external values.
struct VASTExternalOperand : public VASTValue {
  const Value *const V;

  VASTExternalOperand(const Value *V, unsigned Size)
    : VASTValue(VASTNode::vastCustomNode, Size), V(V) {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTExternalOperand *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastCustomNode;
  }

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const {
    OS << *V << '[' << UB << ',' << LB << ']';
  }
};
}

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

struct VASTModuleBuilder : public EarlyDatapathBuilderContext {
  DatapathBuilder Builder;
  VASTModule *VM;
  DataLayout *TD;
  // FIXME: Allocate enough MBBuilder according to the memory bus allocation.
  MemBusBuilder MBBuilder;

  //===--------------------------------------------------------------------===//
  // Implement the functions of EarlyDatapathBuilderContext.
  VASTImmediate *getOrCreateImmediate(const APInt &Value) {
    return VM->getOrCreateImmediateImpl(Value);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
    unsigned UB, unsigned LB) {
      return VM->createExprImpl(Opc, Ops, UB, LB);;
  }

  VASTValPtr getAsOperandImpl(Value *Op, bool GetAsInlineOperand = true);

  //===--------------------------------------------------------------------===//

  //===--------------------------------------------------------------------===//
  void emitFunctionSignature(Function *F, VASTSubModule *SubMod = 0);
  void emitCommonPort(VASTSubModule *SubMod);

  void allocateSubModules();
  //===--------------------------------------------------------------------===//
  VASTSeqValue *getOrCreateSeqVal(Value *V, const Twine &Name);


  //===--------------------------------------------------------------------===//
  void connectEntryState(BasicBlock *EntryBB);

  std::map<BasicBlock*, VASTSlot*> LandingSlots;
  unsigned NumSlots;
  VASTSlot *getOrCreateLandingSlot(BasicBlock *BB) {
    VASTSlot *&LandingSlot = LandingSlots[BB];

    if (LandingSlot == 0)
      LandingSlot = VM->createSlot(++NumSlots, BB);

    return LandingSlot;
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd) {
    Builder.orEqual(S->getOrCreateSuccCnd(NextSlot), Cnd);
  }

  //===--------------------------------------------------------------------===//
  void visitBasicBlock(BasicBlock *BB);

  // Build the memory transaction.
  void visitLoadInst(LoadInst &I, VASTSlot *CurSlot);
  void visitStoreInst(StoreInst &I, VASTSlot *CurSlot);
  void visitReturnInst(ReturnInst &I, VASTSlot *CurSlot);

  //===--------------------------------------------------------------------===//
  VASTModuleBuilder(VASTModule *Module, DataLayout *TD)
    : Builder(*this, TD), VM(Module), TD(TD), MBBuilder(Module, Builder, 0),
      NumSlots(0)  {}
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
  Twine SeqValName = "v_" + Name+ "_r";
  VASTRegister *R
    =  VM->addRegister(SeqValName, BitWidth, 0, VASTNode::Data, 0);
  // V = VM->createSeqValue("v" + utostr_32(RegNo) + "r", BitWidth,
  //                        VASTNode::Data, RegNo, 0);

  // Index the value.
  Builder.indexVASTExpr(V, R->getValue());
  return R->getValue();
}

VASTValPtr VASTModuleBuilder::getAsOperandImpl(Value *Op,
                                               bool GetAsInlineOperand) {
  unsigned NumBits = Builder.getValueSizeInBits(Op);

  if (ConstantInt *Int = dyn_cast<ConstantInt>(Op))
    return getOrCreateImmediate(Int->getValue());

  if (VASTValPtr V = Builder.lookupExpr(Op)) {
    // Try to inline the operand if user ask to.
    if (GetAsInlineOperand) V = V.getAsInlineOperand();
    return V;
  }

  // Else we need to create a leaf node for the expression tree.
  VASTExternalOperand *ValueOp
    = VM->getAllocator().Allocate<VASTExternalOperand>();
    
  new (ValueOp) VASTExternalOperand(Op, NumBits);

  // Remember the newly create VASTValueOperand, so that it will not be created
  // again.
  Builder.indexVASTExpr(Op, ValueOp);
  return ValueOp;
}

//===----------------------------------------------------------------------===//
void VASTModuleBuilder::emitFunctionSignature(Function *F,
                                              VASTSubModule *SubMod) {
  VASTSlot *StartSlot = VM->getStartSlot();
  SmallVector<VASTSeqValue*, 4> Args;
  SmallVector<VASTValPtr, 4> ArgPorts;

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
    Args.push_back(SeqVal);
    ArgPorts.push_back(V);
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
  for (unsigned i = 0, e = Args.size(); i != e; ++i)
    VM->addAssignment(Args[i], ArgPorts[i], StartSlot, StartPort);
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
  VASTSlot *CurSlot = getOrCreateLandingSlot(BB);

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    // PHINodes will be handled in somewhere else.
    if (isa<PHINode>(I)) continue;

    // Try to build the datapath expressions.
    if (VASTValPtr V = Builder.visit(I)) {
      Builder.indexVASTExpr(I, V);
      continue;
    }

    switch (I->getOpcode()) {
    case Instruction::Ret:
      visitReturnInst(cast<ReturnInst>(*I), CurSlot);
      break;
    default: llvm_unreachable("Instruction not supported yet!"); break;
    }
  }
}

void VASTModuleBuilder::visitReturnInst(ReturnInst &I, VASTSlot *CurSlot) {
  // Assign the return port if necessary.
  if (I.getNumOperands()) {
    VASTSeqValue *RetPort = VM->getRetPort().getSeqVal();
    VM->addAssignment(RetPort, getAsOperandImpl(I.getReturnValue()), CurSlot,
                      VASTImmediate::True, &I);
  } else
    VM->createVirtSeqOp(CurSlot, VASTImmediate::True, 0, &I, true);

  // Construct the control flow.
  addSuccSlot(CurSlot, VM->getFinishSlot(), VASTImmediate::True);
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
