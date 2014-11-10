//===-------------------- SIRBuild.cpp - IR2SIR -----------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SIRBuild pass, which construct the SIR from LLVM IR
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/Passes.h"

#include "llvm/InstVisitor.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

//------------------------------------------------------------------------//
char SIRInit::ID = 0;

Pass *llvm::createSIRInitPass() {
  return new SIRInit();
}
//------------------------------------------------------------------------//

INITIALIZE_PASS_BEGIN(SIRInit,
                      "shang-ir-init", "SIR Init",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRInit,
                    "shang-ir-init", "SIR Init",
                    false, true)

bool SIRInit::runOnFunction(Function &F) {
  DataLayout &TD = getAnalysis<DataLayout>();

  SM = new SIR(&F);

  // Initialize SIR from IR by transform llvm-inst to Shang-inst.
  SIRBuilder Builder(SM, TD);

  // Build the general interface(Ports) of the module.
  Builder.buildInterface(&F);

  // Visit the basic block in topological order.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    Builder.visitBasicBlock(*(*I));

  return false;
}

void SIRInit::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

void SIRBuilder::buildInterface(Function *F) {
  // Add basic ports by create Arguments for Function.
  C_Builder.createPort(SIRPort::Clk, "clk", 1);
  C_Builder.createPort(SIRPort::Rst, "rstN", 1);
  C_Builder.createPort(SIRPort::Start, "start", 1);

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
       I != E; ++I) {
    Argument *Arg = I;
    StringRef Name = Arg->getName();
    unsigned BitWidth = TD.getTypeSizeInBits(Arg->getType());

    C_Builder.createPort(SIRPort::ArgPort, Name, BitWidth);
  }
  
  Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD.getTypeSizeInBits(RetTy);

    C_Builder.createPort(SIRPort::RetPort, "return_value", BitWidth);
  }

  // Create StartSlot for module.
  SIRSlot *IdleStartSlot = C_Builder.createSlot(SM->getSlotsSize(), 0, 0, false);
  assert(IdleStartSlot && "We need to create a start slot here!");

  // Get the Start signal of module.
  Value *Start 
    = cast<SIRInPort>(SM->getPort(SIRPort::Start))->getValue();

  // Whole module will be run only when the Start signal is true.
  Value *InsertPosition = F->getEntryBlock().getFirstInsertionPt();
  Value *IdleLoopCondition = D_Builder.createSNotInst(Start, InsertPosition, true);
  C_Builder.createStateTransition(IdleStartSlot, IdleStartSlot, IdleLoopCondition);

  // If the Start signal is true, then slot will jump to the slot of first BB.
  BasicBlock *EntryBB = &F->getEntryBlock();
  SIRSlot *EntryBBSlot = C_Builder.getOrCreateLandingSlot(EntryBB);
  C_Builder.createStateTransition(IdleStartSlot, EntryBBSlot, Start);
}

void SIRBuilder::visitBasicBlock(BasicBlock &BB) {
  // Create the landing slot for this BB.
  SIRSlot *S = C_Builder.getOrCreateLandingSlot(&BB);

  // Hack: After implement the DataflowPass, we should treat
  // the Unreachable BB differently.

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; ++I) 
    visit(I);
}

//-----------------------------------------------------------------------//
// All Data-path instructions should be built by SIRDatapathBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitTruncInst(TruncInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitZExtInst(ZExtInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitSExtInst(SExtInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitPtrToIntInst(PtrToIntInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitIntToPtrInst(IntToPtrInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitBitCastInst(BitCastInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitSelectInst(SelectInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitICmpInst(ICmpInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitBinaryOperator(BinaryOperator &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

void SIRBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  SM->IndexDataPathInst(&I);
  D_Builder.visit(I);
}

//-----------------------------------------------------------------------//
// All Control-path instructions should be built by SIRCtrlRgnBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitBranchInst(BranchInst &I) {
  C_Builder.visit(I);
}

void SIRBuilder::visitReturnInst(ReturnInst &I) {
  C_Builder.visit(I);
}


/// Functions to provide basic information
unsigned SIRCtrlRgnBuilder::getBitWidth(Value *U) {
  return TD.getTypeSizeInBits(U->getType());
}

/// Functions to build Control Logic
Instruction *SIRCtrlRgnBuilder::createPseudoInst() {
  // For slots and ports, we create this pseudo SeqInst,
  // which will represent the assign operation to
  // the register of this slot or port.
  // The pseudo SeqInst will be:
  // SeqInst = 1'b1;
  // And the insert position will be the front of whole module.
  Value *InsertPosition = &*SM->getFunction()->getEntryBlock().getFirstInsertionPt();
  Value *PseudoInstOp = SM->createIntegerValue(1, 1);
  Value *PseudoInst = D_Builder.createShangInstPattern(PseudoInstOp,
                                                          SM->createIntegerType(1),
                                                          InsertPosition, Intrinsic::shang_pseudo,
                                                          true);
  return dyn_cast<Instruction>(PseudoInst);
}

SIRRegister *SIRCtrlRgnBuilder::createRegister(StringRef Name, unsigned BitWidth, 
                                               Instruction *SeqInst, uint64_t InitVal, 
                                               SIRRegister::SIRRegisterTypes T) {
  assert(SeqInst || T == SIRRegister::OutPort || T == SIRRegister::SlotReg
         && "Only Reg for Port or Slot can have no corresponding SeqInst!");

  if (!SeqInst) {
    // If the SeqInst is empty, then this register is created for
    // Slot or OutPort. In this circumstance, we can construct a
    // pseudo SeqInst for it.
    SeqInst = createPseudoInst();
    assert(SeqInst && "Unexpected empty PseudoSeqInst!");
  }

  assert(!SM->lookupSIRReg(SeqInst) && "Register already created before!");

  // Create the register.
  SIRSelector *Sel = new SIRSelector(Name, BitWidth);
  SIRRegister *Reg = new SIRRegister(Sel, InitVal, T, SeqInst);

  // Index the register and index it with the SeqInst.
  SM->IndexRegister(Reg);
  SM->IndexSeqInst2Reg(SeqInst, Reg);

  return Reg;
}

SIRPort *SIRCtrlRgnBuilder::createPort(SIRPort::SIRPortTypes T, StringRef Name, 
                                       unsigned BitWidth) {
  // InPort or OutPort?
  if (T <= SIRPort::InPort) {
    LLVMContext &C = SM->getContext();
    SIRPort *P = new SIRInPort(T, BitWidth, Name, C);
    SM->IndexPorts(P);
    return P;
  } else {
    // Record the Idx of RetPort
    if (T == SIRPort::RetPort) SM->setRetPortIdx(SM->getPortsSize()); 

    // Create the register for OutPort.
    SIRRegister *Reg = createRegister(Name, BitWidth, 0, 0,
                                      SIRRegister::OutPort);
    SIROutPort *P = new SIROutPort(T, Reg, BitWidth, Name);
    SM->IndexPorts(P);
    return P;
  }  
}

SIRSlot *SIRCtrlRgnBuilder::createSlot(unsigned SlotNum, BasicBlock *ParentBB,
  unsigned Schedule, bool IsSubGrp /* = false */) {
    typedef SmallVector<SIRSlot *, 8>::iterator slot_iterator;
    for (slot_iterator I = SM->slot_begin(), E = SM->slot_end(); I != E; I++) {
      assert((*I)->getSlotNum() != SlotNum
              && "The same slot had already been created!");
    }

    // Create a slot register for this slot.
    std::string Name = "Slot" + utostr_32(SlotNum) + "r";
    // If the slot is start slot, the InitVal should be 1.
    unsigned InitVal = !SlotNum ? 1: 0;
    SIRRegister *SlotGuardReg = createRegister(Name, 1, 0, InitVal,
      SIRRegister::SlotReg);

    SIRSlot *S = new SIRSlot(SlotNum, ParentBB, SlotGuardReg->getSeqInst(),
      SlotGuardReg, IsSubGrp, Schedule);

    // Store the slot.
    SM->IndexSlot(S);

    return S;  
}

SIRSlot *SIRCtrlRgnBuilder::getOrCreateLandingSlot(BasicBlock *BB) {
  // Get the landing slot if it is already created.
  std::pair<SIRSlot *, SIRSlot *> &Slots = BB2SlotMap[BB];

  if (Slots.first == 0) {
    // Create the landing slot for this BB.
    assert(Slots.second == 0 && "Unexpected latest slot!");


    Slots.first = 
      (Slots.second = createSlot(SM->getSlotsSize(), BB, 0));
  }

  return Slots.first;
}

SIRSlot *SIRCtrlRgnBuilder::getLatestSlot(BasicBlock *BB)  {
  std::map<BasicBlock*, std::pair<SIRSlot *, SIRSlot *> >::const_iterator
    at = BB2SlotMap.find(BB);
  assert(at != BB2SlotMap.end() && "Latest slot not found!");
  return at->second.second;
}

SIRSlot *SIRCtrlRgnBuilder::getStartSlot() {
  return *(SM->slot_begin());
}

void SIRCtrlRgnBuilder::createConditionalTransition(BasicBlock *DstBB,
                                                    SIRSlot *SrcSlot, Value *Guard) {
  SIRSlot *DstSlot = getOrCreateLandingSlot(DstBB);
  createStateTransition(SrcSlot, DstSlot, Guard);
  // The assignment is still be launched in SrcSlot, which lead to
  // a state transition to landing slot of DstBB.
  visitPHIsInSucc(SrcSlot, DstSlot, Guard, SrcSlot->getParent());
}

void SIRCtrlRgnBuilder::visitPHIsInSucc(SIRSlot *SrcSlot, SIRSlot *DstSlot,
                                        Value *Guard, BasicBlock *SrcBB) {
  BasicBlock *DstBB = DstSlot->getParent();
  assert(DstBB && "Unexpected null BB!");

  typedef BasicBlock::iterator iterator;
  for (iterator I = DstBB->begin(), E = DstBB->getFirstNonPHI(); I != E; ++I) {
    PHINode *PN = cast<PHINode>(I);
    unsigned BitWidth = TD.getTypeSizeInBits(PN->getType());

    // The value which lives out from SrcBB to DstBB;
    Value *LiveOutedFromSrcBB = PN->DoPHITranslation(DstBB, SrcBB);

    // Hack: we don't have methods to identify which BB is unreachable.
    // If the SrcBB cannot be reached, then the LiveOut will be zero.
    Value *LiveOut = LiveOutedFromSrcBB;
    
    // If the register already exist, then just assign to it.
    SIRRegister *PHISeqValReg = SM->lookupSIRReg(I);
    if (!PHISeqValReg) 
      PHISeqValReg = createRegister(PN->getName(), BitWidth, I);

    assignToReg(SrcSlot, Guard, LiveOut, PHISeqValReg);   
  }
}

void SIRCtrlRgnBuilder::createStateTransition(SIRSlot *SrcSlot, SIRSlot *DstSlot, Value *Cnd) {
  assert(!SrcSlot->hasNextSlot(DstSlot) && "Edge already existed!");

  SrcSlot->addSuccSlot(DstSlot, SIRSlot::Sucessor);
  // The State Transition is actually a SeqOp which assign a true/false value to
  // the Slot Reg in the right time to active DstSlot. So we can handle it like
  // a SeqOp.
  SIRRegister *Reg = DstSlot->getGuardReg();
  assignToReg(SrcSlot, Cnd, SM->createIntegerValue(1, 1), Reg);
}

void SIRCtrlRgnBuilder::assignToReg(SIRSlot *S, Value *Guard, Value *Src,
                                    SIRRegister *Dst) {  
  Value *InsertPosition = Dst->getSeqInst();
  // If the register is constructed for port or slot, 
  // we should appoint a insert point for it since
  // the SeqInst it holds is pseudo instruction.
  if (Dst->isOutPort() || Dst->isSlot()) {
    InsertPosition = &SM->getFunction()->getBasicBlockList().back();
  }

  // Associate the guard with the Slot guard.
  Value *FaninGuard = D_Builder.createSAndInst(Guard, S->getGuardReg()->getSeqInst(),
                                               InsertPosition, true);

  assert(getBitWidth(Src) == Dst->getBitWidth() && "BitWidth not matches!");
  assert(getBitWidth(Guard) == 1 && "Bad BitWidth of Guard Value!");

  // Assign the Src value to the register.
  Dst->addAssignment(Src, FaninGuard);
}

/// Functions to visit all control-path instructions

void SIRCtrlRgnBuilder::visitBranchInst(BranchInst &I) {
  SIRSlot *CurSlot = getLatestSlot(I.getParent());
  // Treat unconditional branch differently.
  if (I.isUnconditional()) {
    BasicBlock *DstBB = I.getSuccessor(0);
    createConditionalTransition(DstBB, CurSlot, SM->createIntegerValue(1, 1));
    return;
  }

  // Connect the slots according to the condition.
  Value *Cnd = I.getCondition();

  BasicBlock *TrueBB = I.getSuccessor(0);
  createConditionalTransition(TrueBB, CurSlot, Cnd);

  BasicBlock *FalseBB = I.getSuccessor(1);
  createConditionalTransition(FalseBB, CurSlot, D_Builder.createSNotInst(Cnd, &I, true));
}

void SIRCtrlRgnBuilder::visitReturnInst(ReturnInst &I) {
  // Get the latest slot of CurBB.
  SIRSlot *CurSlot = getLatestSlot(I.getParent());

  // Jump back to the start slot on return.
  createStateTransition(CurSlot, getStartSlot(), SM->createIntegerValue(1, 1));

  if (I.getNumOperands()) {
    SIRRegister *Reg = cast<SIROutPort>(SM->getRetPort())->getRegister();
    
    // Launch the instruction to assignment value to register.
    assignToReg(CurSlot, SM->createIntegerValue(1, 1),
                I.getReturnValue(), Reg);

    // Index the register with return instruction.
    SM->IndexSeqInst2Reg(&I, Reg);
  }
}



/// Functions to provide basic information of instruction

unsigned SIRDatapathBuilder::getBitWidth(Value *U) {
  // Since we create a pseudo instruction for these slots and ports register,
  // so we should handle these pseudo instruction differently when we meet them.
  if(!U) {
    SIRRegister *Reg = SM->lookupSIRReg(dyn_cast<Instruction>(U));
    assert(Reg && (Reg->isOutPort() || Reg->isSlot()) && "Unexpected Null Value!");

    return Reg->getBitWidth();
  }

  Type *Ty = U->getType();
  unsigned BitWidth = TD.getTypeSizeInBits(Ty);
  return BitWidth;
}

/// Functions to visit all data-path instructions

void SIRDatapathBuilder::visitTruncInst(TruncInst &I) {
  createSTruncInst(I.getOperand(0), getBitWidth(&I), 0,
                   &I, false);
}

void SIRDatapathBuilder::visitZExtInst(ZExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSZExtInst(I.getOperand(0), NumBits, &I, false);
}

void SIRDatapathBuilder::visitSExtInst(SExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSSExtInst(I.getOperand(0), NumBits, &I, false);
}

void SIRDatapathBuilder::visitPtrToIntInst(PtrToIntInst &I) {
  createSCastInst(I.getPointerOperand(), &I, false);
}

void SIRDatapathBuilder::visitIntToPtrInst(IntToPtrInst &I) {
  createSCastInst(I.getOperand(0), &I, false);
}

void SIRDatapathBuilder::visitBitCastInst(BitCastInst &I) {
  createSCastInst(I.getOperand(0), &I, false);
}

void SIRDatapathBuilder::visitSelectInst(SelectInst &I) {
  createSSelInst(I.getOperand(0), I.getOperand(1),
                 I.getOperand(2), &I, false);
}

void SIRDatapathBuilder::visitICmpInst(ICmpInst &I) {
  Value *Ops[] = {I.getOperand(0), I.getOperand(1)};
  createSICmpInst(I.getPredicate(), Ops, &I, false);
}

void SIRDatapathBuilder::visitBinaryOperator(BinaryOperator &I) {
  Value *Ops[] = {I.getOperand(0), I.getOperand(1)};

  switch (I.getOpcode()) {
  case Instruction::Add: createSAddInst(Ops, &I, false); return;
  case Instruction::Sub: createSSubInst(Ops, &I, false); return;
  case Instruction::Mul: createSMulInst(Ops, &I, false); return;

  case Instruction::Shl: createSShiftInst(Ops, &I, Intrinsic::shang_shl, false); return;
  case Instruction::AShr: createSShiftInst(Ops, &I, Intrinsic::shang_ashr, false); return;
  case Instruction::LShr: createSShiftInst(Ops, &I, Intrinsic::shang_lshr, false); return;

  case Instruction::UDiv: createSUDivInst(Ops, &I, false); return;
  case Instruction::SDiv: createSSDivInst(Ops, &I, false); return;

  case Instruction::And: createSAndInst(Ops, &I, false); return;
  case Instruction::Or:  createSOrInst(Ops, &I, false); return;
  case Instruction::Xor: createSXorInst(Ops, &I, false); return;

  default: llvm_unreachable("Unexpected opcode"); break;
  }

  return;
}

void SIRDatapathBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  return visitGEPOperator(cast<GEPOperator>(I), I);
}

void SIRDatapathBuilder::visitGEPOperator(GEPOperator &O, GetElementPtrInst &I) {
  //assert(O.getPointerOperand()->getType() == O.getType() && "Type not match!");
  Value *Ptr = O.getPointerOperand();

  // Cast the Ptr into int type so we can do the math operation below.
  Value *PtrVal = new PtrToIntInst(Ptr, IntegerType::getInt32Ty(I.getContext()),
    "SIRPtrToInt", &I);

  //Value *Test = cast<>
  // FIXME: All the pointer arithmetic are perform under the precision of
  // PtrSize, do we need to perform the arithmetic at the max available integer
  // width and truncate the result?
  unsigned PtrSize = getBitWidth(O.getPointerOperand());
  // Note that the pointer operand may be a vector of pointers. Take the scalar
  // element which holds a pointer.
  Type *Ty = O.getPointerOperandType()->getScalarType();

  typedef GEPOperator::op_iterator op_iterator;
  for (op_iterator OI = O.idx_begin(), E = O.op_end(); OI != E; ++OI) {
    const Value *Idx = *OI;
    if (StructType *StTy = dyn_cast<StructType>(Ty)) {
      unsigned Field = cast<ConstantInt>(Idx)->getZExtValue();
      if (Field) {
        // N = N + Offset
        uint64_t Offset = TD.getStructLayout(StTy)->getElementOffset(Field);
        PtrVal = createSAddInst(PtrVal, createSConstantInt(Offset, PtrSize),
          &I, true);
      }

      Ty = StTy->getElementType(Field);
    } else {
      Ty = cast<SequentialType>(Ty)->getElementType();

      // If this is a constant subscript, handle it quickly.
      if (const ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
        if (CI->isZero()) continue;
        uint64_t Offs = TD.getTypeAllocSize(Ty)
                        * cast<ConstantInt>(CI)->getSExtValue();
        PtrVal = createSAddInst(PtrVal, createSConstantInt(Offs, PtrSize),
          &I, true);
        continue;
      }

      // N = N + Idx * ElementSize;
      APInt ElementSize = APInt(PtrSize, TD.getTypeAllocSize(Ty));
      Value *IdxN = const_cast<Value*>(Idx);

      // If the index is smaller or larger than intptr_t, truncate or extend
      // it.
      IdxN = createSBitExtractInst(IdxN, PtrSize, 0, &I, true);

      // If this is a multiply by a power of two, turn it into a shl
      // immediately.  This is a very common case.
      if (ElementSize != 1) {
        if (ElementSize.isPowerOf2()) {
          unsigned Amt = ElementSize.logBase2();
          IdxN = createSShiftInst(IdxN, createSConstantInt(Amt, PtrSize),
                                  &I, Intrinsic::shang_shl, true);
        } else {
          Value *Scale = createSConstantInt(ElementSize.getSExtValue(),
                                            PtrSize);
          IdxN = createSMulInst(IdxN, Scale, &I, true);
        }
      }

      PtrVal = createSAddInst(PtrVal, IdxN, &I, true);
    }
  }

  Value *PtrResult = new IntToPtrInst(PtrVal, I.getType(), "SIRIntToPtr", &I);
  I.replaceAllUsesWith(PtrResult);
}

/// Functions to create Shang-Inst

Value *SIRDatapathBuilder::createShangInstPattern(ArrayRef<Value *> Ops, Type *RetTy,
                                                  Value *InsertPosition, Intrinsic::ID FuncID,
                                                  bool UsedAsArg) {
  // Insert a correctly-typed definition now.
  std::vector<Type *> FuncTy;
  // The return type
  FuncTy.push_back(RetTy);

  // The operand type
  for (unsigned i = 0; i < Ops.size(); ++i) {
    FuncTy.push_back(Ops[i]->getType());
  }  

  Module *M = SM->getModule();
  Value *Func = Intrinsic::getDeclaration(M, FuncID, FuncTy);

  if (Instruction *InsertBefore = dyn_cast<Instruction>(InsertPosition)) {
    Instruction *NewInst = CallInst::Create(Func, Ops, Func->getName(), InsertBefore);

    // Index all these data-path instructions.
    if (FuncID != Intrinsic::shang_pseudo)
      SM->IndexDataPathInst(NewInst);

    // If the inst is not used as an argument of other functions,
    // then it is used to replace the inst in IR
    if (!UsedAsArg) InsertBefore->replaceAllUsesWith(NewInst);
    return NewInst;
  }
  else if (BasicBlock *InsertAtEnd = dyn_cast<BasicBlock>(InsertPosition)) {
    Instruction *NewInst = CallInst::Create(Func, Ops, Func->getName(), InsertAtEnd);

    // Index all these data-path instructions.
    if (FuncID != Intrinsic::shang_pseudo)
      SM->IndexDataPathInst(NewInst);

    return NewInst;
  } 

  assert(false && "Unexpected InsertPosition!");
}

Value *SIRDatapathBuilder::createSBitCatInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                             bool UsedAsArg) {
    // Not finished //

    // Compute the RetTy
    unsigned RetBitWidth = 0;
    for (unsigned i = 0; i < Ops.size(); ++i) {
      RetBitWidth += getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_bit_cat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitExtractInst(Value *U, unsigned UB, unsigned LB,
                                                 Value *InsertPosition, bool UsedAsArg) {
    Value *Ops[] = {U, createSConstantInt(UB, 8), createSConstantInt(LB, 8)};

    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), UB - LB);

    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_bit_extract, UsedAsArg);
}

Value *SIRDatapathBuilder::createSCastInst(Value *U, Value *InsertPosition, bool UsedAsArg) {
  if (!UsedAsArg) {
    assert((!U || getBitWidth(U) == getBitWidth(InsertPosition))
      && "Cast between types with different size found!");
    InsertPosition->replaceAllUsesWith(U);
  }
  return U;
}

Value *SIRDatapathBuilder::createSTruncInst(Value *U, unsigned UB, unsigned LB,
                                            Value *InsertPosition, bool UsedAsArg) {
    // Truncate the value by bit-extract expression.
    return createSBitExtractInst(U, UB, LB, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInst(Value *U, unsigned DstBitWidth, Value *InsertPosition,
                                           bool UsedAsArg) {
    unsigned SrcBitWidth = getBitWidth(U);
    assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
    unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
    Value *Zero = createSConstantInt(0, 1);

    Value *ExtendBits = createSBitRepeatInst(Zero, NumExtendBits, InsertPosition, true);
    Value *Ops[] = { ExtendBits, U };
    return createSBitCatInst(Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInstOrSelf(Value *U, unsigned DstBitWidth, Value *InsertPosition,
                                                 bool UsedAsArg) {
    if (getBitWidth(U) < DstBitWidth)
      return createSZExtInst(U, DstBitWidth, InsertPosition, UsedAsArg);
    else if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);

    return U;
}

Value *SIRDatapathBuilder::createSSExtInst(Value *U, unsigned DstBitWidth, Value *InsertPosition,
                                           bool UsedAsArg) {
    unsigned SrcBitWidth = getBitWidth(U);
    assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
    unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
    Value *SignBit = getSignBit(U, InsertPosition);

    Value *ExtendBits = createSBitRepeatInst(SignBit, NumExtendBits, InsertPosition, true);
    Value *Ops[] = { ExtendBits, U };
    return createSBitCatInst(Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSExtInstOrSelf(Value *U, unsigned DstBitWidth, Value *InsertPosition,
                                                 bool UsedAsArg) {
    if (getBitWidth(U) < DstBitWidth)
      return createSSExtInst(U, DstBitWidth, InsertPosition, UsedAsArg);
    else if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);

    return U;
}

Value *SIRDatapathBuilder::createSRAndInst(Value *U, Value *InsertPosition,
                                           bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), 1);

    return createShangInstPattern(U, RetTy, InsertPosition, Intrinsic::shang_rand, UsedAsArg);
}

Value *SIRDatapathBuilder::createSRXorInst(Value *U, Value *InsertPosition,
                                           bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), 1);

    return createShangInstPattern(U, RetTy, InsertPosition, Intrinsic::shang_rxor, UsedAsArg);
}

Value *SIRDatapathBuilder::createSROrInst(Value *U, Value *InsertPosition,
                                          bool UsedAsArg) {
    // A | B .. | Z = ~(~A & ~B ... & ~Z).
    Value *NotU = createSNotInst(U, InsertPosition, true);
    return createSNotInst(createSRAndInst(NotU, InsertPosition, true),
                          InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSNEInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                         bool UsedAsArg) {
    // Get the bitwise difference by Xor.
    Value *BitWissDiff = createSXorInst(Ops, InsertPosition, true);
    // If there is any bitwise difference, then LHS and RHS is not equal.
    return createSROrInst(BitWissDiff, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                         bool UsedAsArg) {
    return createSNotInst(createSNEInst(Ops, InsertPosition, true),
      InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                         bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSEQInst(Ops, InsertPosition, true);
}

Value *SIRDatapathBuilder::createSdpSGTInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                            bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), 1);

    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_sgt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSdpUGTInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                            bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), 1);

    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_ugt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitRepeatInst(Value *U, unsigned RepeatTimes, Value *InsertPosition,
                                                bool UsedAsArg) {
    if (RepeatTimes == 1) {
      if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);
      return U;
    }

    // Compute the RetTy
    IntegerType *RetTy = SM->createIntegerType(getBitWidth(U) * RepeatTimes);
    Value *Ops[] = { U, createSConstantInt(RepeatTimes, 32)};
    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_bit_repeat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSelInst(Value *Cnd, Value *TrueV, Value *FalseV,
                                          Value *InsertPosition, bool UsedAsArg) {
    assert(getBitWidth(Cnd) == 1 && "Bad condition width!");

    unsigned Bitwidth = getBitWidth(TrueV);
    assert(getBitWidth(FalseV) == Bitwidth && "Bad bitwidth!");

    if (ConstantInt *C = dyn_cast<ConstantInt>(Cnd)) {
      Value *Result = C->getValue().getBoolValue() ? TrueV : FalseV; 
      if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Result);
      return Result;
    }

    Value *NewCnd = createSBitRepeatInst(Cnd, Bitwidth, InsertPosition, true);
    return createSOrInst(createSAndInst(NewCnd, TrueV, InsertPosition, true),
      createSAndInst(createSNotInst(NewCnd, InsertPosition, true),
      FalseV, InsertPosition, true),
      InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                                               Value *InsertPosition, bool UsedAsArg) {
    Value *ICmpResult = createSICmpInst(Predicate, Ops, InsertPosition, true);
    Value *EQResult = createSEQInst(Ops, InsertPosition, true);
    Value *NewOps[] = {EQResult, ICmpResult};
    return createSOrInst(NewOps, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate, Value *LHS, Value *RHS,
                                               Value *InsertPosition, bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSIcmpOrEqInst(Predicate, Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSICmpInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                                           Value *InsertPosition, bool UsedAsArg) {
    assert(Ops.size() == 2 && "There must be two operands!");
    assert(getBitWidth(Ops[0]) == getBitWidth(Ops[1])
      && "Bad icmp bitwidth!");
    SmallVector<Value *, 2> NewOps;
    for (int i = 0; i < Ops.size(); i++)
      NewOps.push_back(Ops[i]);

    switch (Predicate) {
    case CmpInst::ICMP_NE:
      return createSNEInst(NewOps, InsertPosition, UsedAsArg);
    case CmpInst::ICMP_EQ:
      return createSEQInst(NewOps, InsertPosition, UsedAsArg);

    case CmpInst::ICMP_SLT:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_SGT:
      return createSdpSGTInst(NewOps, InsertPosition, UsedAsArg);

    case CmpInst::ICMP_ULT:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_UGT:
      return createSdpUGTInst(NewOps, InsertPosition, UsedAsArg);

    case CmpInst::ICMP_SLE:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_SGE:
      return createSIcmpOrEqInst(CmpInst::ICMP_SGT, NewOps, InsertPosition, UsedAsArg);
      //return buildICmpOrEqExpr(VASTExpr::dpSGT, LHS, RHS);

    case CmpInst::ICMP_ULE:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_UGE:
      return createSIcmpOrEqInst(CmpInst::ICMP_UGT, NewOps, InsertPosition, UsedAsArg);

    default: break;
    }

    llvm_unreachable("Unexpected ICmp predicate!");
    return 0;
}

Value *SIRDatapathBuilder::createSNotInst(Value *U, Value *InsertPosition,
                                          bool UsedAsArg) {
    return createShangInstPattern(U, U->getType(), InsertPosition, Intrinsic::shang_not, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                          bool UsedAsArg) {
    return createShangInstPattern(Ops, Ops[0]->getType(), InsertPosition, Intrinsic::shang_add, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                          bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSAddInst(Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSubInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                          bool UsedAsArg) {
    Value *NewOps[] = { Ops[0],
                        createSNotInst(Ops[1], InsertPosition, true),
                        createSConstantInt(1, 1) };
    return createSAddInst(NewOps, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                          bool UsedAsArg) {
    // Handle the trivial case trivially.
    if (Ops.size() == 1) {
      if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
      return Ops[0];
    }

    // Compute the RetTy
    unsigned RetBitWidth = 1;
    for (unsigned i = 0; i < Ops.size(); ++i) {
      RetBitWidth *= getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_mul, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                          bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS};
    return createSMulInst(Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSUDivInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                           bool UsedAsArg) {
    // Compute the RetTy
    unsigned RetBitWidth = getBitWidth(Ops[0]);
    for (unsigned i = 1; i < Ops.size(); ++i) {
      RetBitWidth /= getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_udiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSDivInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                           bool UsedAsArg) {
    // Compute the RetTy
    unsigned RetBitWidth = getBitWidth(Ops[0]);
    for (unsigned i = 1; i < Ops.size(); ++i) {
      RetBitWidth /= getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(InsertPosition->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_sdiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSShiftInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                            Intrinsic::ID FuncID, bool UsedAsArg) {
    assert(Ops.size() == 2 && "The shift inst must have two operands!");
    Value *LHS = Ops[0]; Value *RHS = Ops[1];
    // Limit the shift amount so keep the behavior of the hardware the same as
    // the corresponding software.
    unsigned RHSMaxSize = Log2_32_Ceil(getBitWidth(LHS));
    if (getBitWidth(RHS) > RHSMaxSize) 
      RHS = createSBitExtractInst(Ops[1], RHSMaxSize, 0, InsertPosition, true);  
    Value *NewOps[] = {LHS, RHS};

    return createShangInstPattern(NewOps, LHS->getType(), InsertPosition, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSShiftInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                            Intrinsic::ID FuncID, bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSShiftInst(Ops, InsertPosition, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                          bool UsedAsArg) {
    // Handle the trivial case trivially.
    if (Ops.size() == 1) {
      if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
      return Ops[0];
    }

    return createShangInstPattern(Ops, Ops[0]->getType(), InsertPosition, Intrinsic::shang_and, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                          bool UsedAsArg) {
    Value *Ops[] = {LHS, RHS};
    //assert(TD.getTypeSizeInBits(LHS->getType()) == TD.getTypeSizeInBits(RHS->getType())
    //       && "BitWidth not match!");
    return createSAndInst(Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                         bool UsedAsArg) {
    // Handle the trivial case trivially.
    if (Ops.size() == 1) {
      if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
      return Ops[0];
    }

    SmallVector<Value *, 8> NotInsts;
    // Build the operands of Or operation into not inst.
    for (unsigned i = 0; i < Ops.size(); ++i) 
      NotInsts.push_back(createSNotInst(Ops[i], InsertPosition, true));

    // Build Or operation with the And Inverter Graph (AIG).
    Value *AndInst = createSAndInst(NotInsts, InsertPosition, true);
    return createSNotInst(AndInst, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                         bool UsedAsArg) {
    Value *Ops[] = {LHS, RHS};
    return createSOrInst(Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(ArrayRef<Value *> Ops, Value *InsertPosition,
                                          bool UsedAsArg) {
    assert (Ops.size() == 2 && "There should be more than one operand!!");

    // Build the Xor Expr with the And Inverter Graph (AIG).
    Value *OrInst = createSOrInst(Ops, InsertPosition, true);
    Value *AndInst = createSAndInst(Ops, InsertPosition, true);
    Value *NotInst = createSNotInst(AndInst, InsertPosition, true);

    Value *NewOps[] = {OrInst, NotInst};
    return createSAndInst(NewOps, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                          bool UsedAsArg) {
    Value *Ops[] = {LHS, RHS};
    return createSXorInst(Ops, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrEqualInst(Value *LHS, Value *RHS, Value *InsertPosition,
                                              bool UsedAsArg) {
    if (LHS == NULL) {
      if (!UsedAsArg) InsertPosition->replaceAllUsesWith(RHS);
      return (LHS = RHS);
    }

    return (LHS = createSOrInst(LHS, RHS, InsertPosition, UsedAsArg));
}

/// Functions to help us create Shang-Inst.
Value *SIRDatapathBuilder::getSignBit(Value *U, Value *InsertPosition) {
  unsigned BitWidth = getBitWidth(U);
  return createSBitExtractInst(U, BitWidth, BitWidth - 1, InsertPosition, true);
}

Value *SIRDatapathBuilder::createSConstantInt(uint16_t Value, unsigned BitWidth) {
  return SM->createIntegerValue(BitWidth, Value);
}