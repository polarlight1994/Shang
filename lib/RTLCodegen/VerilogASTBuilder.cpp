//===- Writer.cpp - VTM machine instructions to RTL verilog  ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VerilogASTBuilder pass, which write VTM machine
// instructions in form of RTL verilog code.
//
//===----------------------------------------------------------------------===//
#include "MachineFunction2Datapath.h"

#include "vtm/Passes.h"
#include "vtm/VFInfo.h"
#include "vtm/LangSteam.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VInstrInfo.h"
#include "vtm/VASTSubModules.h"
#include "vtm/VASTModule.h"
#include "vtm/VerilogModuleAnalysis.h"
#include "vtm/Utilities.h"

#include "llvm/Constants.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Type.h"
#include "llvm/Module.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetData.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "vtm-rtl-codegen"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(SlotsByPassed, "Number of slots are bypassed");

namespace {
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

class VerilogASTBuilder : public MachineFunctionPass,
                          public DatapathBuilderContext {
  const Module *M;
  MachineFunction *MF;
  TargetData *TD;
  VRegisterInfo *TRI;
  VFInfo *FInfo;
  MachineRegisterInfo *MRI;
  VASTModule *VM;
  OwningPtr<DatapathBuilder> Builder;
  MemBusBuilder *MBBuilder;
  std::map<unsigned, VASTNode*> EmittedSubModules;

  VASTSubModule *getSubModule(unsigned FNNum) const {
    std::map<unsigned, VASTNode*>::const_iterator at
      = EmittedSubModules.find(FNNum);
    assert(at != EmittedSubModules.end() && "Submodule not yet created!");
    return dyn_cast<VASTSubModule>(at->second);
  }

  VASTImmediate *getOrCreateImmediate(const APInt &Value) {
    return VM->getOrCreateImmediateImpl(Value);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB) {
    return VM->createExprImpl(Opc, Ops, UB, LB);
  }

  typedef DatapathBuilder::RegIdxMapTy RegIdxMapTy;
  RegIdxMapTy Idx2Reg;

  VASTValPtr lookupSignal(unsigned RegNum) {
    if (TargetRegisterInfo::isPhysicalRegister(RegNum)) {
      RegIdxMapTy::const_iterator at = Idx2Reg.find(RegNum);
      assert(at != Idx2Reg.end() && "Signal not found!");

      return at->second;
    }

    // Retrieve the expression.
    VASTValPtr Expr = Builder->lookupExpr(RegNum);
    if (!Expr) {
      // For pipelined loop, we may visit the user before visiting
      // the defining instruction.
      MachineInstr *MI = MRI->getVRegDef(RegNum);
      assert(MI && "Register definition not found!");
      Expr = Builder->createAndIndexExpr(MI);
    }
    
    return Expr;
  }

  VASTValPtr indexPhysReg(unsigned RegNum, VASTValPtr V) {
    assert(TargetRegisterInfo::isPhysicalRegister(RegNum)
           && "Expect physical register!");
    bool inserted = Idx2Reg.insert(std::make_pair(RegNum, V)).second;
    assert(inserted && "RegNum already indexed some value!");

    return V;
  }

  VASTRegister *addDataRegister(unsigned RegNum, unsigned BitWidth,
                                const char *Attr = "") {
    std::string Name = "p" + utostr_32(RegNum) + "r";

    VASTRegister *R = VM->addDataRegister(Name, BitWidth, RegNum, Attr);
    indexPhysReg(RegNum, R->getValue());
    return R;
  }

  VASTSlot *getInstrSlot(MachineInstr *MI) {
    unsigned SlotNum = VInstrInfo::getInstrSlotNum(MI);
    return VM->getSlot(SlotNum - 1);
  }

  VASTSlot *getOrCreateCtrlStartSlot(MachineInstr *MI, unsigned ParentIdx) {
    unsigned SlotNum = VInstrInfo::getBundleSlot(MI);
    return VM->getOrCreateSlot(SlotNum - 1, MI);
  }

  void OrCnd(VASTValPtr &U, VASTValPtr Cnd) {
    if (!U)  U = Cnd;
    else     U = Builder->buildOrExpr(Cnd, U, 1);
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd) {
    OrCnd(S->getOrCreateSuccCnd(NextSlot), Cnd);
  }

  void addSlotDisable(VASTSlot *S, VASTSeqValue *P, VASTValPtr Cnd) {
    OrCnd(S->getOrCreateDisable(P), Cnd);
  }

  void addSlotReady(VASTSlot *Slot, VASTValue *V, VASTValPtr Cnd) {
    OrCnd(Slot->getOrCreateReady(V), Cnd);
  }

  void addSlotEnable(VASTSlot *S, VASTSeqValue *P, VASTValPtr Cnd) {
    OrCnd(S->getOrCreateEnable(P), Cnd);
  }

  void addAssignment(VASTSeqValue *V, VASTValPtr Src, VASTSlot *Slot,
                     ArrayRef<VASTValPtr> Cnds, MachineInstr *DefMI = 0,
                     bool AddSlotActive = true);

  void addSlotReady(MachineInstr *MI, VASTSlot *Slot);
  void emitFunctionSignature(const Function *F, VASTSubModule *SubMod = 0);
  void emitCommonPort(VASTSubModule *SubMod);
  void emitAllocatedFUs();
  VASTNode *emitSubModule(const char *CalleeName, unsigned FNNum);
  void emitIdleState();

  void emitBasicBlock(MachineBasicBlock &MBB);

  void emitAllSignals();

  // Mapping success fsm state to their predicate in current state.
  void emitCtrlOp(MachineBasicBlock::instr_iterator ctrl_begin,
                  MachineBasicBlock::instr_iterator ctrl_end);

  MachineBasicBlock::iterator emitDatapath(MachineInstr *Bundle);

  typedef SmallVectorImpl<VASTValPtr> VASTValueVecTy;
  // Emit the operations in the first micro state in the FSM state when we are
  // jumping to it.
  // Return true if the first slot of DstBB is bypassed.
  bool emitFirstCtrlBundle(MachineBasicBlock *DstBB, VASTSlot *Slot,
                           VASTValueVecTy &Cnds);

  void emitBr(MachineInstr *MI, VASTSlot *CurSlot, VASTValueVecTy &Cnds,
              MachineBasicBlock *CurBB);

  VASTValPtr getAsOperandImpl(MachineOperand &Op, bool GetAsInlineOperand = true);

  template <class Ty>
  Ty *getAsLValue(MachineOperand &Op) {
    assert(Op.isReg() && "Bad MO type for LValue!");
    if (VASTValPtr V = lookupSignal(Op.getReg())) {
      assert(!V.isInverted()
             && "Don't know how to handle inverted LValue at the moment!");
      return dyn_cast<Ty>(V);
    }

    return 0;
  }

  void printOperand(MachineOperand &Op, raw_ostream &OS);

  void emitOpInternalCall(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpReadReturn(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpUnreachable(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpRetVal(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpRet(MachineInstr *MIRet, VASTSlot *CurSlot, VASTValueVecTy &Cnds);
  void emitOpReadFU(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpDisableFU(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);

  void emitOpMemTrans(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds);
  void emitOpBRamTrans(MachineInstr *MI, VASTSlot *Slot, VASTValueVecTy &Cnds,
                       bool IsWrite);

public:
  /// @name FunctionPass interface
  //{
  static char ID;

  VerilogASTBuilder() : MachineFunctionPass(ID) {
    initializeVerilogASTBuilderPass(*PassRegistry::getPassRegistry());
  }

  ~VerilogASTBuilder();

  bool doInitialization(Module &M);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<VerilogModuleAnalysis>();
    AU.addRequiredID(MachineBasicBlockTopOrderID);
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  void releaseMemory() {
    Builder.reset();
    EmittedSubModules.clear();
    Idx2Reg.clear();
  }

  bool runOnMachineFunction(MachineFunction &MF);

  virtual void print(raw_ostream &O, const Module *M) const;
  //}
};

}

//===----------------------------------------------------------------------===//
char VerilogASTBuilder::ID = 0;

Pass *llvm::createVerilogASTBuilderPass() {
  return new VerilogASTBuilder();
}

INITIALIZE_PASS_BEGIN(VerilogASTBuilder, "vtm-rtl-info-VerilogASTBuilder",
                      "Build RTL Verilog module for synthesised function.",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(MachineBasicBlockTopOrder);
  INITIALIZE_PASS_DEPENDENCY(VerilogModuleAnalysis);
INITIALIZE_PASS_END(VerilogASTBuilder, "vtm-rtl-info-VerilogASTBuilder",
                    "Build RTL Verilog module for synthesised function.",
                    false, true)

bool VerilogASTBuilder::doInitialization(Module &Mod) {
  M = &Mod;
  return false;
}

bool VerilogASTBuilder::runOnMachineFunction(MachineFunction &F) {
  MF = &F;
  TD = getAnalysisIfAvailable<TargetData>();
  FInfo = MF->getInfo<VFInfo>();
  MRI = &MF->getRegInfo();
  TargetRegisterInfo *RegInfo
    = const_cast<TargetRegisterInfo*>(MF->getTarget().getRegisterInfo());
  TRI = reinterpret_cast<VRegisterInfo*>(RegInfo);

  Builder.reset(new DatapathBuilder(*this, *MRI));
  VerilogModuleAnalysis &VMA = getAnalysis<VerilogModuleAnalysis>();
  VM = VMA.createModule(FInfo->getInfo().getModName());
  VM->allocaSlots(FInfo->getTotalSlots());

  emitFunctionSignature(F.getFunction());

  // Note: Create the memory bus builder will add the input/output ports of the
  // memory bus implicitly. We should add these ports after function
  // "emitFunctionSignature" is called, which add some other ports that need to
  // be added before input/output ports of memory bus.
  MemBusBuilder MBB(VM, *Builder, 0);
  MBBuilder = &MBB;

  // Emit all function units then emit all register/wires because function units
  // may alias with registers.
  emitAllocatedFUs();
  emitAllSignals();

  // States of the control flow.
  emitIdleState();

  typedef MachineFunction::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I)
    emitBasicBlock(*I);

  // Build the mux for memory bus.
  MBBuilder->buildMemBusMux();

  // Building the Slot active signals.
  VM->buildSlotLogic(*Builder);

  // Assign names to the data-path expressions.
  VM->nameDatapath();

  // Release the context.
  releaseMemory();
  return false;
}

void VerilogASTBuilder::print(raw_ostream &O, const Module *M) const {

}

void VerilogASTBuilder::addAssignment(VASTSeqValue *V, VASTValPtr Src,
                                      VASTSlot *Slot, ArrayRef<VASTValPtr> Cnds,
                                      MachineInstr *DefMI, bool AddSlotActive) {
  if (!Src) return;

  VASTValPtr Cnd = Builder->buildAndExpr(Cnds, 1);
  VM->addAssignment(V, Src, Slot, Cnd, DefMI, AddSlotActive);
}

void VerilogASTBuilder::emitIdleState() {
  // The module is busy now
  MachineBasicBlock *EntryBB =  GraphTraits<MachineFunction*>::getEntryNode(MF);
  VASTSlot *IdleSlot = VM->getOrCreateStartSlot();
  IdleSlot->buildReadyLogic(*VM, *Builder);
  VASTValue *StartPort = VM->getPort(VASTModule::Start).getValue();
  addSuccSlot(IdleSlot, IdleSlot, Builder->buildNotExpr(StartPort));

  // Always Disable the finish signal.
  addSlotDisable(IdleSlot, VM->getPort(VASTModule::Finish).getSeqVal(),
                 VM->getBoolImmediateImpl(true));
  SmallVector<VASTValPtr, 1> Cnds(1, StartPort);
  if (!emitFirstCtrlBundle(EntryBB, IdleSlot, Cnds)) {
    unsigned EntryStartSlot = FInfo->getStartSlotFor(EntryBB);
    // Get the second control bundle by skipping the first control bundle and
    // data-path bundle.
    MachineBasicBlock::iterator I = llvm::next(EntryBB->begin(), 2);

    addSuccSlot(IdleSlot, VM->getOrCreateSlot(EntryStartSlot, I), StartPort);
  }
}

void VerilogASTBuilder::emitBasicBlock(MachineBasicBlock &MBB) {
  unsigned startSlot = FInfo->getStartSlotFor(&MBB);
  unsigned IISlot = FInfo->getIISlotFor(&MBB);
  unsigned II = IISlot - startSlot;
  unsigned EndSlot = FInfo->getEndSlotFor(&MBB);
  typedef MachineBasicBlock::instr_iterator instr_iterator;
  typedef MachineBasicBlock::iterator bundle_iterator;
  bundle_iterator I = MBB.getFirstNonPHI();
  // Skip the first bundle, it already emitted by the predecessor bbs.
  ++I;

  // Emit the data-path bundle right after the first bundle.
  I = emitDatapath(I);

  // Create the slots for all control-path bundles in the current BB.
  for (bundle_iterator BI = I, BE = MBB.end(); BI != BE && !BI->isTerminator();
       BI = llvm::next(BI, 2) ) {
    VASTSlot *LeaderSlot = getOrCreateCtrlStartSlot(BI, startSlot);
    // Create the alias slots for the pipelined loop.
    if (startSlot + II < EndSlot) {
      LeaderSlot->setAliasSlots(LeaderSlot->SlotNum, EndSlot, II);
      unsigned CurSlotNum = LeaderSlot->SlotNum;
      for (unsigned S = CurSlotNum + II; S < EndSlot; S += II)
        VM->getOrCreateSlot(S, BI)->setAliasSlots(CurSlotNum, EndSlot, II);
    }
  }

  // Emit the other bundles.
  while(!I->isTerminator()) {
    // We are assign the register at the previous slot of this slot, so the
    // data-path op with same slot can read the register schedule to this slot.
    unsigned CurSlotNum = VInstrInfo::getBundleSlot(I) - 1;

    // Collect slot ready signals.
    instr_iterator NextI = instr_iterator(I);

    // Create and collect the slots.
    VASTSlot *LeaderSlot = VM->getSlot(CurSlotNum);
    assert(LeaderSlot->getBundleStart() == NextI && "BundleStart not match!");

    while ((++NextI)->getOpcode() != VTM::CtrlEnd)
      if (NextI->getOpcode() == VTM::VOpReadFU)
        addSlotReady(NextI, getInstrSlot(NextI));

    // Build the straight-line control-flow. Note that when
    // CurSlotNum == startSlot, slot[CurSlotNum - 1] is in other MBB, and the
    // condition is not always true. Such control-flow is handled by function
    // "emitOpBr".
    if (CurSlotNum != startSlot)
      addSuccSlot(VM->getSlot(CurSlotNum - 1), LeaderSlot,
                  VM->getBoolImmediateImpl(true));

    LeaderSlot->buildReadyLogic(*VM, *Builder);

    // There will be alias slot if the BB is pipelined.
    if (startSlot + II < EndSlot) {
      for (unsigned slot = CurSlotNum + II; slot < EndSlot; slot += II) {
        VASTSlot *S = VM->getSlot(slot);
        addSuccSlot(VM->getSlot(slot - 1), S, VM->getBoolImmediateImpl(true));
        S->buildReadyLogic(*VM, *Builder);
      }
    }

    // Emit the control operations.
    emitCtrlOp(instr_iterator(I), NextI);
    I = bundle_iterator(llvm::next(NextI));
    // Emit the date-path of current state.
    I = emitDatapath(I);
  }
}

void VerilogASTBuilder::addSlotReady(MachineInstr *MI, VASTSlot *Slot) {
  FuncUnitId Id = VInstrInfo::getPreboundFUId(MI);

  switch (Id.getFUType()) {
  case VFUs::CalleeFN: {
    // The register representing the function unit is store in the src operand
    // of VOpReadFU.
    if (VASTSubModule *Submod = getSubModule(MI->getOperand(1).getReg()))
      // TODO: Assert not in first slot.
      addSlotReady(Slot, Submod->getFinPort(), Builder->createCnd(MI));

    break;
  }
  default: return;
  }
}

void VerilogASTBuilder::emitAllocatedFUs() {
  for (VFInfo::const_bram_iterator I = FInfo->bram_begin(), E = FInfo->bram_end();
       I != E; ++I) {
    unsigned BRAMNum = I->first;
    const VFInfo::BRamInfo &Info = I->second;

    //const Value* Initializer = Info.Initializer;
    unsigned NumElem = Info.NumElem;
    unsigned DataWidth = Info.ElemSizeInBytes * 8;
    const GlobalVariable *Initializer =
      dyn_cast_or_null<GlobalVariable>(Info.Initializer);
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
      // Dirty Hack: For some block RAM, there is no read access, while for
      // some others, there is no write access.
      unsigned PhysReg = std::max(Info.ReadPortARegNum, Info.WritePortARegnum);
      assert((Info.ReadPortARegNum == Info.WritePortARegnum
              || Info.WritePortARegnum == 0 || Info.ReadPortARegNum == 0)
             && "The same register should be assigned to the single element BRAM"
                " or there is no write access!");
      VASTRegister *R = VM->addDataRegister(VFUBRAM::getOutDataBusName(PhysReg),
                                            DataWidth, PhysReg);
      indexPhysReg(PhysReg, R->getValue());
      continue;
    }

    // Create the block RAM object.
    VASTBlockRAM *BRAM = VM->addBlockRAM(BRAMNum, DataWidth, NumElem, Initializer);

    // Index the read port if there is any read accesses.
    if (unsigned ReadPortNum = Info.ReadPortARegNum)
      indexPhysReg(ReadPortNum, BRAM->getRAddr(0));

    // Index the write ports if there is any write access.
    if (unsigned WritePortNum = Info.WritePortARegnum) {
      indexPhysReg(WritePortNum, BRAM->getWAddr(0));
      indexPhysReg(WritePortNum + 1, BRAM->getWData(0));
    }
  }
}

VASTNode *VerilogASTBuilder::emitSubModule(const char *CalleeName, unsigned FNNum) {
  VASTNode *&N = EmittedSubModules[FNNum];
  // Do not emit a submodule more than once.
  if (N) return N;

  if (const Function *Callee = M->getFunction(CalleeName)) {
    if (!Callee->isDeclaration()) {
      VASTSubModule *SubMod = VM->addSubmodule(CalleeName, FNNum);
      emitFunctionSignature(Callee, SubMod);
      SubMod->setIsSimple();
      N = SubMod;
      return SubMod;
    }
  }

  SmallVector<VFUs::ModOpInfo, 4> OpInfo;
  unsigned Latency = VFUs::getModuleOperands(CalleeName, FNNum, OpInfo);
  VRegisterInfo::PhyRegInfo Info = TRI->getPhyRegInfo(FNNum);

  // Submodule information not available, create the seqential code.
  if (OpInfo.empty()) {
    N = VM->addSeqCode(CalleeName);
    return N;
  }

  VASTSubModule *SubMod = VM->addSubmodule(CalleeName, FNNum);
  N = SubMod;
  SubMod->setIsSimple(false);
  SmallVector<VASTValPtr, 4> Ops;
  // Add the fanin registers.
  for (unsigned i = 0, e = OpInfo.size(); i < e; ++i) {
    VASTRegister *R = VM->addOpRegister(OpInfo[i].first, OpInfo[i].second, FNNum);
    SubMod->addInPort(OpInfo[i].first, R->getValue());
    Ops.push_back(R->getValue());
  }
  // Add the start register.
  indexPhysReg(FNNum + 1, SubMod->createStartPort(VM));
  // Create the finish signal from the submodule.
  SubMod->createFinPort(VM);

  // Dose the submodule have a return port?
  if (Info.getBitWidth()) {
    indexPhysReg(FNNum, SubMod->createRetPort(VM, Info.getBitWidth(), Latency));
    return SubMod;
  }

  // Else do not has return port.
  indexPhysReg(FNNum, 0);
  return SubMod;
}

void VerilogASTBuilder::emitFunctionSignature(const Function *F,
                                              VASTSubModule *SubMod) {
  for (Function::const_arg_iterator I = F->arg_begin(), E = F->arg_end();
       I != E; ++I) {
    const Argument *Arg = I;
    std::string Name = Arg->getName();
    unsigned BitWidth = TD->getTypeSizeInBits(Arg->getType());
    // Add port declaration.
    if (SubMod) {
      std::string RegName = SubMod->getPortName(Name);
      VASTRegister *R = VM->addOpRegister(RegName, BitWidth, SubMod->getNum());
      SubMod->addInPort(Name, R->getValue());
      continue;
    }

    VM->addInputPort(Name, BitWidth, VASTModule::ArgPort);
  }

  Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD->getTypeSizeInBits(RetTy);
    if (SubMod)
      indexPhysReg(SubMod->getNum(), SubMod->createRetPort(VM, BitWidth));
    else
      VM->addOutputPort("return_value", BitWidth, VASTModule::RetPort);
  }

  emitCommonPort(SubMod);
}

void VerilogASTBuilder::emitCommonPort(VASTSubModule *SubMod) {
  if (SubMod) {
    // It is a callee function, emit the signal for the sub module.
    SubMod->createStartPort(VM);
    SubMod->createFinPort(VM);
    // Also connedt the memory bus.
    MBBuilder->addSubModule(SubMod);
  } else { // If F is current function.
    VM->addInputPort("clk", 1, VASTModule::Clk);
    VM->addInputPort("rstN", 1, VASTModule::RST);
    VM->addInputPort("start", 1, VASTModule::Start);
    VM->addOutputPort("fin", 1, VASTModule::Finish);
  }
}

void VerilogASTBuilder::emitAllSignals() {
  for (unsigned i = 0, e = TRI->num_phyreg(); i != e; ++i) {
    unsigned RegNum = i + 1;
    VRegisterInfo::PhyRegInfo Info = TRI->getPhyRegInfo(RegNum);
    if (!Info.isTopLevelReg(RegNum)
        // Sub-register for RCFNRegClass already handled in
        // emitFunctionSignature called by emitAllocatedFUs;
        && Info.getRegClass() != VTM::RCFNRegClassID) {
      VASTValPtr Parent = lookupSignal(Info.getParentRegister());
      indexPhysReg(RegNum, Builder->buildBitSliceExpr(Parent.getAsInlineOperand(),
                                                      Info.getUB(), Info.getLB()));
      continue;
    }

    switch (Info.getRegClass()) {
    case VTM::DRRegClassID:
      addDataRegister(RegNum, Info.getBitWidth());
      break;
    case VTM::RINFRegClassID: {
      // FIXME: Do not use such magic number!
      // The offset of data input port is 3
      unsigned DataInIdx = VM->getFUPortOf(FuncUnitId(VFUs::MemoryBus, 0)) + 3;
      VASTValue *V = VM->getPort(DataInIdx).getValue();
      indexPhysReg(RegNum, V);
      break;
    }
    case VTM::RBRMRegClassID:
    case VTM::RCFNRegClassID:
      /*Nothing to do, it is allocated on the fly*/
      break;
    case VTM::RMUXRegClassID: {
      std::string Name = "dstmux" + utostr_32(RegNum) + "r";
      VASTRegister *R = VM->addDataRegister(Name, Info.getBitWidth(), RegNum);
      indexPhysReg(RegNum, R->getValue());
      break;
    }
    default: llvm_unreachable("Unexpected register class!"); break;
    }
  }
}

VerilogASTBuilder::~VerilogASTBuilder() {}

//===----------------------------------------------------------------------===//
void VerilogASTBuilder::emitCtrlOp(MachineBasicBlock::instr_iterator ctrl_begin,
                                   MachineBasicBlock::instr_iterator ctrl_end) {
  MachineBasicBlock *CurBB = ctrl_begin->getParent();
  assert(ctrl_begin->getOpcode() == VTM::CtrlStart && "Expect control bundle!");
  SmallVector<VASTValPtr, 4> Cnds;

  typedef MachineBasicBlock::instr_iterator instr_it;
  for (instr_it I = llvm::next(ctrl_begin); I != ctrl_end; ++I) {
    MachineInstr *MI = I;

    VASTSlot *CurSlot = getInstrSlot(MI);
    assert(VInstrInfo::getInstrSlotNum(MI) !=
             FInfo->getStartSlotFor(CurSlot->getParentBB())
           && "Unexpected first slot!");

    Cnds.push_back(Builder->createCnd(MI));

    // Emit the operations.
    switch (MI->getOpcode()) {
    case VTM::VOpDstMux:
    case VTM::VOpMoveArg:
    case VTM::VOpMove:
    case VTM::VOpMvPhi:
    case VTM::VOpMvPipe:        emitOpReadFU(MI, CurSlot, Cnds);          break;
    case VTM::VOpReadFU:        emitOpReadFU(MI, CurSlot, Cnds);          break;
    case VTM::VOpDisableFU:     emitOpDisableFU(MI, CurSlot, Cnds);       break;
    case VTM::VOpInternalCall:  emitOpInternalCall(MI, CurSlot, Cnds);    break;
    case VTM::VOpRetVal:        emitOpRetVal(MI, CurSlot, Cnds);          break;
    case VTM::VOpRet_nt:        emitOpRet(MI, CurSlot, Cnds);             break;
    case VTM::VOpMemTrans:      emitOpMemTrans(MI, CurSlot, Cnds);        break;
    case VTM::VOpBRAMRead:      emitOpBRamTrans(MI, CurSlot, Cnds, false);break;
    case VTM::VOpBRAMWrite:     emitOpBRamTrans(MI, CurSlot, Cnds, true); break;
    case VTM::VOpToState_nt:    emitBr(MI, CurSlot, Cnds, CurBB);         break;
    case VTM::VOpReadReturn:    emitOpReadReturn(MI, CurSlot, Cnds);      break;
    case VTM::VOpUnreachable:   emitOpUnreachable(MI, CurSlot, Cnds);     break;
    default:  assert(0 && "Unexpected opcode!");                          break;
    }
    Cnds.pop_back();
    assert(Cnds.empty() && "Unexpected extra predicate operand!");
  }
}

bool VerilogASTBuilder::emitFirstCtrlBundle(MachineBasicBlock *DstBB,
                                            VASTSlot *Slot,
                                            VASTValueVecTy &Cnds) {
  // TODO: Emit PHINodes if necessary.
  MachineInstr *FirstBundle = DstBB->instr_begin();
  assert(FInfo->getStartSlotFor(DstBB) == VInstrInfo::getBundleSlot(FirstBundle)
         && FirstBundle->getOpcode() == VTM::CtrlStart && "Broken Slot!");

  typedef MachineBasicBlock::instr_iterator instr_it;
  instr_it I = FirstBundle;
  while ((++I)->isInsideBundle()) {
    MachineInstr *MI = I;
    if (MI->getOpcode() == VTM::CtrlEnd) break;

    Cnds.push_back(Builder->createCnd(MI));
    switch (MI->getOpcode()) {
    case VTM::VOpDstMux:
    case VTM::VOpMoveArg:
    case VTM::VOpMove:
    case VTM::VOpMvPhi:
    case VTM::COPY:             emitOpReadFU(MI, Slot, Cnds);   break;
    case VTM::VOpDefPhi:                                      break;
    case VTM::VOpToState_nt:
      emitBr(MI, Slot, Cnds, DstBB);
      ++SlotsByPassed;
      break;
    case VTM::VOpRetVal:        emitOpRetVal(MI, Slot, Cnds); break;
    case VTM::VOpInternalCall:  emitOpInternalCall(MI, Slot, Cnds);    break;
    case VTM::VOpRet_nt:
      emitOpRet(MI, Slot, Cnds);
      ++SlotsByPassed;
      break;
    case VTM::VOpMemTrans:      emitOpMemTrans(MI, Slot, Cnds);        break;
    case VTM::VOpBRAMRead:      emitOpBRamTrans(MI, Slot, Cnds, false);break;
    case VTM::VOpBRAMWrite:     emitOpBRamTrans(MI, Slot, Cnds, true); break;
    default:  llvm_unreachable("Unexpected opcode!");         break;
    }
    Cnds.pop_back();
  }

  return FInfo->getTotalSlotFor(DstBB) == 0;
}

void VerilogASTBuilder::emitBr(MachineInstr *MI, VASTSlot *CurSlot,
                               VASTValueVecTy &Cnds, MachineBasicBlock *CurBB) {
  MachineOperand &CndOp = MI->getOperand(0);
  Cnds.push_back(Builder->createCnd(CndOp));

  MachineBasicBlock *TargetBB = MI->getOperand(1).getMBB();
  assert(VInstrInfo::getPredOperand(MI)->getReg() == 0 &&
    "Cannot handle predicated BrCnd");

  // Emit the first micro state of the target state.
  if (!emitFirstCtrlBundle(TargetBB, CurSlot, Cnds)) {
    // Build the edge if the edge is not bypassed.
    unsigned TargetSlotNum = FInfo->getStartSlotFor(TargetBB);
    // Get the second control bundle by skipping the first control bundle and
    // data-path bundle.
    MachineBasicBlock::iterator I = llvm::next(TargetBB->begin(), 2);

    VASTSlot *TargetSlot = VM->getOrCreateSlot(TargetSlotNum, I);
    VASTValPtr Cnd = Builder->buildAndExpr(Cnds, 1);
    addSuccSlot(CurSlot, TargetSlot, Cnd);
  }
  Cnds.pop_back();
}

void VerilogASTBuilder::emitOpUnreachable(MachineInstr *MI, VASTSlot *Slot,
                                          VASTValueVecTy &Cnds) {
  addSuccSlot(Slot, VM->getFinishSlot(), VM->getBoolImmediateImpl(true));
}

void VerilogASTBuilder::emitOpReadFU(MachineInstr *MI, VASTSlot *Slot,
                                     VASTValueVecTy &Cnds) {
  // The dst operand of ReadFU change to immediate if it is dead.
  if (!MI->getOperand(0).isReg() || !MI->getOperand(0).getReg())
    return;

  MachineOperand &Dst = MI->getOperand(0), &Src = MI->getOperand(1);
  VASTSeqValue *DstR = getAsLValue<VASTSeqValue>(Dst);
  VASTValPtr SrcVal = getAsOperandImpl(Src);

  // Ignore the identical copy.
  if (DstR == SrcVal) return;

  addAssignment(DstR, SrcVal, Slot, Cnds, MI, true);
}

void VerilogASTBuilder::emitOpDisableFU(MachineInstr *MI, VASTSlot *Slot,
                                        VASTValueVecTy &Cnds) {
  FuncUnitId Id = VInstrInfo::getPreboundFUId(MI);
  unsigned FUNum = Id.getFUNum();
  VASTValPtr EnablePort = 0;

  switch (Id.getFUType()) {
  case VFUs::MemoryBus:
    EnablePort = VM->getSymbol(VFUMemBus::getEnableName(FUNum) + "_r");
    break;
  case VFUs::CalleeFN: {
    VASTSubModule *Submod = getSubModule(MI->getOperand(0).getReg());
    // The submodule information is not available.
    if (!Submod) return;

    EnablePort =  Submod->getStartPort();
    
    break;
  }
  default:
    llvm_unreachable("Unexpected FU to disable!");
    break;
  }

  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);
  addSlotDisable(Slot, cast<VASTSeqValue>(EnablePort), Pred);
}

void VerilogASTBuilder::emitOpReadReturn(MachineInstr *MI, VASTSlot *Slot,
                                         VASTValueVecTy &Cnds) {
  VASTSeqValue *R = getAsLValue<VASTSeqValue>(MI->getOperand(0));
  // Dirty Hack: Do not trust the bitwidth information of the operand
  // representing the return port.
  addAssignment(R, lookupSignal(MI->getOperand(1).getReg()), Slot, Cnds, MI);
}

void VerilogASTBuilder::emitOpInternalCall(MachineInstr *MI, VASTSlot *Slot,
                                           VASTValueVecTy &Cnds) {
  // Assign input port to some register.
  const char *CalleeName = MI->getOperand(1).getSymbolName();
  unsigned FNNum = FInfo->getCalleeFNNum(CalleeName);

  // Emit the submodule on the fly.
  VASTNode *N = emitSubModule(CalleeName, FNNum);

  if (VASTSubModule *Submod = dyn_cast<VASTSubModule>(N)) {
    VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);
    // Start the submodule.
    // Because there maybe conflict between the disabling and enabling the start
    // port, we need to resolve it in "buildSlotLogic", instead of simply adding
    // this transaction to the VASTSeqOp.
    addSlotEnable(Slot, Submod->getStartPort(), Pred);
    unsigned NumOperands = MI->getNumOperands() - 4;
    VASTSeqOp *Op = VM->createSeqOp(Slot, Pred, NumOperands, MI, true);
    // Assign the new value for this function call to the operand registers.
    for (unsigned i = 4, e = MI->getNumOperands(); i != e; ++i) {
      VASTValPtr V = getAsOperandImpl(MI->getOperand(i));
      Op->addSrc(V, i - 4, false, Submod->getFanin(i - 4));
    }

    return;
  }

  // Else we had to write the control code to the control block.
  VASTSeqCode *Code = cast<VASTSeqCode>(N);
  SmallVector<VASTValPtr, 4> Operands;

  for (unsigned i = 4, e = MI->getNumOperands(); i != e; ++i) {
    MachineOperand &Operand = MI->getOperand(i);
    if (Operand.isReg() && (Operand.getReg() == 0 || Operand.isImplicit()))
      continue;

    // It is the format string?
    StringRef FmtStr;
    if (Operand.isGlobal()
        && getConstantStringInfo(Operand.getGlobal(), FmtStr)) {
      std::string s;
      {
        raw_string_ostream SS(s);
        std::string s;
        {
          raw_string_ostream SS(s);
          SS << '"';
          PrintEscapedString(FmtStr, SS);
          SS << '"';
          SS.flush();

        }
        PrintEscapedString(s,SS);
      }

      Operands.push_back(VM->getOrCreateSymbol(s, 1, false));
      continue;
    }

    Operands.push_back(getAsOperandImpl(Operand));
  }

  // Create the VASTSeqOp
  VASTValPtr Cnd = Builder->buildAndExpr(Cnds, 1);
  VASTSeqOp *Op = VM->createSeqOp(Slot, Cnd, Operands.size(), MI, true);
  // Create the operand list.
  unsigned Idx = 0;
  typedef VASTSeqOp::op_iterator op_iterator;
  for (op_iterator I = Op->src_begin(), E = Op->src_end(); I != E; ++I) {
    new (I) VASTUse(Code, Operands[Idx++]);
  }

  // Add the operation to the seqcode.
  Code->addSeqOp(Op);
}

void VerilogASTBuilder::emitOpRet(MachineInstr *MI, VASTSlot *CurSlot,
                                  VASTValueVecTy &Cnds) {
  // Go back to the idle slot.
  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);
  addSuccSlot(CurSlot, VM->getFinishSlot(), Pred);
  addSlotEnable(CurSlot, VM->getPort(VASTModule::Finish).getSeqVal(), Pred);
}

void VerilogASTBuilder::emitOpRetVal(MachineInstr *MI, VASTSlot *Slot,
                                     VASTValueVecTy &Cnds) {
  unsigned retChannel = MI->getOperand(1).getImm();
  assert(retChannel == 0 && "Only support Channel 0!");
  VASTSeqOp *Op
    = VM->createSeqOp(Slot, Builder->buildAndExpr(Cnds, 1), 1, MI, true);
  Op->addSrc(getAsOperandImpl(MI->getOperand(0)), 0, false,
             VM->getRetPort().getSeqVal());
}

void VerilogASTBuilder::emitOpMemTrans(MachineInstr *MI, VASTSlot *Slot,
                                       VASTValueVecTy &Cnds) {
  unsigned FUNum = VInstrInfo::getPreboundFUId(MI).getFUNum();
  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);

  // Create the SeqOp for the load/store.
  VASTSeqOp *Op = VM->createSeqOp(Slot, Pred, 4, MI, true);

  // Emit Address.
  std::string RegName = VFUMemBus::getAddrBusName(FUNum) + "_r";
  VASTSeqValue *R = VM->getSymbol<VASTSeqValue>(RegName);
  Op->addSrc(getAsOperandImpl(MI->getOperand(1)), 0, false, R);
  // Assign store data.
  RegName = VFUMemBus::getOutDataBusName(FUNum) + "_r";
  R = VM->getSymbol<VASTSeqValue>(RegName);
  // Please note that the data are not present when we are performing a load.
  Op->addSrc(getAsOperandImpl(MI->getOperand(2)), 1, false, R);
  // And write enable.
  RegName = VFUMemBus::getCmdName(FUNum) + "_r";
  R = VM->getSymbol<VASTSeqValue>(RegName);
  Op->addSrc(getAsOperandImpl(MI->getOperand(3)), 2, false, R);
  // The byte enable.
  RegName = VFUMemBus::getByteEnableName(FUNum) + "_r";
  R = VM->getSymbol<VASTSeqValue>(RegName);
  Op->addSrc(getAsOperandImpl(MI->getOperand(4)), 3, false, R);

  // Remember we enabled the memory bus at this slot.
  std::string EnableName = VFUMemBus::getEnableName(FUNum) + "_r";
  VASTValPtr MemEn = VM->getSymbol(EnableName);
  addSlotEnable(Slot, cast<VASTSeqValue>(MemEn), Pred);
}

void VerilogASTBuilder::emitOpBRamTrans(MachineInstr *MI, VASTSlot *Slot,
                                        VASTValueVecTy &Cnds, bool IsWrite) {
  unsigned FUNum =
    VFUBRAM::FUNumToBRamNum(VInstrInfo::getPreboundFUId(MI).getFUNum());
  unsigned SizeInBytes = FInfo->getBRamInfo(FUNum).ElemSizeInBytes;
  unsigned Alignment = Log2_32_Ceil(SizeInBytes);

  VASTValPtr Addr = getAsOperandImpl(MI->getOperand(1));
  Addr = Builder->buildBitSliceExpr(Addr, Addr->getBitWidth(), Alignment);
  
  unsigned PortRegNum = MI->getOperand(0).getReg();

  VASTSeqValue *AddrPort = cast<VASTSeqValue>(lookupSignal(PortRegNum));
  // The block RAM maybe degraded.
  if (AddrPort->getValType() != VASTNode::BRAM) {
    assert(isa<VASTRegister>(AddrPort->getParent()) && "Bad VASTSeqVal Parent!");

    if (IsWrite) {
      VASTValPtr Data = getAsOperandImpl(MI->getOperand(2));
      addAssignment(AddrPort, Data, Slot, Cnds, MI);
    }

    return;
  }

  VASTValPtr Pred = Builder->buildAndExpr(Cnds, 1);
  VASTSeqOp *Op = VM->createSeqOp(Slot, Pred, IsWrite ? 2 : 1, MI, true);
  // DIRTY HACK: Because the Read address are also use as the data ouput port of
  // the block RAM, the block RAM read define its result at the address port.
  Op->addSrc(Addr, 0, !IsWrite, AddrPort);

  // Also assign the data to write to the dataport of the block RAM.
  if (IsWrite) {
    VASTValPtr Data = getAsOperandImpl(MI->getOperand(2));
    VASTSeqValue *DataPort = cast<VASTSeqValue>(lookupSignal(PortRegNum + 1));
    Op->addSrc(Data, 1, false, DataPort);
  }
}

MachineBasicBlock::iterator
VerilogASTBuilder::emitDatapath(MachineInstr *Bundle) {
  typedef MachineBasicBlock::instr_iterator instr_it;
  assert(Bundle->getOpcode() == VTM::Datapath
         && "Expect data-path bundle start!");

  instr_it I = Bundle;
  while ((++I)->isInsideBundle())
    Builder->createAndIndexExpr(I, true);

  return I;
}

static void printOperandImpl(raw_ostream &OS, const MachineOperand &MO,
                             unsigned UB = 64, unsigned LB = 0) {
  switch (MO.getType()) {
  case MachineOperand::MO_ExternalSymbol:
    UB = std::min(VInstrInfo::getBitWidth(MO), UB);
    OS << MO.getSymbolName();
    OS << VASTValue::printBitRange(UB, LB, VInstrInfo::getBitWidth(MO) != 1);
    return;
  case MachineOperand::MO_GlobalAddress:
    OS << "(`gv" << VBEMangle(MO.getGlobal()->getName());
    if (int64_t Offset = MO.getOffset())
      OS  << " + " << Offset;
    OS << ')';
    return;
  default: break;
  }

  MO.print(OS);
}

VASTValPtr VerilogASTBuilder::getAsOperandImpl(MachineOperand &Op,
                                               bool GetAsInlineOperand) {
  unsigned BitWidth = VInstrInfo::getBitWidth(Op);
  switch (Op.getType()) {
  case MachineOperand::MO_Register: {
    if (unsigned Reg = Op.getReg())
      if (VASTValPtr V = lookupSignal(Reg)) {
        // The operand may only use a sub bitslice of the signal.
        V = Builder->buildBitSliceExpr(V, BitWidth, 0);
        // Try to inline the operand.
        if (GetAsInlineOperand) V = V.getAsInlineOperand();
        return V;
      }
    return 0;
  }
  case MachineOperand::MO_Immediate:
    return VM->getOrCreateImmediateImpl(Op.getImm(), BitWidth);
  default: break;
  }

  // DirtyHack: simply create a symbol.
  std::string Name;
  raw_string_ostream SS(Name);
  printOperandImpl(SS, Op);
  SS.flush();

  bool NeedWrapper = false;
  unsigned SymbolWidth = 0;
  if (Op.isGlobal()) { // GlobalValues are addresses.
    if (Op.getGlobal()->getType()->getAddressSpace())
      // Not in generic address space, this is the base address of block rams.
      // The base address is 0 as we do not merge block ram at the moment.
      return VM->getOrCreateImmediateImpl(0 + Op.getOffset(), BitWidth);

    SymbolWidth = std::max(TD->getPointerSizeInBits(), BitWidth);
    NeedWrapper = true;
  }

  VASTValPtr Symbol = VM->getOrCreateSymbol(Name, SymbolWidth, NeedWrapper);
  if (SymbolWidth && GetAsInlineOperand)
    Symbol = Builder->buildBitSliceExpr(Symbol, BitWidth, 0).getAsInlineOperand();

  return Symbol;
}

void VerilogASTBuilder::printOperand(MachineOperand &Op, raw_ostream &OS) {
  if(Op.isReg() || Op.isImm()){
    VASTValPtr U = getAsOperandImpl(Op);
    U.printAsOperand(OS);
    //U.PinUser();
    return;
  }

  printOperandImpl(OS, Op);
}
