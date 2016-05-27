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
#include "vast/LuaI.h"

#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/Passes.h"
#include "sir/LangSteam.h"

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
using namespace vast;

static int NumSIRTempRegs = 0;

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
  INITIALIZE_PASS_DEPENDENCY(SIRAllocation)
INITIALIZE_PASS_END(SIRInit,
                    "shang-ir-init", "SIR Init",
                    false, true)

bool SIRInit::runOnFunction(Function &F) {
  /// Debug code
  std::string FinalIR = LuaI::GetString("FinalIR");
  std::string ErrorInFinalIR;
	raw_fd_ostream OutputForFinalIR(FinalIR.c_str(), ErrorInFinalIR);
	vlang_raw_ostream OutForFinalIR;
	OutForFinalIR.setStream(OutputForFinalIR);
	OutForFinalIR << F;

  DataLayout &TD = getAnalysis<DataLayout>();
  SIRAllocation &SA = getAnalysis<SIRAllocation>();

  SM = SA.getSIR();

  assert(SM->getFunction() == &F && "Function not matches!");

  // Initialize SIR from IR by transform llvm-inst to Shang-inst.
  SIRBuilder Builder(SM, TD, SA);

  // Build the general interface(Ports) of the module.
  Builder.buildInterface(&F);

  // Visit the basic block in topological order so we can avoid the instruction
  // is used before it is defined which will lead to bug when we create a Shang
  // Intrinsic to replace it.
  ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    Builder.visitBasicBlock(*(*I));

  return false;
}

void SIRInit::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DataLayout>();
  AU.addRequired<SIRAllocation>();
  AU.setPreservesAll();
}

void SIRBuilder::buildInterface(Function *F) {
  // Add basic ports by create Arguments for Function.
  C_Builder.createPort(SIRPort::Clk, "clk", 1);
  C_Builder.createPort(SIRPort::Rst, "rstN", 1);
  C_Builder.createPort(SIRPort::Start, "start", 1);
  C_Builder.createPort(SIRPort::Finish, "fin", 1);

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
  SIRSlot *IdleStartSlot = C_Builder.createSlot(0, 0);
  assert(IdleStartSlot && "We need to create a start slot here!");

  // Insert the implement just in front of the terminator instruction
  // at back of the module to avoid being used before declaration.
  Value *InsertPosition = SM->getPositionAtBackOfModule();

  // Get the Start signal of module.
  Value *Start
    = cast<SIRInPort>(SM->getPort(SIRPort::Start))->getValue();
  Value *IdleCnd = D_Builder.createSNotInst(Start, Start->getType(), InsertPosition, true);

  // Whole module will be run only when the Start signal is true.
  C_Builder.createStateTransition(IdleStartSlot, IdleStartSlot, IdleCnd);

  // Set the Finish signal of module.
  SIRRegister *FinReg = cast<SIROutPort>(SM->getFinPort())->getRegister();
  C_Builder.assignToReg(IdleStartSlot, Start, C_Builder.createIntegerValue(1, 0), FinReg);

  // Set the Argument register of module.
  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
       I != E; ++I) {
    Argument *Arg = I;
    StringRef Name = Arg->getName();
    unsigned BitWidth = TD.getTypeSizeInBits(Arg->getType());

    SIRRegister *ArgReg = C_Builder.createRegister(Name.str()+ "_reg", C_Builder.createIntegerType(BitWidth));
    Value *RegVal = ArgReg->getLLVMValue();

    // Then we need to do a type transform.
    if (Arg->getType()->isPointerTy())
      RegVal = D_Builder.createIntToPtrInst(ArgReg->getLLVMValue(), Arg->getType(), InsertPosition, true);
    else
      assert(Arg->getType()->isIntegerTy() && "Unexpected Argument Type!");

    Arg->replaceAllUsesWith(RegVal);

    Value *ArgVal = Arg;
    if (Arg->getType()->isPointerTy())
      ArgVal = D_Builder.createPtrToIntInst(Arg, ArgReg->getLLVMValue()->getType(), InsertPosition, true);

    C_Builder.assignToReg(IdleStartSlot, Start, ArgVal, ArgReg);

    // Index all argument registers.
    SM->indexArgReg(ArgReg);
  }

  // If the Start signal is true, then slot will jump to the slot of first BB.
  BasicBlock *EntryBB = &F->getEntryBlock();
  SIRSlot *EntryBBSlot = C_Builder.getOrCreateLandingSlot(EntryBB);
  C_Builder.createStateTransition(IdleStartSlot, EntryBBSlot, Start);
}

void SIRBuilder::visitBasicBlock(BasicBlock &BB) {
  // Create the landing slot for this BB.
  SIRSlot *S = C_Builder.getOrCreateLandingSlot(&BB);

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; ++I)
    visit(I);
}

//-----------------------------------------------------------------------//
// All Data-path instructions should be built by SIRDatapathBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitTruncInst(TruncInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitZExtInst(ZExtInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitSExtInst(SExtInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitSelectInst(SelectInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitICmpInst(ICmpInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitBinaryOperator(BinaryOperator &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitIntrinsicInst(IntrinsicInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitExtractValueInst(ExtractValueInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitExtractElementInst(ExtractElementInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitInsertElementInst(InsertElementInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitShuffleVectorInst(ShuffleVectorInst &I) {
  D_Builder.visit(I);
}

//-----------------------------------------------------------------------//
// All Control-path instructions should be built by SIRCtrlRgnBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitBranchInst(BranchInst &I) {
  C_Builder.visit(I);
}

void SIRBuilder::visitSwitchInst(SwitchInst &I) {
  C_Builder.visit(I);
}

void SIRBuilder::visitReturnInst(ReturnInst &I) {
  C_Builder.visit(I);
}

void SIRBuilder::visitStoreInst(StoreInst &I) {
  // Get the corresponding memory bank.
  SIRMemoryBank *Bank;
  Bank = SA.getMemoryBank(I);

  // Get the correct operand of the StoreInst.
  Value *PointerOperand = D_Builder.getAsOperand(I.getPointerOperand(), &I);
  Value *ValueOperand = D_Builder.getAsOperand(I.getValueOperand(), &I);

  C_Builder.createMemoryTransaction(PointerOperand, ValueOperand, Bank, I);
}

void SIRBuilder::visitLoadInst(LoadInst &I) {
  // Get the corresponding memory bank.
  SIRMemoryBank *Bank;
  Bank = SA.getMemoryBank(I);

  // Get the correct operand of the LoadInst.
  Value *PointerOperand = D_Builder.getAsOperand(I.getPointerOperand(), &I);

  C_Builder.createMemoryTransaction(PointerOperand, 0, Bank, I);
}


/// Functions to provide basic information
unsigned SIRCtrlRgnBuilder::getBitWidth(Value *U) {
  return TD.getTypeSizeInBits(U->getType());
}

Value *SIRCtrlRgnBuilder::createRegAssignInst(Type *RetTy, Value *InsertPosition) {
  // For all SeqValue which will be held in register, we create this pseudo SeqInst,
  // which will represent the assign operation to the register of this slot or port.
  // And the insert position will be the front of whole module.

  // Get the BitWidth of the RetTy, and we will create two pseudo value
  // to represent the operand of this pseudo instruction.
  unsigned BitWidth = TD.getTypeSizeInBits(RetTy);

  // Create pseudo SrcVal and GuardVal to assign to the register here. To be noted
  // that, the real SrcVal and GuardVal will replace these pseudo value after
  // register synthesis pass.
  Value *PseudoSrcVal = createIntegerValue(BitWidth, 1);
  Value *PseudoGuardVal = createIntegerValue(1, 1);
  Value *Ops[] = { PseudoSrcVal, PseudoGuardVal };

  Value *RegAssignInst = D_Builder.createShangInstPattern(Ops, RetTy, InsertPosition,
                                                          Intrinsic::shang_reg_assign,
                                                          true);
  return RegAssignInst;
}

SIRRegister *SIRCtrlRgnBuilder::createRegister(StringRef Name, Type *ValueTy,
                                               BasicBlock *ParentBB, uint64_t InitVal,
                                               SIRRegister::SIRRegisterTypes T) {
  // For the General Register or PHI Register, we create a pseudo instruction and
  // this pseudo instruction will be inserted into the back of the module.
  Value *InsertPosition = SM->getPositionAtBackOfModule();
  Value *SeqVal = createRegAssignInst(ValueTy, InsertPosition);

  assert(SeqVal && "Unexpected empty PseudoSeqInst!");
  assert(!SM->lookupSIRReg(SeqVal) && "Register already created before!");

  // Make sure we don't get empty name.
  std::string RegName;
  if (Name.size() == 0)
    RegName = "SIRTempReg" + utostr_32(NumSIRTempRegs++);
  else
    RegName = Name;

  // Create the register, and the BitWidth is determined by the ValueTy.
  unsigned BitWidth = TD.getTypeSizeInBits(ValueTy);
  SIRRegister *Reg = new SIRRegister(RegName, BitWidth, InitVal, ParentBB, T, SeqVal);

  // Index the register and index it with the SeqInst.
  SM->IndexRegister(Reg);
  SM->IndexSeqVal2Reg(SeqVal, Reg);

  return Reg;
}

SIRPort *SIRCtrlRgnBuilder::createPort(SIRPort::SIRPortTypes T, StringRef Name, 
                                       unsigned BitWidth) {
  // InPort or OutPort?
  if (T <= SIRPort::InPort) {
    LLVMContext &C = SM->getContext();
    SIRPort *P = new SIRInPort(T, BitWidth, Name, C);
    SM->IndexPort(P);
    return P;
  } else {
    // Record the Idx of FinPort and RetPort.
    if (T == SIRPort::RetPort)
      SM->setRetPortIdx(SM->getPortsSize());
    if (T == SIRPort::Finish)
      SM->setFinPortIdx(SM->getPortsSize());

    // Create the register for OutPort. To be noted that,
    // here we cannot assign the ParentBB for this register.
    uint64_t MaxVal = (uint64_t)std::pow(2.0, (double)BitWidth) - 1;
    if (T == SIRPort::Finish)
      MaxVal = 0;
    SIRRegister *Reg = createRegister(Name, createIntegerType(BitWidth),
                                      0, MaxVal, SIRRegister::OutPort);
    SIROutPort *P = new SIROutPort(T, Reg, BitWidth, Name);
    SM->IndexPort(P);
    return P;
  }  
}

void SIRCtrlRgnBuilder::createPortsForMemoryBank(SIRMemoryBank *SMB) {
  // Address pin
  Type *AddrTy = createIntegerType(SMB->getAddrWidth());
  SIRRegister *Addr = createRegister(SMB->getAddrName(), AddrTy, 0, 0, SIRRegister::FUInput);
  SMB->addFanin(Addr);

  // Write (to memory) data pin
  Type *WDataTy = createIntegerType(SMB->getDataWidth());
  SIRRegister *WData = createRegister(SMB->getWDataName(), WDataTy, 0, 0, SIRRegister::FUInput);
  SMB->addFanin(WData);

  // Read (from memory) data pin
  Type *RDataTy = createIntegerType(SMB->getDataWidth());
  SIRRegister *RData = createRegister(SMB->getRDataName(), WDataTy, 0, 0, SIRRegister::FUOutput);
  SMB->addFanout(RData);

  // Enable pin
  Type *EnableTy = createIntegerType(1);
  SIRRegister *Enable = createRegister(SMB->getEnableName(), EnableTy, 0,	0, SIRRegister::FUInput);
  SMB->addFanin(Enable);

  // Write enable pin
  Type *WriteEnTy = createIntegerType(1);
  SIRRegister *WriteEn = createRegister(SMB->getWriteEnName(), WriteEnTy, 0, 0, SIRRegister::FUInput);
  SMB->addFanin(WriteEn);

  // Byte enable pin
  if (SMB->requireByteEnable()) {
    Type *ByteEnTy = createIntegerType(SMB->getByteEnWidth());
    SIRRegister *ByteEn = createRegister(SMB->getByteEnName(), ByteEnTy, 0, 0, SIRRegister::FUInput);
    SMB->addFanin(ByteEn);
  }
}

SIRMemoryBank *SIRCtrlRgnBuilder::createMemoryBank(unsigned BusNum, unsigned AddrSize,
                                                   unsigned DataSize, bool RequireByteEnable,
                                                   bool IsReadOnly, unsigned ReadLatency) {
  SIRMemoryBank *SMB = new SIRMemoryBank(BusNum, AddrSize, DataSize,
                                         RequireByteEnable, IsReadOnly, ReadLatency);

  // Also create the ports for it.
  createPortsForMemoryBank(SMB);

  // Index the memory bank.
  SM->IndexSubModuleBase(SMB);

  return SMB;
}

void SIRCtrlRgnBuilder::createMemoryTransaction(Value *Addr, Value *Data,
                                                SIRMemoryBank *Bank, Instruction &I) {
  // Get ParentBB of this instruction.
  BasicBlock *ParentBB = I.getParent();
  // Get the slot.
  SIRSlot *Slot = SM->getLandingSlot(ParentBB);

  // Initial a vector to collect all SeqOps we created to implement this Load/Store
  // instruction, to be noted that, the collecting order matters!
  SmallVector<SIRSeqOp *, 4> MemSeqOps;

  /// Handle the enable pin.
  SIRSeqOp *AssignToEn = assignToReg(Slot, createIntegerValue(1, 1),
                                     createIntegerValue(1, 1), Bank->getEnable());
  MemSeqOps.push_back(AssignToEn);

  /// Handle the address pin.
  // Clamp the address width, to the address width of the memory bank.
  Type *RetTy = createIntegerType(Bank->getAddrWidth());
  // Mutate the type of the address to integer so that we can do match operation on it.
  assert(Addr->getType()->isPointerTy() && "Unexpected address type!");

  Value *AddrVal = D_Builder.createPtrToIntInst(Addr, createIntegerType(getBitWidth(Addr)),
                                                &I, true);
  AddrVal = D_Builder.createSBitExtractInst(AddrVal, Bank->getAddrWidth(),
                                            0, RetTy, &I, true);

  SIRSeqOp *AssignToAddr = assignToReg(Slot, createIntegerValue(1, 1),
                                       AddrVal, Bank->getAddr());
  MemSeqOps.push_back(AssignToAddr);

  /// Handle the byte enable pin.
  if (Bank->requireByteEnable()) {
    // Initial the ByteEn.
    PointerType *AddrTy = cast<PointerType>(Addr->getType());
    Type *DataTy = AddrTy->getElementType();
    unsigned DataSizeInBytes = TD.getTypeStoreSize(DataTy);
    unsigned ByteEnInitialValue = (0x1 << DataSizeInBytes) - 1;

    Value *ByteEnInit = createIntegerValue(Bank->getByteEnWidth(), ByteEnInitialValue);
    // Get the byte address part in address.
    unsigned ByteAddrPartWidth = Bank->getByteAddrWidth();
    Value *ByteAddr = D_Builder.createSBitExtractInst(AddrVal, ByteAddrPartWidth, 0,
                                                      createIntegerType(ByteAddrPartWidth),
                                                      &I, true);
    Value *ByteEn = D_Builder.createSShiftInst(ByteEnInit, ByteAddr, ByteEnInit->getType(),
                                               &I, Intrinsic::shang_shl, true);

    SIRSeqOp *AssignToByteEn = assignToReg(Slot, createIntegerValue(1, 1),
                                           ByteEn, Bank->getByteEn());
    MemSeqOps.push_back(AssignToByteEn);
  }

  /// Handle the data pin and write enable pin.
  // If Data != NULL, then this intruction is writing to memory.
  if (Data) {
    assert(getBitWidth(Data) <= Bank->getDataWidth() && "Unexpected data width!");

    if (Data->getType()->isVectorTy()) {
      Data = D_Builder.createBitCastInst(Data, createIntegerType(getBitWidth(Data)), &I, true);
    } else if (Data->getType()->isPointerTy()) {
      Data = D_Builder.createPtrToIntInst(Data, createIntegerType(getBitWidth(Data)), &I, true);
    }

    Type *RetTy = createIntegerType(Bank->getDataWidth());
    assert(Data->getType()->isIntegerTy() && "Unexpected data type!");
    // Extend the data width to match the memory bank.
    Value *DataVal = D_Builder.createSZExtInstOrSelf(Data, Bank->getDataWidth(), RetTy, &I, true);

    // If the memory bank requires ByteEnable, then we need to align the Data according to the
    // byte address part in address.
    if (Bank->requireByteEnable()) {
      // Get the byte address part in address.
      unsigned ByteAddrPartWidth = Bank->getByteAddrWidth();
      Value *ByteAddr = D_Builder.createSBitExtractInst(AddrVal, ByteAddrPartWidth, 0,
                                                        createIntegerType(ByteAddrPartWidth), &I, true);

      // Align the data by shift the data according the byte address value. To be note that the
      // byte address is in byte level, so we need to multiply it by 8. And the data in right
      // side has low byte address.
      Value *ShiftAmt = D_Builder.createSBitCatInst(ByteAddr, createIntegerValue(3, 0),
                                                    createIntegerType(ByteAddrPartWidth + 3), &I, true);
      DataVal = D_Builder.createSShiftInst(DataVal, ShiftAmt, DataVal->getType(), &I, Intrinsic::shang_shl, true);
    }

    // Handle the data pin.
    SIRSeqOp *AssignToWData = assignToReg(Slot, createIntegerValue(1, 1), DataVal, Bank->getWData());
    MemSeqOps.push_back(AssignToWData);

    // Handle the write enable pin.
    SIRSeqOp *AssignToWriteEn = assignToReg(Slot, createIntegerValue(1, 1), createIntegerValue(1, 1), Bank->getWriteEn());
    MemSeqOps.push_back(AssignToWriteEn);
  }
  // If Data == NULL, then this intruction is reading from memory.
  else {
    // According the read latency, advance to the slot
    // that we can get the RData.
    unsigned Latency = Bank->getReadLatency();
    Slot = advanceToNextSlot(Slot, Latency);

    // Load the RData into a register.
    Value *RData = Bank->getRData()->getLLVMValue();

    // Align the RData if the Bank requires ByteEn.
    if (getBitWidth(RData) != getBitWidth(&I)) {
      assert(getBitWidth(&I) < getBitWidth(RData) && "Unexpected Access BitWidth!");
      assert(Bank->requireByteEnable() && "The Memory Bank should require ByteEn!");

      // Get the byte address part in address.
      unsigned ByteAddrPartWidth = Bank->getByteAddrWidth();
      Value *ByteAddr = D_Builder.createSBitExtractInst(AddrVal, ByteAddrPartWidth, 0,
                                                        createIntegerType(ByteAddrPartWidth), &I, true);
      // Align the data by shift the data according the byte address value. To be note that the
      // byte address is in byte level, so we need to multiply it by 8. And the data in right
      // side has low byte address.
      Value *ShiftAmt = D_Builder.createSBitCatInst(ByteAddr, createIntegerValue(3, 0),
                                                    createIntegerType(ByteAddrPartWidth + 3), &I, true);
      RData = D_Builder.createSShiftInst(RData, ShiftAmt, RData->getType(), &I, Intrinsic::shang_lshr, true);
    }

    // Extract the wanted bits and the result will replace the use of this LoadInst.
    Value *Result = D_Builder.createSBitExtractInst(RData, getBitWidth(&I), 0,
                                                    createIntegerType(getBitWidth(&I)), &I, true);

    SIRRegister *ResultReg = createRegister(I.getName(), I.getType(), ParentBB);

    // The LoadInst is load the result into a register, that is a SeqVal in SIR.
    // So index the LoadInst to the SeqVal and replace all use of the LoadInst
    // with the SeqVal.
    SM->IndexVal2SeqVal(&I,	ResultReg->getLLVMValue());
                        I.replaceAllUsesWith(ResultReg->getLLVMValue());

    SIRSeqOp *AssignToResult = assignToReg(Slot, createIntegerValue(1, 1), Result, ResultReg);
    MemSeqOps.push_back(AssignToResult);
  }

  // Index the MemInst to MemSeqOps
  SM->IndexMemInst2SeqOps(&I, MemSeqOps);

//   // Advance to next slot so other operations will not conflicted with this memory
//   // transaction operation.
//   advanceToNextSlot(Slot);
}

SIRSlot *SIRCtrlRgnBuilder::createSlot(BasicBlock *ParentBB, unsigned Schedule) {
  // To be noted that, the SlotNum is decided by the creating order,
  // so it has no connection with the state transition order.
  unsigned SlotNum = SM->getSlotsSize();
  // Create a slot register for this slot.
  std::string Name = "Slot" + utostr_32(SlotNum) + "r";
  // If the slot is start slot, the InitVal should be 1.
  unsigned InitVal = !SlotNum ? 1: 0;
  SIRRegister *SlotGuardReg = createRegister(Name, createIntegerType(1), ParentBB,
                                             InitVal, SIRRegister::SlotReg);

  SIRSlot *S = new SIRSlot(SlotNum, ParentBB, SlotGuardReg->getLLVMValue(),
                           SlotGuardReg, Schedule);

  // Store the slot.
  SM->IndexSlot(S);
  SM->IndexReg2Slot(SlotGuardReg, S);

  return S;  
}

SIRSlot *SIRCtrlRgnBuilder::getOrCreateLandingSlot(BasicBlock *BB) {
  // Get the landing slot if it is already created.
  typedef std::pair<SIRSlot *, SIRSlot *> slot_pair;
  std::map<BasicBlock *, slot_pair> BB2SlotMap = SM->getBB2SlotMap();

  std::map<BasicBlock*, slot_pair>::const_iterator at = BB2SlotMap.find(BB);

  if (at == BB2SlotMap.end()) { 
    // Create the landing slot and latest slot for this BB.
    SIRSlot *S = createSlot(BB, 0);
    SM->IndexBB2Slots(BB, S, S);

    return S;
  }

  slot_pair &Slots = BB2SlotMap[BB];
  return Slots.first;
}

SIRSlot *SIRCtrlRgnBuilder::advanceToNextSlot(SIRSlot *CurSlot) {
  BasicBlock *BB = CurSlot->getParent();
  SIRSlot *Slot = SM->getLatestSlot(BB);

  if (CurSlot != Slot) {
    assert(CurSlot->succ_size() == 1 && "Unexpected multiple successors!");

    SIRSlot *SuccSlot = CurSlot->succ_begin()->getSlot();

    assert(SuccSlot->getParent() == CurSlot->getParent()
           && "Should locate in the same BB!");
    assert(SuccSlot->getStepInLocalBB() == CurSlot->getStepInLocalBB() + 1
           && "Bad schedule of the SuccSlot!");

    return SuccSlot;
  }

  assert(Slot == CurSlot && "CurSlot is not the last slot in BB!");

  // Create the next slot.
  unsigned Step = CurSlot->getStepInLocalBB() + 1;
  SIRSlot *NextSlot = createSlot(BB, Step);

  // Create the transition between two slots.
  createStateTransition(CurSlot, NextSlot, createIntegerValue(1, 1));

  // Index the new latest slot to BB.
  SM->IndexBB2Slots(BB, SM->getLandingSlot(BB), NextSlot);

  return NextSlot;
}

SIRSlot *SIRCtrlRgnBuilder::advanceToNextSlot(SIRSlot *CurSlot, unsigned NumSlots) {
  SIRSlot *S = CurSlot;

  for (unsigned i = 0; i < NumSlots; ++i)
    S = advanceToNextSlot(S);

  return S;
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
  for (iterator I = DstBB->begin(), E = DstBB->end(); I != E; ++I) {
    if (!isa<PHINode>(I))
      continue;

    PHINode *PN = cast<PHINode>(I);
    unsigned BitWidth = TD.getTypeSizeInBits(PN->getType());

    // The value which lives out from SrcBB to DstBB;
    Value *LiveOutedFromSrcBB = D_Builder.getAsOperand(PN->DoPHITranslation(DstBB, SrcBB), PN);

    // If the register already exist, then just assign to it.
    SIRRegister *PHISeqValReg;
    if (!SM->lookupSeqVal(PN)) {
      PHISeqValReg = createRegister(PN->getName(), PN->getType(), DstBB, 0, SIRRegister::PHI);

      std::string RegName = PHISeqValReg->getName();

      // Index this Inst to SeqInst.
      SM->IndexVal2SeqVal(PN, PHISeqValReg->getLLVMValue());
      // Replace the use of Inst to the Register of SeqInst.
      PN->replaceAllUsesWith(PHISeqValReg->getLLVMValue());
    } else {
      Value *PNSeqVal = SM->lookupSeqVal(PN);

      PHISeqValReg = SM->lookupSIRReg(PNSeqVal);
    }

    // Assign in SrcSlot so we can schedule to the latest slot of SrcBB.
    assignToReg(SrcSlot, Guard, LiveOutedFromSrcBB, PHISeqValReg);
  }
}

SIRSlotTransition *SIRCtrlRgnBuilder::createStateTransition(SIRSlot *SrcSlot, SIRSlot *DstSlot,
                                                            Value *Cnd) {
  assert(!SrcSlot->hasNextSlot(DstSlot) && "Edge already existed!");

  SrcSlot->addSuccSlot(DstSlot, SIRSlot::Sucessor, Cnd);
  SIRSeqOp *SeqOp = assignToReg(SrcSlot, Cnd, createIntegerValue(1, 1), DstSlot->getSlotReg());

  SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(SeqOp);
  assert(SST && "Should be a SIR Slot Transition!");

  return SST;
}

SIRSeqOp *SIRCtrlRgnBuilder::assignToReg(SIRSlot *S, Value *Guard, Value *Src,
                                         SIRRegister *Dst) {
  SIRSeqOp *SeqOp;

  if (Dst->isSlot())
    SeqOp = new SIRSlotTransition(Src, S, SM->lookupSIRSlot(Dst), Guard);
  else
    SeqOp = new SIRSeqOp(Src, Dst, Guard, S);

  // If SIRSlot is specified , then add this SeqOp to the lists in SIRSlot.
  // If not, then the SeqOp is executed every clock like pipeline.
  if (S)
    S->addSeqOp(SeqOp);

  SM->IndexSeqOp(SeqOp);

  return SeqOp;
}

/// Functions to visit all control-path instructions

void SIRCtrlRgnBuilder::visitBranchInst(BranchInst &I) {
  SIRSlot *CurSlot = SM->getLatestSlot(I.getParent());
  // Treat unconditional branch differently.
  if (I.isUnconditional()) {
    BasicBlock *DstBB = I.getSuccessor(0);
    createConditionalTransition(DstBB, CurSlot, createIntegerValue(1, 1));
    return;
  }

  // Connect the slots according to the condition.
  Value *Cnd = D_Builder.getAsOperand(I.getCondition(), &I);

  BasicBlock *TrueBB = I.getSuccessor(0);
  createConditionalTransition(TrueBB, CurSlot, Cnd);

  BasicBlock *FalseBB = I.getSuccessor(1);
  createConditionalTransition(FalseBB, CurSlot, D_Builder.createSNotInst(Cnd, Cnd->getType(), &I, true));
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

  // The comparison function for sorting the switch case values in the vector.
  struct CaseCmp {
    bool operator () (const CaseRange& C1, const CaseRange& C2) {
      return C1.Low.slt(C2.High);
    }
  };
}

// Clusterify - Transform simple list of Cases into list of CaseRange's
static unsigned Clusterify(CaseVector& Cases, SwitchInst *SI) {
  unsigned numCmps = 0;

  // Start with "simple" cases
  for (SwitchInst::CaseIt i = SI->case_begin(), e = SI->case_end(); i != e; ++i)
    Cases.push_back(CaseRange(i.getCaseValue()->getValue(),
                              i.getCaseValue()->getValue(),
                              i.getCaseSuccessor()));

  std::sort(Cases.begin(), Cases.end(), CaseCmp());

  // Merge case into clusters
  if (Cases.size() >= 2) {
    for (CaseItr I = Cases.begin(), J = llvm::next(Cases.begin()); J != Cases.end(); ) {
      int64_t nextValue = J->Low.getSExtValue();
      int64_t currentValue = I->High.getSExtValue();
      BasicBlock* nextBB = J->BB;
      BasicBlock* currentBB = I->BB;

      // If the two neighboring cases go to the same destination, merge them
      // into a single case.
      if ((nextValue-currentValue == 1) && (currentBB == nextBB)) {
        I->High = J->High;
        J = Cases.erase(J);
      } else {
        I = J++;
      }
    }
  }

  for (CaseItr I = Cases.begin(), E = Cases.end(); I != E; ++I, ++numCmps) {
    if (I->Low != I->High)
      // A range counts double, since it requires two compares.
      ++numCmps;
  }

  return numCmps;
}

void SIRCtrlRgnBuilder::visitSwitchInst(SwitchInst &I) {
  SIRSlot *CurSlot = SM->getLatestSlot(I.getParent());

  // The Condition Value
  Value *CndVal = D_Builder.getAsOperand(I.getCondition(), &I);
  // The Case Map
  std::map<BasicBlock *, Value *> CaseMap;

  // Prepare cases vector.
  CaseVector Cases;
  Clusterify(Cases, &I);

  // Build the condition map.
  for (CaseItr CI = Cases.begin(), CE = Cases.end(); CI != CE; ++CI) {
    const CaseRange &Case = *CI;
    // Simple case, test if the CndVal is equal to a specific value.
    if (Case.High == Case.Low) {
      Value *CaseVal = createIntegerValue(Case.High);
      Value *Pred = D_Builder.createSEQInst(CndVal, CaseVal, createIntegerType(1),
                                            &I, true);
      Value *&BBPred = CaseMap[Case.BB];
      if (!BBPred) BBPred = Pred;
      else D_Builder.createSOrEqualInst(BBPred, Pred, BBPred->getType(), &I, true);

      continue;
    }

    // Test if Low <= CndVal <= High.
    Value *Low = createIntegerValue(Case.Low);
    Value *LowCmp = D_Builder.createSIcmpOrEqInst(CmpInst::ICMP_UGT, CndVal, Low,
                                                  createIntegerType(1), &I, true);
    Value *High = createIntegerValue(Case.High);
    Value *HighCmp = D_Builder.createSIcmpOrEqInst(CmpInst::ICMP_UGT, High, CndVal,
                                                   createIntegerType(1), &I, true);
    Value *Pred = D_Builder.createSAndInst(LowCmp, HighCmp, LowCmp->getType(), &I, true);
    Value *&BBPred = CaseMap[Case.BB];
    if (!BBPred) BBPred = Pred;
    else D_Builder.createSOrEqualInst(BBPred, Pred, BBPred->getType(), &I, true);
  }

  // The predicate for each non-default destination.
  SmallVector<Value *, 4> CasePreds;
  typedef std::map<BasicBlock*, Value *>::iterator CaseIt;
  for (CaseIt CI = CaseMap.begin(), CE = CaseMap.end(); CI != CE; CI++) {
    BasicBlock *SuccBB = CI->first;
    Value *Pred = CI->second;
    CasePreds.push_back(Pred);

    createConditionalTransition(SuccBB, CurSlot, Pred);
  }

  // Jump to the default block when all the case value not match, i.e. all case
  // predicate is false.
  Value *DefaultPred
    = D_Builder.createSNotInst(D_Builder.createSOrInst(CasePreds, createIntegerType(1),
                                                       &I, true),
                               createIntegerType(1), &I, true);
  BasicBlock *DefBB = I.getDefaultDest();

  createConditionalTransition(DefBB, CurSlot, DefaultPred);
}

void SIRCtrlRgnBuilder::visitReturnInst(ReturnInst &I) {
  // Get the latest slot of CurBB.
  SIRSlot *CurSlot = SM->getLatestSlot(I.getParent());
  // Get the start slot of SIR.
  SIRSlot *StartSlot = SM->slot_begin();

  // Jump back to the start slot on return.
  createStateTransition(CurSlot, StartSlot, createIntegerValue(1, 1));

  if (I.getNumOperands()) {
    SIRRegister *Reg = cast<SIROutPort>(SM->getRetPort())->getRegister();

    // Launch the instruction to assignment value to register.
    // Here we should know that the Ret instruction different from others:
    // The module may have mutil-RetInst, but only one Ret Register.
    assignToReg(CurSlot, createIntegerValue(1, 1),
                D_Builder.getAsOperand(I.getReturnValue(), &I), Reg);

    // Replace the Ret operand with the RegVal. So all Ret-instruction
    // will return the RetRegVal in the corresponding slot. This will
    // make sure the correct Use Link in whole SIR.
    I.setOperand(0, Reg->getLLVMValue());
  }

  // Enable the finish port.
  SIRRegister *FinReg = cast<SIROutPort>(SM->getFinPort())->getRegister();
  assignToReg(CurSlot, createIntegerValue(1, 1), createIntegerValue(1, 1), FinReg);
}

IntegerType *SIRCtrlRgnBuilder::createIntegerType(unsigned BitWidth) {
  return D_Builder.createIntegerType(BitWidth);
}

Value *SIRCtrlRgnBuilder::createIntegerValue(unsigned BitWidth, unsigned Val) {
  return D_Builder.createIntegerValue(BitWidth, Val);
}

Value *SIRCtrlRgnBuilder::createIntegerValue(APInt Val) {
  return D_Builder.createIntegerValue(Val);
}


/// Functions to provide basic informations and elements.

unsigned SIRDatapathBuilder::getBitWidth(Value *V) {
  // Since we create a pseudo instruction for these slots and ports register,
  // so we should handle these pseudo instruction differently when we meet them.
  if(!V) {
    SIRRegister *Reg = SM->lookupSIRReg(dyn_cast<Instruction>(V));
    assert(Reg && (Reg->isOutPort() || Reg->isSlot()) && "Unexpected Null Value!");

    return Reg->getBitWidth();
  }

  Type *Ty = V->getType();
  unsigned BitWidth = TD.getTypeSizeInBits(Ty);
  return BitWidth;
}

unsigned SIRDatapathBuilder::getBitWidth(Type *Ty) {
  unsigned BitWidth = TD.getTypeSizeInBits(Ty);
  return BitWidth;
}

Value *SIRDatapathBuilder::getAsOperand(Value *Operand, Instruction *ParentInst) {
  if (GEPOperator *GEP = dyn_cast<GEPOperator>(Operand))
    return createSGEPInst(GEP, GEP->getType(), ParentInst, true);

  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Operand)) {
    switch (CE->getOpcode()) {
    case Instruction::GetElementPtr: {
      GEPOperator *GEP = dyn_cast<GEPOperator>(CE);
      assert(GEP && "Unexpected NULL GEP!");

      return createSGEPInst(GEP, GEP->getType(), ParentInst, true);
    }
    case Instruction::IntToPtr: {
      Value *IntOperand = getAsOperand(CE->getOperand(0), ParentInst);

      Value *ITPInst = createIntToPtrInst(IntOperand, CE->getType(),
                                          ParentInst, true);
      return ITPInst;
    }
    case Instruction::PtrToInt: {
      Value *PtrOperand = getAsOperand(CE->getOperand(0), ParentInst);

      return createPtrToIntInst(PtrOperand, CE->getType(), ParentInst, true);
    }
    case Instruction::BitCast: {
      Value *BCOperand = getAsOperand(CE->getOperand(0), ParentInst);

      Value *BCInst = createBitCastInst(BCOperand, CE->getType(),
                                        ParentInst, true);
      return BCInst;
    }
    case Instruction::ZExt: {
      Value *ZExtOperand = getAsOperand(CE->getOperand(0), ParentInst);

      return createSZExtInst(ZExtOperand, getBitWidth(CE), CE->getType(),
                             ParentInst, true);
    }
    case Instruction::And: {
      Value *LHS = getAsOperand(CE->getOperand(0), ParentInst),
        *RHS = getAsOperand(CE->getOperand(1), ParentInst);

      return createSAndInst(LHS, RHS, CE->getType(), ParentInst, true);
    }
    case Instruction::ICmp: {
      Value *LHS = getAsOperand(CE->getOperand(0), ParentInst),
        *RHS = getAsOperand(CE->getOperand(1), ParentInst);

      return createSICmpInst(CmpInst::Predicate(CE->getPredicate()), LHS,
                             RHS, CE->getType(), ParentInst, true);
    }
    default: llvm_unreachable("Unexpected Opcode!");
    }
  }

  assert(isa<Instruction>(Operand) || isa<Constant>(Operand) ||
         isa<UndefValue>(Operand) || isa<Argument>(Operand) ||
         isa<GlobalValue>(Operand) && "Unexpected Value!");

  return Operand;
}

/// Functions to visit all data-path instructions

void SIRDatapathBuilder::visitTruncInst(TruncInst &I) {
  createSTruncInst(getAsOperand(I.getOperand(0), &I), getBitWidth(&I),
                   0, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitZExtInst(ZExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSZExtInst(getAsOperand(I.getOperand(0), &I), NumBits, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitSExtInst(SExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSSExtInst(getAsOperand(I.getOperand(0), &I), NumBits, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitSelectInst(SelectInst &I) {
  // Get the correct operands.
  Value *Cnd    = getAsOperand(I.getOperand(0), &I);
  Value *TrueV  = getAsOperand(I.getOperand(1), &I);
  Value *FalseV = getAsOperand(I.getOperand(2), &I);

  createSSelInst(Cnd, TrueV, FalseV, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitICmpInst(ICmpInst &I) {
  // Get the correct operands.
  Value *LHS  = getAsOperand(I.getOperand(0), &I);
  Value *RHS  = getAsOperand(I.getOperand(1), &I);

  Value *Ops[] = { LHS, RHS };
  createSICmpInst(I.getPredicate(), Ops, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitBinaryOperator(BinaryOperator &I) {
  // Get the correct operands.
  Value *LHS  = getAsOperand(I.getOperand(0), &I);
  Value *RHS  = getAsOperand(I.getOperand(1), &I);
  Value *Ops[] = { LHS, RHS };

  Type *RetTy = I.getType();

  switch (I.getOpcode()) {
  case Instruction::Add: createSAddInst(Ops, RetTy, &I, false); return;
  case Instruction::Sub: createSSubInst(Ops, RetTy, &I, false); return;
  case Instruction::Mul: createSMulInst(Ops, RetTy, &I, false); return;

  case Instruction::Shl: createSShiftInst(Ops, RetTy, &I, Intrinsic::shang_shl, false); return;
  case Instruction::AShr: createSShiftInst(Ops, RetTy, &I, Intrinsic::shang_ashr, false); return;
  case Instruction::LShr: createSShiftInst(Ops, RetTy, &I, Intrinsic::shang_lshr, false); return;

  case Instruction::UDiv: createSUDivInst(Ops, RetTy, &I, false); return;
  case Instruction::SDiv: createSSDivInst(Ops, RetTy, &I, false); return;

  case Instruction::SRem: createSSRemInst(Ops, RetTy, &I, false); return;

  case Instruction::And: createSAndInst(Ops, RetTy, &I, false); return;
  case Instruction::Or:  createSOrInst(Ops, RetTy, &I, false); return;
  case Instruction::Xor: createSXorInst(Ops, RetTy, &I, false); return;

  default: llvm_unreachable("Unexpected opcode"); break;
  }

  return;
}

void SIRDatapathBuilder::visitIntrinsicInst(IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  default: break;

  case Intrinsic::uadd_with_overflow: {
    // Get the correct operands.
    Value *LHS = getAsOperand(I.getOperand(0), &I);
    Value *RHS = getAsOperand(I.getOperand(1), &I);

    assert(getBitWidth(LHS) == getBitWidth(RHS) && "BitWidth not matches!");

    // The result of uadd_with_overflow is 1 bit bigger than the operand size.
    unsigned ResultBitWidth = getBitWidth(LHS) + 1;
    // Mutate the RetTy to IntegerType with correct BitWidth since
    // the original RetTy cannot be recognized by SIR framework.
    I.mutateType(createIntegerType(ResultBitWidth));
    createSAddInst(LHS, RHS, I.getType(), &I, false);

    return;
  }
  case Intrinsic::bswap: {
    unsigned BitWidth = getBitWidth(&I);
    Value *V = getAsOperand(I.getOperand(0), &I);

    assert(BitWidth % 16 == 0 && "Unexpected BitWidth!");
    unsigned NumBytes = BitWidth / 8;

    SmallVector<Value *, 8> Bytes;
    for (unsigned i = 0; i != NumBytes; ++i)
      Bytes.push_back(createSBitExtractInst(V, i * 8 + 8, i * 8,
                                            createIntegerType(8), &I, true));

    createSBitCatInst(Bytes, I.getType(), &I, false);

    return;
  }
  // Ignore the llvm.mem Intrinsic since we have lower it in SIRLowerIntrinsicPass
  case Intrinsic::memcpy:
  case Intrinsic::memset:
  case Intrinsic::memmove:
  // Ignore the Shang Intrinsic we created.
  case Intrinsic::shang_reg_assign:
  case Intrinsic::shang_not:
  case Intrinsic::shang_rand:
  case Intrinsic::shang_rxor:
  case Intrinsic::shang_and:
  case Intrinsic::shang_or:
  case Intrinsic::shang_xor:
  case Intrinsic::shang_add:
  case Intrinsic::shang_addc:
  case Intrinsic::shang_mul:
  case Intrinsic::shang_sdiv:
  case Intrinsic::shang_udiv:
  case Intrinsic::shang_sgt:
  case Intrinsic::shang_ugt:
  case Intrinsic::shang_bit_cat:
  case Intrinsic::shang_bit_extract:
  case Intrinsic::shang_bit_repeat:
  case Intrinsic::shang_shl:
  case Intrinsic::shang_ashr:
  case Intrinsic::shang_lshr:
    return;
  }

  llvm_unreachable("Unexpected opcode");
}

void SIRDatapathBuilder::visitExtractValueInst(ExtractValueInst &I) {
  Value *Operand = getAsOperand(I.getAggregateOperand(), &I);
  unsigned BitWidth = getBitWidth(Operand);

  assert(I.getNumIndices() == 1 && "Unexpected number of indices!");

  // Return the overflow bit.
  if (I.getIndices()[0] == 1) {
    createSBitExtractInst(Operand, BitWidth, BitWidth - 1,
                          createIntegerType(1), &I, false);
    return;
  }

  // Else return the addition result.
  assert(I.getIndices()[0] == 0 && "Bad index!");
  createSBitExtractInst(Operand, BitWidth - 1, 0,
                        createIntegerType(BitWidth - 1), &I, false);
}

void SIRDatapathBuilder::visitExtractElementInst(ExtractElementInst &I) {
  // Get the Vector and the Index.
  Value *VectorOp = getAsOperand(I.getVectorOperand(), &I);
  Value *IndexOp = getAsOperand(I.getIndexOperand(), &I);

  // Get some basic information.
  unsigned ElemNum = I.getVectorOperandType()->getNumElements();
  unsigned VectorBitWidth = getBitWidth(I.getVectorOperandType());
  unsigned ElemBitWidth = getBitWidth(I.getVectorOperandType()->getElementType());

  Type *Int8Ty = createIntegerType(8);

  // After we cast the vector into integer type, the element we want to extract will
  // be located in a integer value like { HigherPart...Element...LowerPart }.
  // To be noted that, the Start/End point of this extraction is not unsigned int but
  // value type, so we can not use the createSBitExtractInst function directly. Instead,
  // we implement the extraction using createSShiftInst function.
  Value *LShrBitWidth = createSMulInst(createIntegerValue(8, ElemBitWidth),
                                       IndexOp, Int8Ty, &I, true);

  // After the cast, we will get a value like { HigherPart...Element...LowerPart }
  Value *BitCastResult = createBitCastInst(VectorOp, createIntegerType(VectorBitWidth), &I, true);

  // After the Right Shift, we will get a value like { 0...0...HigherPart...Element }
  Value *LShrResult = createSShiftInst(BitCastResult, LShrBitWidth,
                                       BitCastResult->getType(),
                                       &I, Intrinsic::shang_lshr, true);

  Value *Result = createSBitExtractInst(LShrResult, ElemBitWidth, 0,
                                        createIntegerType(ElemBitWidth), &I, false);
}

void SIRDatapathBuilder::visitInsertElementInst(InsertElementInst &I) {
  // Get the Vector and the Index.
  Value *VectorOp = getAsOperand(I.getOperand(0), &I);
  Value *ElemOp = getAsOperand(I.getOperand(1), &I);
  Value *IndexOp = getAsOperand(I.getOperand(2), &I);

  VectorType *VT = dyn_cast<VectorType>(VectorOp->getType());
  assert(VT && "Unexpected Type!");

  // Get some basic information.
  unsigned ElemNum = VT->getNumElements();
  unsigned VectorBitWidth = getBitWidth(VT);
  unsigned ElemBitWidth = getBitWidth(VT->getElementType());

  Type *Int32Ty = createIntegerType(32);

  // After we cast the vector into integer type, the element we want to insert will replace
  // the OldElement located inside of a value like { HigherPart...OldElement...LowerPart }.
  Value *LShrBitWidth = createSMulInst(createIntegerValue(32, ElemBitWidth),
                                       createSAddInst(IndexOp, createIntegerValue(1, 1),
                                                      IndexOp->getType(), &I, true),
                                       Int32Ty, &I, true);
  Value *ShlBitWidth = createSMulInst(createIntegerValue(8, ElemBitWidth),
                                      createSSubInst(createIntegerValue(32, ElemNum),
                                                     IndexOp, Int32Ty, &I, true),
                                      Int32Ty, &I, true);

  // After the cast, we will get a value like
  // { HigherPart...OldElement...LowerPart }
  Value *BitCastResult
    = createBitCastInst(VectorOp, createIntegerType(VectorBitWidth), &I, true);

  // After the Right Shift, we will get a value like
  // { 0...0...HigherPart }
  Value *LShrResult
    = createSShiftInst(BitCastResult, LShrBitWidth,
                       BitCastResult->getType(),
                       &I, Intrinsic::shang_lshr, true);

  // After the Left Shift, we will get a value like
  // { LowerPart...0...0 }
  Value *ShlResult = createSShiftInst(BitCastResult, ShlBitWidth,
                                      BitCastResult->getType(),
                                      &I, Intrinsic::shang_shl, true);

  // After the BitCat, we will get a value like
  // { 0...0...HigherPart...Elem...LowerPart...0...0 }
  Value *BitCatOps[] = { LShrResult, ElemOp, ShlResult };
  Value *BitCatResult
    = createSBitCatInst(BitCatOps, createIntegerType(ElemBitWidth + VectorBitWidth * 2),
                        &I, true);

  // After the second Right Shift, we will get a value like
  // { 0...0...0...0...HigherPart...Elem...LowerPart }
  Value *LShrAgainResult = createSShiftInst(BitCatResult, ShlBitWidth,
                                            BitCatResult->getType(),
                                            &I, Intrinsic::shang_lshr, true);

  Value *BitExtractResult = createSBitExtractInst(LShrAgainResult, VectorBitWidth, 0,
                                                  createIntegerType(VectorBitWidth),
                                                  &I, true);

  Value *Result = createBitCastInst(BitExtractResult, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitShuffleVectorInst(ShuffleVectorInst &I) {

}

void SIRDatapathBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  GEPOperator *GEP = dyn_cast<GEPOperator>(&I);
  assert(GEP && "Unexpected NULL GEP!");

  createSGEPInst(GEP, I.getType(), &I, false);
}

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

  // If the InsertPosition is a Instruction, then we insert the new-created instruction
  // before the InsertPosition-Instruction.
  if (Instruction *InsertBefore = dyn_cast<Instruction>(InsertPosition)) {
    // The name of the instruction we created.
    std::string S = UsedAsArg ? Func->getName() : InsertPosition->getName();

    // Create the instruction.
    Instruction *NewInst = CallInst::Create(Func, Ops, S, InsertBefore);

    // Index all these data-path instructions.
    if (FuncID != Intrinsic::shang_reg_assign)
      SM->IndexDataPathInst(NewInst);

    // If the inst is not used as an argument of other functions,
    // then it is used to replace the inst in IR
    if (!UsedAsArg) {
      Value *result = NewInst;

      // Before we call the replaceAllUsesWith function, we should make sure the
      // type matches. To be noted that, we only permit PointerTy & IntegerTy.
      if (InsertBefore->getType() != NewInst->getType()) {
        Type *OriginType = InsertBefore->getType();
        assert(OriginType->isPointerTy() && "Unexpected Type!");

        result = createIntToPtrInst(result, OriginType, InsertBefore, false);
      }

      InsertBefore->replaceAllUsesWith(result);
    }

    // Only return the IntegerTy since all the following SIR instructions only support
    // IntegerTy as operands.
    return NewInst;
  }
  // If the InsertPosition is a BasicBlock, then we insert the new-created instruction
  // at the back of the InsertPosition-BasicBlock but before the TerminatorInst so that
  // we will not mess up with the getTerminator() function of BasicBlock.
  else if (BasicBlock *InsertAtEnd = dyn_cast<BasicBlock>(InsertPosition)) {
    Instruction *Terminator = InsertAtEnd->getTerminator();
    Instruction *NewInst = CallInst::Create(Func, Ops, Func->getName(), Terminator);

    // Index all these data-path instructions.
    if (FuncID != Intrinsic::shang_reg_assign)
      SM->IndexDataPathInst(NewInst);

    return NewInst;
  } 

  assert(false && "Unexpected InsertPosition!");
}

Value *SIRDatapathBuilder::createSNegativeInst(Value *U, bool isPositiveValue,
                                               bool isNegativeValue, Type *RetTy,
                                               Value *InsertPosition, bool UsedAsArg) {
  assert(U->getType() == RetTy && "Unexpected RetTy!");

  // Prepare some useful elements.
  unsigned BitWidth = getBitWidth(U);
  Value *temp = createSBitExtractInst(U, BitWidth - 1, 0,
                                      createIntegerType(BitWidth - 1),
                                      InsertPosition, true);

  // If it is a Positive Value, just change the sign bit to 1'b1;
  if (isPositiveValue) {
    assert(!isNegativeValue && "This should be a positive value!");

    return createSBitCatInst(createIntegerValue(1, 1), temp, RetTy,
                             InsertPosition, UsedAsArg);
  }

  // If it is a Negative Value, just change the sign bit to 1'b0;
  if (isNegativeValue) {
    assert(!isPositiveValue && "This should be a negative value!");

    return createSBitCatInst(createIntegerValue(1, 0), temp, RetTy,
                             InsertPosition, UsedAsArg);
  }

  assert(!isPositiveValue && !isNegativeValue
         && "These two circumstance should be handled before!");

  // If we do not know the detail information, we should build the
  // logic to test it is Positive or Negative.
  Value *NewSignBit = createSNotInst(getSignBit(U, InsertPosition),
                                     createIntegerType(1), InsertPosition, true);

  return createSBitCatInst(NewSignBit, temp, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOriginToComplementInst(Value *U, Type *RetTy,
                                                         Value *InsertPosition,
                                                         bool UsedAsArg) {
  assert(U->getType() == RetTy && "Unexpected RetTy!");

  unsigned BitWidth = getBitWidth(U);

  Value *isNegative = createSBitExtractInst(U, BitWidth, BitWidth - 1,
                                            createIntegerType(1),
                                            InsertPosition, true);
  Value *isPositive = createSNotInst(isNegative, isNegative->getType(),
                                     InsertPosition, true);

  /// If the value is positive, then the complement will be same as origin.
  Value *resultWhenPositive = U;
  Value *PositiveCondition = createSBitRepeatInst(isPositive, BitWidth,
                                                  resultWhenPositive->getType(),
                                                  InsertPosition, true);
  Value *temp1 = createSAndInst(resultWhenPositive, PositiveCondition,
                                resultWhenPositive->getType(), InsertPosition, true);

  /// If the value is negative, then compute the complement.
  // Extract the other bits and revert them.
  Value *OtherBits = createSBitExtractInst(U, BitWidth - 1, 0,
                                           createIntegerType(BitWidth - 1),
                                           InsertPosition, true);
  Value *RevertBits = createSNotInst(OtherBits, OtherBits->getType(),
                                     InsertPosition, true);

  // Catenate the Sign bit and Revert bits, then plus 1.
  Value *CatenateResult = createSBitCatInst(createIntegerValue(1, 1), RevertBits,
                                            RetTy, InsertPosition, true);
  Value *AddResult = createSAddInst(CatenateResult, createIntegerValue(1, 1),
                                    RetTy, InsertPosition, UsedAsArg);

  Value *resultWhenNegative = AddResult;
  Value *NegativeCondition = createSBitRepeatInst(isNegative, BitWidth,
                                                  resultWhenNegative->getType(),
                                                  InsertPosition, true);
  Value *temp2 = createSAndInst(resultWhenNegative, NegativeCondition,
                                resultWhenNegative->getType(),
                                InsertPosition, true);

  Value *Temps[] = { temp1, temp2 };
  return createSOrInst(Temps, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSComplementToOriginInst(Value *U, Type *RetTy,
                                                         Value *InsertPosition,
                                                         bool UsedAsArg) {
  assert(U->getType() == RetTy && "Unexpected RetTy!");

  unsigned BitWidth = getBitWidth(U);

  Value *isNegative = createSBitExtractInst(U, BitWidth, BitWidth - 1,
                                            createIntegerType(1), InsertPosition, true);
  Value *isPositive = createSNotInst(isNegative, isNegative->getType(),
                                     InsertPosition, true);

  /// If the value is positive, then the origin will be same as complement.
  Value *resultWhenPositive = U;
  Value *PositiveCondition = createSBitRepeatInst(isPositive, BitWidth,
                                                  resultWhenPositive->getType(),
                                                  InsertPosition, true);
  Value *temp1 = createSAndInst(resultWhenPositive, PositiveCondition,
                                resultWhenPositive->getType(), InsertPosition, true);

  /// If the value is negative, then compute the complement.
  // Extract the other bits and minus 1.
  Value *OtherBits = createSBitExtractInst(U, BitWidth - 1, 0,
                                           createIntegerType(BitWidth - 1),
                                           InsertPosition, true);
  Value *MinusResult = createSSubInst(OtherBits, createIntegerValue(BitWidth - 1, 1),
                                      OtherBits->getType(), InsertPosition, true);

  // Revert the minus result and Catenate the Sign bit.
  Value *RevertBits = createSNotInst(MinusResult, MinusResult->getType(),
                                     InsertPosition, true);
  Value *CatenateResult = createSBitCatInst(createIntegerValue(1, 1), RevertBits,
                                            RetTy, InsertPosition, UsedAsArg);

  Value *resultWhenNegative = CatenateResult;
  Value *NegativeCondition = createSBitRepeatInst(isNegative, BitWidth,
                                                  resultWhenNegative->getType(),
                                                  InsertPosition, true);
  Value *temp2 = createSAndInst(resultWhenNegative, NegativeCondition,
                                resultWhenNegative->getType(),
                                InsertPosition, true);

  Value *Temps[] = { temp1, temp2 };
  return createSOrInst(Temps, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitCatInst(ArrayRef<Value *> Ops, Type *RetTy,
                                             Value *InsertPosition, bool UsedAsArg) {
  // If there are more than two operands, transform it into mutil-SBitCatInst.
  if (Ops.size() > 2) {
    unsigned TempSBitCatBitWidth = getBitWidth(Ops[0]) + getBitWidth(Ops[1]);
    Value *TempSBitCatInst = createSBitCatInst(Ops[0], Ops[1],
                                               createIntegerType(TempSBitCatBitWidth),
                                               InsertPosition, true);
    for (unsigned i = 0; i < Ops.size() - 3; i++) {
      TempSBitCatBitWidth = TempSBitCatBitWidth + getBitWidth(Ops[i + 2]);
      TempSBitCatInst = createSBitCatInst(TempSBitCatInst, Ops[i + 2],
                                          createIntegerType(TempSBitCatBitWidth),
                                          InsertPosition, true);
    }

    int num = Ops.size();
    TempSBitCatBitWidth = TempSBitCatBitWidth + getBitWidth(Ops[num - 1]);
    return createSBitCatInst(TempSBitCatInst, Ops[num - 1],
                             createIntegerType(TempSBitCatBitWidth),
                             InsertPosition, UsedAsArg);
  }

  assert(getBitWidth(Ops[0]) + getBitWidth(Ops[1]) == getBitWidth(RetTy)
         && "BitWidth not matches!");

  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_bit_cat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitCatInst(Value *LHS, Value *RHS, Type *RetTy,
                                             Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSBitCatInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitExtractInst(Value *U, unsigned UB,
                                                 unsigned LB, Type *RetTy,
                                                 Value *InsertPosition,
                                                 bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == UB - LB && "RetTy not matches!");
  assert(UB <= getBitWidth(U) && LB >=0 && "Unexpected Extract BitWidth!");

  // Trivial case: no real extract move here.
  if (UB == getBitWidth(U) && LB == 0) {
    if (!UsedAsArg)
      InsertPosition->replaceAllUsesWith(U);

    return U;
  }

  if (isa<ConstantInt>(U) && LB == 0) {
    if (UB == getBitWidth(U))
      return U;

    int64_t ValOfU = getConstantIntValue(U);
    int64_t MaxVal = std::pow(2.0, (double)UB) - 1;
    if (ValOfU <= MaxVal)
      return createIntegerValue(UB, ValOfU);
  }

  Value *Ops[] = {U, createIntegerValue(16, UB), createIntegerValue(16, LB)};
  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_bit_extract, UsedAsArg);
}

Value *SIRDatapathBuilder::createSTruncInst(Value *U, unsigned UB,
                                            unsigned LB, Type *RetTy,
                                            Value *InsertPosition,
                                            bool UsedAsArg) {
  // Truncate the value by bit-extract expression.
  return createSBitExtractInst(U, UB, LB, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInst(Value *U, unsigned DstBitWidth,
                                           Type *RetTy, Value *InsertPosition,
                                           bool UsedAsArg) {
  unsigned SrcBitWidth = getBitWidth(U);
  assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
  unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
  Value *ExtendZeroBits = createIntegerValue(NumExtendBits, 0);

  Value *Ops[] = { ExtendZeroBits, U };
  return createSBitCatInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInstOrSelf(Value *U, unsigned DstBitWidth,
                                                 Type *RetTy, Value *InsertPosition,
                                                 bool UsedAsArg) {
  if (getBitWidth(U) < DstBitWidth)
    return createSZExtInst(U, DstBitWidth, RetTy, InsertPosition, UsedAsArg);
  else if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);

  return U;
}

Value *SIRDatapathBuilder::createSSExtInst(Value *U, unsigned DstBitWidth,
                                           Type *RetTy, Value *InsertPosition,
                                           bool UsedAsArg) {
  unsigned SrcBitWidth = getBitWidth(U);
  assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
  unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
  Value *SignBit = getSignBit(U, InsertPosition);

  Value *ExtendBits = createSBitRepeatInst(SignBit, NumExtendBits,
                                           createIntegerType(NumExtendBits),
                                           InsertPosition, true);
  Value *Ops[] = { ExtendBits, U };
  return createSBitCatInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSExtInstOrSelf(Value *U, unsigned DstBitWidth,
                                                 Type *RetTy, Value *InsertPosition,
                                                 bool UsedAsArg) {
  if (getBitWidth(U) < DstBitWidth)
    return createSSExtInst(U, DstBitWidth, RetTy, InsertPosition, UsedAsArg);
  else if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);

  return U;
}

Value *SIRDatapathBuilder::createSRAndInst(Value *U, Type *RetTy,
                                           Value *InsertPosition, bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(U, RetTy, InsertPosition,
                                Intrinsic::shang_rand, UsedAsArg);
}

Value *SIRDatapathBuilder::createSRXorInst(Value *U, Type *RetTy,
                                           Value *InsertPosition, bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(U, RetTy, InsertPosition,
                                Intrinsic::shang_rxor, UsedAsArg);
}

Value *SIRDatapathBuilder::createSROrInst(Value *U, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  // A | B .. | Z = ~(~A & ~B ... & ~Z).
  Value *NotU = createSNotInst(U, U->getType(), InsertPosition, true);
  return createSNotInst(createSRAndInst(NotU, RetTy, InsertPosition, true), RetTy,
                        InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSNEInst(ArrayRef<Value *> Ops, Type *RetTy,
                                         Value *InsertPosition, bool UsedAsArg) {
  assert(getBitWidth(Ops[0]) == getBitWidth(Ops[1]) == TD.getTypeSizeInBits(RetTy)
         && "RetTy not matches!");

  // Get the bitwise difference by Xor.
  Value *BitWissDiff = createSXorInst(Ops, Ops[0]->getType(), InsertPosition, true);
  // If there is any bitwise difference, then LHS and RHS is not equal.
  return createSROrInst(BitWissDiff, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(ArrayRef<Value *> Ops, Type *RetTy,
                                         Value *InsertPosition, bool UsedAsArg) {
  return createSNotInst(createSNEInst(Ops, RetTy, InsertPosition, true),
                        RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(Value *LHS, Value *RHS, Type *RetTy,
                                         Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSEQInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSdpSGTInst(ArrayRef<Value *> Ops, Type *RetTy,
                                            Value *InsertPosition, bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_sgt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSdpUGTInst(ArrayRef<Value *> Ops, Type *RetTy,
                                            Value *InsertPosition, bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_ugt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSdpUGTInst(Value *LHS, Value *RHS, Type *RetTy,
                                            Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSdpUGTInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitRepeatInst(Value *U, unsigned RepeatTimes, Type *RetTy,
                                                Value *InsertPosition, bool UsedAsArg) {
  if (RepeatTimes == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);
    return U;
  }

  assert(TD.getTypeSizeInBits(RetTy) == getBitWidth(U) * RepeatTimes
         && "RetTy not matches!");

  Value *Ops[] = { U, createIntegerValue(32, RepeatTimes)};
  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_bit_repeat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSelInst(Value *Cnd, Value *TrueV, Value *FalseV,
                                          Type *RetTy, Value *InsertPosition,
                                           bool UsedAsArg) {
  assert(getBitWidth(Cnd) == 1 && "Bad condition width!");
  assert(TrueV->getType() == FalseV->getType() &&
         TrueV->getType() == RetTy && "Unexpected Type!");

  unsigned Bitwidth = getBitWidth(TrueV);
  Type *IntTy = createIntegerType(Bitwidth);

  if (ConstantInt *C = dyn_cast<ConstantInt>(Cnd)) {
    Value *Result = C->getValue().getBoolValue() ? TrueV : FalseV; 
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Result);
    return Result;
  }

  // If the operand is PointerTy, we should transform it into IntegerTy
  // so we can do logic operations on it.
  if (RetTy->isPointerTy()) {
    Instruction *InsertBefore = dyn_cast<Instruction>(InsertPosition);

    TrueV = createPtrToIntInst(TrueV, IntTy, InsertBefore, true);
    FalseV = createPtrToIntInst(FalseV, IntTy, InsertBefore, true);
  }

  Value *NewCnd = createSBitRepeatInst(Cnd, Bitwidth, IntTy, InsertPosition, true);
  Value *NotNewCnd = createSNotInst(NewCnd, IntTy, InsertPosition, true);

  Value *ResultWhenCnd = createSAndInst(NewCnd, TrueV, IntTy, InsertPosition, true);
  Value *ResultWhenNotCnd= createSAndInst(NotNewCnd, FalseV, IntTy, InsertPosition, true);

  return createSOrInst(ResultWhenCnd, ResultWhenNotCnd, IntTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate,
                                               ArrayRef<Value *> Ops,
                                               Type *RetTy, Value *InsertPosition,
                                               bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  Value *ICmpResult = createSICmpInst(Predicate, Ops, RetTy, InsertPosition, true);
  Value *EQResult = createSEQInst(Ops, RetTy, InsertPosition, true);
  Value *NewOps[] = {EQResult, ICmpResult};
  return createSOrInst(NewOps, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate,
                                               Value *LHS, Value *RHS,
                                               Type *RetTy, Value *InsertPosition,
                                               bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSIcmpOrEqInst(Predicate, Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSICmpInst(ICmpInst::Predicate Predicate,
                                           ArrayRef<Value *> Ops,
                                           Type *RetTy, Value *InsertPosition,
                                           bool UsedAsArg) {
  assert(Ops.size() == 2 && "There must be two operands!");
  assert(getBitWidth(Ops[0]) == getBitWidth(Ops[1]) && "Bad ICMP BitWidth!");
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  Value *LHS = Ops[0], *RHS = Ops[1];

  Type *IntTy = createIntegerType(getBitWidth(LHS));

  // Transform into IntegerTy so it can be taken as operand in Shang Intrinsic instruction.
  if (LHS->getType()->isPointerTy())
    LHS = createPtrToIntInst(LHS, IntTy, dyn_cast<Instruction>(InsertPosition), true);
  if (RHS->getType()->isPointerTy())
    RHS = createPtrToIntInst(RHS, IntTy, dyn_cast<Instruction>(InsertPosition), true);

  Value *NewOps[] = { LHS, RHS };

  switch (Predicate) {
  case CmpInst::ICMP_NE:
    return createSNEInst(NewOps, RetTy, InsertPosition, UsedAsArg);
  case CmpInst::ICMP_EQ:
    return createSEQInst(NewOps, RetTy, InsertPosition, UsedAsArg);

  case CmpInst::ICMP_SLT:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_SGT:
    return createSdpSGTInst(NewOps, RetTy, InsertPosition, UsedAsArg);

  case CmpInst::ICMP_ULT:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_UGT:
    return createSdpUGTInst(NewOps, RetTy, InsertPosition, UsedAsArg);

  case CmpInst::ICMP_SLE:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_SGE:
    return createSIcmpOrEqInst(CmpInst::ICMP_SGT, NewOps, RetTy,
                               InsertPosition, UsedAsArg);

  case CmpInst::ICMP_ULE:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_UGE:
    return createSIcmpOrEqInst(CmpInst::ICMP_UGT, NewOps, RetTy,
                               InsertPosition, UsedAsArg);

  default: break;
  }

  llvm_unreachable("Unexpected ICmp predicate!");
  return 0;
}

Value *SIRDatapathBuilder::createSICmpInst(ICmpInst::Predicate Predicate,
                                           Value *LHS, Value *RHS,
                                           Type *RetTy, Value *InsertPosition,
                                           bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSICmpInst(Predicate, Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSNotInst(Value *U, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  // If the instruction is like: A = ~(~B), then we can simplify it.
  IntrinsicInst *Inst = dyn_cast<IntrinsicInst>(U);
  if(Inst && Inst->getIntrinsicID() == Intrinsic::shang_not) {
    Value *Operand = Inst->getOperand(0);

    // If the inst is not used as an argument of other functions,
    // then it is used to replace the inst in IR
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Operand);
    return Operand;
  }

  assert(U->getType() == RetTy && "RetTy not matches!");

  return createShangInstPattern(U, RetTy, InsertPosition,
                                Intrinsic::shang_not, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(ArrayRef<Value *> Ops, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  assert(Ops.size() == 2 || Ops.size() == 3 && "Unexpected operand size!");

  if (Ops.size() == 3) {
    assert(getBitWidth(Ops[2]) == 1 && "Unexpected BitWidth of Carry!");

    return createShangInstPattern(Ops, RetTy, InsertPosition,
                                  Intrinsic::shang_addc, UsedAsArg);
  }

  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_add, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(Value *LHS, Value *RHS, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSAddInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(Value *LHS, Value *RHS, Value *Carry,
                                          Type *RetTy, Value *InsertPosition,
                                          bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS, Carry };
  return createSAddInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSubInst(ArrayRef<Value *> Ops, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  assert(Ops.size() == 2 && "Only support two operands now!");
  Value *A = Ops[0], *B = Ops[1];

  // Transform the A - B into A + ~B + 1.
  Value *NotB = createSNotInst(B, B->getType(), InsertPosition, true);

  return createSAddInst(A, NotB, createIntegerValue(1, 1), RetTy,
                        InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSubInst(Value *LHS, Value *RHS, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSSubInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(ArrayRef<Value *> Ops, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
    return Ops[0];
  }

//   if (Ops.size() == 2) {
//     Value *LHS = Ops[0], *RHS = Ops[1];
// 
//     SmallVector <Value *, 4> LHSPartialProducts;
//     if (isa<ConstantInt>(LHS) && !isa<ConstantInt>(RHS)) {
//       ConstantInt *LHSCI = dyn_cast<ConstantInt>(LHS);
//       unsigned LHSCIVal = LHSCI->getValue().getZExtValue();
// 
//       while(LHSCIVal != 0) {
//         unsigned ShiftNum = Log2_32(LHSCIVal);
//         LHSCIVal = LHSCIVal - unsigned(pow(double(2), double(ShiftNum)));
// 
//         if (ShiftNum == 0) {
//           LHSPartialProducts.push_back(RHS);
//           continue;
//         }
// 
//         Value *ShiftNumVal = createIntegerValue(32, ShiftNum);
//         Value *PartialProduct = createSShiftInst(RHS, ShiftNumVal, RHS->getType(), InsertPosition, Intrinsic::shang_shl, true);
//         LHSPartialProducts.push_back(PartialProduct);
//       }
// 
//       // If there are more than two operands, transform it into mutil-SAddInst.
//       if (LHSPartialProducts.size() > 2) {
//         Value *TempSAddInst = createSAddInst(LHSPartialProducts[0], LHSPartialProducts[1], RetTy, InsertPosition, true);
//         for (unsigned i = 0; i < LHSPartialProducts.size() - 3; i++) {
//           TempSAddInst = createSAddInst(TempSAddInst, LHSPartialProducts[i + 2], RetTy, InsertPosition, true);
//         }
// 
//         int num = LHSPartialProducts.size();
//         return createSAddInst(TempSAddInst, LHSPartialProducts[num - 1], RetTy, InsertPosition, UsedAsArg);
//       }
// 
//       return createSAddInst(LHSPartialProducts, RetTy, InsertPosition, UsedAsArg);
//     }
// 
//     SmallVector <Value *, 4> RHSPartialProducts;
//     if (isa<ConstantInt>(RHS) && !isa<ConstantInt>(LHS)) {
//       ConstantInt *RHSCI = dyn_cast<ConstantInt>(RHS);
//       unsigned RHSCIVal = RHSCI->getValue().getZExtValue();
// 
//       while(RHSCIVal != 0) {
//         unsigned ShiftNum = Log2_32(RHSCIVal);
//         RHSCIVal = RHSCIVal - unsigned(pow(double(2), double(ShiftNum)));
// 
//         if (ShiftNum == 0) {
//           LHSPartialProducts.push_back(LHS);
//           continue;
//         }
// 
//         Value *ShiftNumVal = createIntegerValue(32, ShiftNum);
//         Value *PartialProduct = createSShiftInst(LHS, ShiftNumVal, LHS->getType(), InsertPosition, Intrinsic::shang_shl, true);
//         RHSPartialProducts.push_back(PartialProduct);
//       }
// 
//       // If there are more than two operands, transform it into mutil-SAddInst.
//       if (RHSPartialProducts.size() > 2) {
//         Value *TempSAddInst = createSAddInst(RHSPartialProducts[0], RHSPartialProducts[1], RetTy, InsertPosition, true);
//         for (unsigned i = 0; i < RHSPartialProducts.size() - 3; i++) {
//           TempSAddInst = createSAddInst(TempSAddInst, RHSPartialProducts[i + 2], RetTy, InsertPosition, true);
//         }
// 
//         int num = RHSPartialProducts.size();
//         return createSAddInst(TempSAddInst, RHSPartialProducts[num - 1], RetTy, InsertPosition, UsedAsArg);
//       }
// 
//       return createSAddInst(RHSPartialProducts, RetTy, InsertPosition, UsedAsArg);
//     }
//   }

  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_mul, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(Value *LHS, Value *RHS, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSMulInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSUDivInst(ArrayRef<Value *> Ops, Type *RetTy,
                                           Value *InsertPosition, bool UsedAsArg) {
  assert(Ops.size() == 2 && "Unexpected operand size!");

  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_udiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSDivInst(ArrayRef<Value *> Ops, Type *RetTy,
                                           Value *InsertPosition, bool UsedAsArg) {
  assert(Ops.size() == 2 && "Unexpected operand size!");

  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_sdiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSRemInst(ArrayRef<Value *> Ops, Type *RetTy,
                                           Value *InsertPosition, bool UsedAsArg) {
  Value *LHS = Ops[0], *RHS = Ops[1];

  ConstantInt *LHSCI = dyn_cast<ConstantInt>(LHS);
  ConstantInt *RHSCI = dyn_cast<ConstantInt>(RHS);

  if (LHSCI && RHSCI && !RHSCI->isNullValue()) {
    Value *Result = createIntegerValue(LHSCI->getValue().srem(RHSCI->getValue()));

    if (!UsedAsArg)
      InsertPosition->replaceAllUsesWith(Result);

    return Result;
  }

  if (RHSCI && !RHSCI->isNullValue()) {
    Value *SDivResult = createSSDivInst(Ops, RetTy, InsertPosition, true);

    unsigned BitWidth = getBitWidth(LHS);
    Value *MulResult = createSMulInst(SDivResult, RHS, RetTy, InsertPosition, true);

    return createSSubInst(LHS, MulResult, RetTy, InsertPosition, UsedAsArg);
  }

  llvm_unreachable("Not implemented yet!");
}

Value *SIRDatapathBuilder::createSShiftInst(ArrayRef<Value *> Ops, Type *RetTy,
                                            Value *InsertPosition, Intrinsic::ID FuncID,
                                            bool UsedAsArg) {
  assert(Ops.size() == 2 && "The shift inst must have two operands!");
  Value *LHS = Ops[0]; Value *RHS = Ops[1];

  // Limit the shift amount so keep the behavior of the hardware the same as
  // the corresponding software.
  unsigned RHSMaxSize = Log2_32_Ceil(getBitWidth(LHS));
  if (ConstantInt *CI = dyn_cast<ConstantInt>(RHS))
    assert(getConstantIntValue(CI) < getBitWidth(LHS) && "Unexpected shift amount!");
  if (getBitWidth(RHS) > RHSMaxSize)
    // Extract the useful bits.
    RHS = createSBitExtractInst(Ops[1], RHSMaxSize, 0, createIntegerType(RHSMaxSize), 
                                InsertPosition, true);

  Value *NewOps[] = {LHS, RHS};

  assert(LHS->getType() == RetTy && "RetTy not matches!");

//   if (ConstantInt *RHSCI = dyn_cast<ConstantInt>(RHS)) {
//     unsigned RHSCIVal = RHSCI->getValue().getZExtValue();
// 
//     unsigned LHSBitWidth = getBitWidth(LHS);
// 
//     if (FuncID == Intrinsic::shang_shl) {
//       Value *ExtractResult = createSBitExtractInst(LHS, LHSBitWidth - RHSCIVal, 0, createIntegerType(LHSBitWidth - RHSCIVal), InsertPosition, true);
//       Value *PaddingBits = createIntegerValue(RHSCIVal, 0);
// 
//       return createSBitCatInst(ExtractResult, PaddingBits, RetTy, InsertPosition, UsedAsArg);
//     } else if (FuncID == Intrinsic::shang_lshr) {
//       Value *ExtractResult = createSBitExtractInst(LHS, LHSBitWidth, RHSCIVal, createIntegerType(LHSBitWidth - RHSCIVal), InsertPosition, true);
//       Value *PaddingBits = createIntegerValue(RHSCIVal, 0);
// 
//       return createSBitCatInst(PaddingBits, ExtractResult, RetTy, InsertPosition, UsedAsArg);
//     } else if (FuncID == Intrinsic::shang_ashr) {
//       Value *ExtractResult = createSBitExtractInst(LHS, LHSBitWidth, RHSCIVal, createIntegerType(LHSBitWidth - RHSCIVal), InsertPosition, true);
//       Value *PaddingBit = createSBitExtractInst(LHS, LHSBitWidth, LHSBitWidth - 1, createIntegerType(1), InsertPosition, true);
//       Value *PaddingBits = createSBitRepeatInst(PaddingBit, RHSCIVal, createIntegerType(RHSCIVal), InsertPosition, true);
// 
//       return createSBitCatInst(PaddingBits, ExtractResult, RetTy, InsertPosition, UsedAsArg);
//     }
//   }

  return createShangInstPattern(NewOps, RetTy, InsertPosition, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSShiftInst(Value *LHS, Value *RHS, Type *RetTy,
                                            Value *InsertPosition, Intrinsic::ID FuncID,
                                            bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSShiftInst(Ops, RetTy, InsertPosition, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(ArrayRef<Value *> Ops, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  for (unsigned i = 0; i < Ops.size(); i++)
    assert(Ops[0]->getType() == RetTy && "RetTy not matches!");

  // Handle the trivial case trivially.
  if (Ops.size() == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
    return Ops[0];
  }

  SmallVector<Value *, 4> NewOps;
  typedef ArrayRef<Value *>::iterator iterator;
  for (iterator I = Ops.begin(), E = Ops.end(); I != E; I++) {
    Value *Operand = *I;
    ConstantInt *CI = dyn_cast<ConstantInt>(Operand);

    // If A = x'b0 & B & C, we can simplify it into x'b0.
    if (CI && getConstantIntValue(CI) == 0) {
      // If the inst is not used as an argument of other functions,
      // then it is used to replace the inst in IR
      if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Operand);
      return Operand;
    }

    // If A = 1'b1 & 1'bB & 1'bC, we can simplify it into A = 1'bB & 1'bC.
    if (CI && getConstantIntValue(CI) == 1 && getBitWidth(RetTy) == 1)
      // Ignore the 1'b1 operand, since it have no impact on result.
      continue;

    NewOps.push_back(Operand);
  }

  // If the size of NewOps is zero, it means all operands are 1'b1.
  if (NewOps.size() == 0) {
    assert(getBitWidth(RetTy) == 1 && "Unexpected BitWidth!");
    Value *OneValue = createIntegerValue(1, 1);

    if (!UsedAsArg)
      InsertPosition->replaceAllUsesWith(OneValue);

    return OneValue;
  }

  // If the size of NewOps is one, then the left value is the result.
  if (NewOps.size() == 1) {
    Value *LeftValue = NewOps[0];

    if (!UsedAsArg)
      InsertPosition->replaceAllUsesWith(LeftValue);

    return LeftValue;
  }

  return createShangInstPattern(NewOps, RetTy, InsertPosition,
                                Intrinsic::shang_and, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(Value *LHS, Value *RHS, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  unsigned BitWidth = TD.getTypeSizeInBits(RetTy);
  assert(getBitWidth(LHS) == BitWidth && "Unexpected BitWidth!");
  assert(getBitWidth(RHS) == BitWidth && "Unexpected BitWidth!");

  Value *NewLHS = LHS, *NewRHS = RHS;

  Type *IntTy = createIntegerType(BitWidth);
  // Transform into IntegerTy so it can be taken as operand in Shang Intrinsic instruction.
  if (LHS->getType()->isPointerTy())
    NewLHS = createPtrToIntInst(LHS, IntTy, InsertPosition, true);
  else if (!LHS->getType()->isIntegerTy())
    NewLHS = createBitCastInst(LHS, IntTy, InsertPosition, true);

  if (RHS->getType()->isPointerTy())
    NewRHS = createPtrToIntInst(RHS, IntTy, InsertPosition, true);
  else if (!RHS->getType()->isIntegerTy())
    NewRHS = createBitCastInst(RHS, IntTy, InsertPosition, true);
  // The RetTy also need to be transformed into IntegerTy when we create this instruction as
  // argument of other Shang intrinsic instructions. However, if it is used to replace the
  // origin instruction then we will handle it in the createShangInstPattern function.
  if (UsedAsArg)
    RetTy = IntTy;

  Value *Ops[] = {NewLHS, NewRHS};
  return createSAndInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(ArrayRef<Value *> Ops, Type *RetTy,
                                         Value *InsertPosition, bool UsedAsArg) {
  for (unsigned i = 0; i < Ops.size(); i++)
    assert(Ops[i]->getType() == RetTy && "RetTy not matches!");

  // Handle the trivial case trivially.
  if (Ops.size() == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
    return Ops[0];
  }

  // If there are more than two operands, transform it into mutil-SOrInst.
  if (Ops.size() > 2) {
    Value *TempSOrInst = createSOrInst(Ops[0], Ops[1], RetTy, InsertPosition, true);
    for (unsigned i = 0; i < Ops.size() - 3; i++) {
      TempSOrInst = createSOrInst(TempSOrInst, Ops[i + 2], RetTy, InsertPosition, true);
    }

    int num = Ops.size();
    return createSOrInst(TempSOrInst, Ops[num - 1], RetTy, InsertPosition, UsedAsArg);
  }

  assert(Ops.size() == 2 && "Unexpected Operand Size!");

  SmallVector<Value *, 4> NewOps;
  typedef ArrayRef<Value *>::iterator iterator;
  for (iterator I = Ops.begin(), E = Ops.end(); I != E; I++) {
    Value *Operand = *I;
    ConstantInt *CI = dyn_cast<ConstantInt>(Operand);

//     If A = 1'b1 | 1'bB | 1'bC, we can simplify it into 1'b1.
//           if (CI && getConstantIntValue(CI) == 1 && getBitWidth(RetTy) == 1) {
//             // If the inst is not used as an argument of other functions,
//             // then it is used to replace the inst in IR
//             if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Operand);
//             return Operand;
//           }
//       
//           // If A = x'b0 | B | C, we can simplify it into A = B | C.
//           if (CI && getConstantIntValue(CI) == 0)
//             // Ignore the x'b0 operand, since it have no impact on result.
//             continue;

    NewOps.push_back(Operand);
  }

  // If the size of NewOps is zero, it means all operands are x'b0.
  if (NewOps.size() == 0) {
    Value *ZeroValue = createIntegerValue(getBitWidth(RetTy), 0);

    if (!UsedAsArg)
      InsertPosition->replaceAllUsesWith(ZeroValue);

    return ZeroValue;
  }

  // If the size of NewOps is one, then the left value is the result.
  if (NewOps.size() == 1) {
    Value *LeftValue = NewOps[0];

    if (!UsedAsArg)
      InsertPosition->replaceAllUsesWith(LeftValue);

    return LeftValue;
  }

  return createShangInstPattern(NewOps, RetTy, InsertPosition,
                                Intrinsic::shang_or, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(Value *LHS, Value *RHS, Type *RetTy,
                                         Value *InsertPosition, bool UsedAsArg) {
  unsigned BitWidth = TD.getTypeSizeInBits(RetTy);
  assert(getBitWidth(LHS) == BitWidth && "Unexpected BitWidth!");
  assert(getBitWidth(RHS) == BitWidth && "Unexpected BitWidth!");

  Value *NewLHS = LHS, *NewRHS = RHS;

  Type *IntTy = createIntegerType(BitWidth);
  // Transform into IntegerTy so it can be taken as operand in Shang Intrinsic instruction.
  if (LHS->getType()->isPointerTy())
    NewLHS = createPtrToIntInst(LHS, IntTy, dyn_cast<Instruction>(InsertPosition), true);
  if (RHS->getType()->isPointerTy())
    NewRHS = createPtrToIntInst(RHS, IntTy, dyn_cast<Instruction>(InsertPosition), true);
  // The RetTy also need to be transformed into IntegerTy when we create this instruction as
  // argument of other Shang intrinsic instructions. However, if it is used to replace the
  // origin instruction then we will handle it in the createShangInstPattern function.
  if (UsedAsArg)
    RetTy = IntTy;

  Value *Ops[] = {NewLHS, NewRHS};
  return createSOrInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(ArrayRef<Value *> Ops, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  assert (Ops.size() == 2 && "There should be more than one operand!!");

  for (unsigned i = 0; i < Ops.size(); i++)
    assert(Ops[i]->getType() == RetTy && "RetTy not matches!");

  // Disable the AIG transition for debug convenient.
  //   // Build the Xor Expr with the And Inverter Graph (AIG).
  //   Value *OrInst = createSOrInst(Ops, RetTy, InsertPosition, true);
  //   Value *AndInst = createSAndInst(Ops, RetTy, InsertPosition, true);
  //   Value *NotInst = createSNotInst(AndInst, RetTy, InsertPosition, true);
  //
  //   Value *NewOps[] = {OrInst, NotInst};
  //   return createSAndInst(NewOps, RetTy, InsertPosition, UsedAsArg);
  return createShangInstPattern(Ops, RetTy, InsertPosition,
                                Intrinsic::shang_xor, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(Value *LHS, Value *RHS, Type *RetTy,
                                          Value *InsertPosition, bool UsedAsArg) {
  assert(LHS->getType() == RHS->getType() && LHS->getType() == RetTy
         && "BitWidth not match!");

  Value *Ops[] = {LHS, RHS};
  return createSXorInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrEqualInst(Value *&LHS, Value *RHS, Type *RetTy,
                                              Value *InsertPosition, bool UsedAsArg) {
  assert(LHS->getType() == RHS->getType() && LHS->getType() == RetTy
         && "BitWidth not match!");

  if (LHS == NULL) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(RHS);
    return (LHS = RHS);
  }

  return (LHS = createSOrInst(LHS, RHS, RetTy, InsertPosition, UsedAsArg));
}

Value *SIRDatapathBuilder::createSGEPInst(GEPOperator *GEP, Type *RetTy,
                                          Value *InserPosition, bool UsedAsArg) {
  Instruction *InsertBefore = dyn_cast<Instruction>(InserPosition);
  assert(InsertBefore && "Unexpected NULL InsertBefore!");

  Value *Ptr = getAsOperand(GEP->getPointerOperand(), InsertBefore);
  unsigned PtrSize = getBitWidth(Ptr);

  // Cast the Ptr into int type so we can do the math operation below.
  Value *PtrVal = createPtrToIntInst(Ptr, createIntegerType(PtrSize), InsertBefore, true);

  // Note that the pointer operand may be a vector of pointers. Take the scalar
  // element which holds a pointer.
  Type *Ty = GEP->getPointerOperandType()->getScalarType();

  typedef GEPOperator::op_iterator op_iterator;
  for (op_iterator OI = GEP->idx_begin(), E = GEP->op_end(); OI != E; ++OI) {
    Value *Idx = *OI;
    if (StructType *StTy = dyn_cast<StructType>(Ty)) {
      unsigned Field = getConstantIntValue(Idx);
      if (Field) {
        // N = N + Offset
        uint64_t Offset = TD.getStructLayout(StTy)->getElementOffset(Field);
        PtrVal = createSAddInst(PtrVal, createIntegerValue(PtrSize, Offset),
                                PtrVal->getType(), InserPosition, true);
      }

      Ty = StTy->getElementType(Field);
    } else {
      Ty = cast<SequentialType>(Ty)->getElementType();

      // If this is a constant subscript, handle it quickly.
      if (const ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
        if (CI->isZero()) continue;
        uint64_t Offs = TD.getTypeAllocSize(Ty) * cast<ConstantInt>(CI)->getSExtValue();
        PtrVal = createSAddInst(PtrVal, createIntegerValue(PtrSize, Offs),
                                PtrVal->getType(), InserPosition, true);
        continue;
      }

      // N = N + Idx * ElementSize;
      APInt ElementSize = APInt(PtrSize, TD.getTypeAllocSize(Ty));
      Value *IdxN = const_cast<Value*>(Idx);

      // If the index is smaller or larger than intptr_t, truncate or extend
      // it.
      if (getBitWidth(IdxN) > PtrSize)
        IdxN = createSBitExtractInst(IdxN, PtrSize, 0, createIntegerType(PtrSize),
                                     InserPosition, true);
      else
        IdxN = createSZExtInstOrSelf(IdxN, PtrSize, createIntegerType(PtrSize),
                                     InserPosition, true);

      // If this is a multiply by a power of two, turn it into a shl
      // immediately.  This is a very common case.
      if (ElementSize != 1) {
        if (ElementSize.isPowerOf2()) {
          unsigned Amt = ElementSize.logBase2();
          IdxN = createSShiftInst(IdxN, createIntegerValue(PtrSize, Amt),
                                  IdxN->getType(), InserPosition,
                                  Intrinsic::shang_shl, true);
        } else {
          Value *Scale = createIntegerValue(ElementSize);
          IdxN = createSMulInst(IdxN, Scale, IdxN->getType(), InserPosition, true);
        }
      }

      PtrVal = createSAddInst(PtrVal, IdxN, PtrVal->getType(), InserPosition, true);
    }
  }

  if (!UsedAsArg)
    assert(GEP == InserPosition && "Unexpected InsertPosition!");

  Value *PtrResult = createIntToPtrInst(PtrVal, RetTy, InsertBefore, UsedAsArg);

  return PtrResult;
}

Value *SIRDatapathBuilder::createPtrToIntInst(Value *V, Type *IntTy,
                                              Value *InsertPosition, bool UsedAsArg) {
  assert(IntTy->isIntegerTy() && "Unexpected Type!");

  Value *Inst;
  if (Instruction *I = dyn_cast<Instruction>(InsertPosition))
    Inst = new PtrToIntInst(V, IntTy, "SIRPtrToInt", I);
  else if (BasicBlock *BB = dyn_cast<BasicBlock>(InsertPosition))
    Inst = new PtrToIntInst(V, IntTy, "SIRPtrToInt", BB->getTerminator());

  //assert(Inst && "Instruction not created?");

  if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Inst);

  return Inst;
}

Value *SIRDatapathBuilder::createIntToPtrInst(Value *V, Type *PtrTy,
                                              Value *InsertPosition, bool UsedAsArg) {
  assert(PtrTy->isPointerTy() && "Unexpected Type!");

  Value *Inst;
  if (Instruction *I = dyn_cast<Instruction>(InsertPosition))
    Inst = new IntToPtrInst(V, PtrTy, "SIRIntToPtr", I);
  else if (BasicBlock *BB = dyn_cast<BasicBlock>(InsertPosition))
    Inst = new IntToPtrInst(V, PtrTy, "SIRIntToPtr", BB->getTerminator());

  assert(Inst && "Instruction not created?");

  if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Inst);

  return Inst;
}

Value *SIRDatapathBuilder::createBitCastInst(Value *V, Type *RetTy,
                                             Value *InsertPosition, bool UsedAsArg) {
  Value *Inst;
  if (Instruction *I = dyn_cast<Instruction>(InsertPosition))
    Inst = new BitCastInst(V, RetTy, "SIRBitCast", I);
  else if (BasicBlock *BB = dyn_cast<BasicBlock>(InsertPosition))
    Inst = new BitCastInst(V, RetTy, "SIRBitCast", BB->getTerminator());

  assert(Inst && "Instruction not created?");

  if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Inst);

  return Inst;
}

Value *SIRDatapathBuilder::createCompressorInst(Value *InsertPosition) {
  SmallVector<Value *, 2> Ops;
  Ops.push_back(createIntegerValue(getBitWidth(InsertPosition), 1));

  Value *CompressorInst =  createShangInstPattern(Ops, InsertPosition->getType(),
                                                        InsertPosition, Intrinsic::shang_compressor,
                                                        false);

  Instruction *Compressor = dyn_cast<Instruction>(CompressorInst);

  return CompressorInst;
}

/// Functions to help us create Shang-Inst.
Value *SIRDatapathBuilder::getSignBit(Value *U, Value *InsertPosition) {
  unsigned BitWidth = getBitWidth(U);
  return createSBitExtractInst(U, BitWidth, BitWidth - 1, createIntegerType(1),
                               InsertPosition, true);
}

IntegerType *SIRDatapathBuilder::createIntegerType(unsigned BitWidth) {
  return IntegerType::get(SM->getContext(), BitWidth);
}

Value *SIRDatapathBuilder::createIntegerValue(unsigned BitWidth, unsigned Val) {
  IntegerType *T = createIntegerType(BitWidth);
  ConstantInt *CI =  ConstantInt::get(T, Val);
  APInt V = CI->getValue();

  // The mask of constant value is itself.
  SIRBitMask Mask(~V, V, V.getNullValue(BitWidth));
  SM->IndexVal2BitMask(CI, Mask);

  return CI;
}

Value *SIRDatapathBuilder::createIntegerValue(APInt Val) {
  Value *V = ConstantInt::get(SM->getContext(), Val);

  // The mask of constant value is itself.
  SIRBitMask Mask(~Val, Val, Val.getNullValue(Val.getBitWidth()));
  SM->IndexVal2BitMask(V, Mask);

  return V;
}