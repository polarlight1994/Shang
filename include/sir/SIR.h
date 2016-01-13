//===------------------- SIR.h - Modules in SIR ----------------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the SIR.
//
//===----------------------------------------------------------------------===//
#ifndef SIR_MODULE_H
#define SIR_MODULE_H

#include "llvm/IR/Value.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/ValueMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"

#include <iostream>
#include <set>

namespace llvm {
// Get the signed value of a ConstantInt.
static int64_t getConstantIntValue(Value *V) {
  ConstantInt *CI = dyn_cast<ConstantInt>(V);
  assert(CI && "Unexpected Value type!");

  APInt AI = CI->getValue();

  // If it is a one-bit value, then we don't need to
  // consider whether it is negative or not.
  if (AI.getBitWidth() == 1) return AI.getBoolValue();

  if (AI.isNonNegative()) return AI.getZExtValue();
  else {
    return 0 - (AI.abs().getZExtValue());
  }
}

// Construct a string of unsigned int in binary or hexadecimal.
static std::string buildLiteralUnsigned(uint64_t Value, unsigned bitwidth) {
  std::string ret;
  ret = utostr_32(bitwidth) + '\'';
  if (bitwidth == 1) ret += "b";
  else               ret += "h";
  // Mask the value that small than 4 bit to prevent printing something
  // like 1'hf out.
  if (bitwidth < 4) Value &= (1 << bitwidth) - 1;

  std::string ss = utohexstr(Value);
  unsigned int uselength = (bitwidth/4) + (((bitwidth&0x3) == 0) ? 0 : 1);
  if(uselength < ss.length())
    ss = ss.substr(ss.length() - uselength, uselength);
  ret += ss;

  return ret;
}

// Construct a string of corresponding FU name.
static std::string getFUName(IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  case Intrinsic::shang_add:  return "shang_addc";
  case Intrinsic::shang_addc: return "shang_addc";
  case Intrinsic::shang_mul:  return "shang_mult";
  case Intrinsic::shang_udiv: return "shang_udiv";
  case Intrinsic::shang_sdiv: return "shang_sdiv";
  case Intrinsic::shang_rand: return "shang_rand";
  case Intrinsic::shang_shl:  return "shang_shl";
  case Intrinsic::shang_lshr: return "shang_srl";
  case Intrinsic::shang_ashr: return "shang_sra";
  case Intrinsic::shang_ugt:  return "shang_ugt";
  case Intrinsic::shang_sgt:  return "shang_sgt";
  default: break;
  }

  return NULL;
}

// Construct a string with only characters, numbers and underlines.
static std::string Mangle(const std::string &S) {
  std::string Result;

  for (unsigned i = 0, e = S.size(); i != e; ++i) {
    if (isalnum(S[i]) || S[i] == '_') {
      Result += S[i];
    } else {
      Result += '_';
    }
  }

  return Result;
}

// Construct a string of BitRange like [31:0].
static std::string BitRange(unsigned UB, unsigned LB = 0, bool printOneBit = false) {
  std::string ret;
  assert(UB && UB > LB && "Bad bit range!");
  --UB;
  if (UB != LB)
    ret = "[" + utostr_32(UB) + ":" + utostr_32(LB) + "]";
  else if(printOneBit)
    ret = "[" + utostr_32(LB) + "]";

  return ret;
}

// Print the name of IR instruction.
static void printName(raw_ostream &OS, Instruction &I) {
  OS << Mangle(I.getName());
}

// Print the true form of the ConstantInt value in binary or hexadecimal.
static void printConstantIntValue(raw_ostream &OS, ConstantInt *CI) {
  OS << utostr_32(CI->getBitWidth()) << '\'';

  APInt AI = CI->getValue();

  if (CI->getBitWidth() <= 4)
    OS << "b" << AI.toString(2, false);
  else
    OS << "h" << AI.toString(16, false);
}
}

namespace llvm {
class SIRSlot;

class SIRSeqOp;
class SIRSlotTransition;

class SIRRegister;

class SIRPort;
class SIRInPort;
class SIROutPort;

class SIRSubModuleBase;
class SIRSubModule;
class SIRMemoryBank;

class SIR;
}

namespace llvm {
// Represent the registers in the Verilog.
class SIRRegister : public ilist_node<SIRRegister> {
public:
  enum SIRRegisterTypes {
    General,            // Common registers which hold data for data-path.
    PHI,                // Register hold value in PHINode.
    SlotReg,            // Register for slot.
    Pipeline,           // Register for pipeline of data-path.
    FUInput,						// Input register for FUnits like memory bank.
    FUOutput,           // Output register for FUnits like memory bank.
    OutPort,            // Register for OutPort of module.
  };

private:
  SIRRegisterTypes T;
  const uint64_t InitVal;
  BasicBlock *ParentBB;
  Value *LLVMValue;

  /// Each register contains a corresponding Mux to holds
  /// all assignment to it.
  typedef std::vector<Value *> FaninVector;
  FaninVector Fanins;

  typedef std::vector<Value *> FaninGuardVector;
  FaninGuardVector FaninGuards;

  // Also remember the slot of assignment.
  typedef std::vector<SIRSlot *> FaninSlotVector;
  FaninSlotVector FaninSlots;

  // After RegisterSynthesis, all assignments will be
  // synthesized into forms below:
  // RegVal = (Fanin1 & Guard1) | (Fanin2 & Guard2)...
  Value *RegVal;

  // When the RegGuard is true, the RegVal can be
  // assign to the register. After RegisterSynthesis,
  // all assignments will be synthesized into forms
  // below:
  // RegGuard = Guard1 | Guard2...
  Value *RegGuard;

  std::string Name;
  unsigned BitWidth;

public:
  // Construction for normal node & ilist node.
  SIRRegister(std::string Name = "", unsigned BitWidth = 0,
              uint64_t InitVal = 0, BasicBlock *ParentBB = NULL,
              SIRRegisterTypes T = SIRRegister::General,
              Value *LLVMValue = 0)
    : Name(Name), BitWidth(BitWidth), ParentBB(ParentBB),
    InitVal(InitVal), T(T), LLVMValue(LLVMValue) {}

  typedef FaninVector::const_iterator const_iterator;
  const_iterator fanin_begin() const { return Fanins.begin(); }
  const_iterator fanin_end() const { return Fanins.end(); }
  typedef FaninVector::iterator iterator;
  iterator fanin_begin() { return Fanins.begin(); }
  iterator fanin_end() { return Fanins.end(); }
  unsigned fanin_size() const { return Fanins.size(); }
  bool fanin_empty() const { return Fanins.empty(); }

  typedef FaninGuardVector::const_iterator const_guard_iterator;
  const_guard_iterator guard_begin() const { return FaninGuards.begin(); }
  const_guard_iterator guard_end() const { return FaninGuards.end(); }
  typedef FaninGuardVector::iterator guard_iterator;
  guard_iterator guard_begin() { return FaninGuards.begin(); }
  guard_iterator guard_end() { return FaninGuards.end(); }
  unsigned guard_size() const { return FaninGuards.size(); }
  bool guard_empty() const { return FaninGuards.empty(); }

  typedef FaninSlotVector::const_iterator const_faninslots_iterator;
  const_faninslots_iterator faninslots_begin() const { return FaninSlots.begin(); }
  const_faninslots_iterator faninslots_end() const { return FaninSlots.end(); }
  typedef FaninSlotVector::iterator faninslots_iterator;
  faninslots_iterator faninslots_begin() { return FaninSlots.begin(); }
  faninslots_iterator faninslots_end() { return FaninSlots.end(); }

  void setLLVMValue(Instruction *I) { LLVMValue = I; }
  Value *getLLVMValue() const { return LLVMValue; }

  void setRegName(std::string N) { Name = N; }

  void setParentBB(BasicBlock *BB) { ParentBB = BB; }
  BasicBlock *getParentBB() const { return ParentBB; }

  bool isGeneral() { return T == SIRRegister::General; }
  bool isPHI() { return T == SIRRegister::PHI; }
  bool isSlot() { return T == SIRRegister::SlotReg; }
  bool isPipeline() { return T == SIRRegister::Pipeline; }
  bool isOutPort() { return T == SIRRegister::OutPort; }
  bool isFUInput() { return T == SIRRegister::FUInput; }
  bool isFUOutput() { return T == SIRRegister::FUOutput; }
  bool isFUInOut() { return isFUInput() || isFUOutput(); }

  std::string getName() const { return Name; }
  unsigned getBitWidth() const { return BitWidth; }
  const uint64_t getInitVal() const { return InitVal; }
  SIRRegisterTypes getRegisterType() const { return T; }
  Value *getRegVal() const { return RegVal; }
  Value *getRegGuard() const { return RegGuard; }
  void addAssignment(Value *Fanin, Value *FaninGuard);
  void addAssignment(Value *Fanin, Value *FaninGuard, SIRSlot *S);
  void setMux(Value *V, Value *G) {
    RegVal = V; RegGuard = G;

    // Set the real operands to this register assign instruction.
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(getLLVMValue());
    if (II && II->getIntrinsicID() == Intrinsic::shang_reg_assign) {
      Value *RegSrcVal = II->getOperand(0);
      Value *RegGuardVal = II->getOperand(1);
      if (RegSrcVal != RegVal) {
        II->setOperand(0, RegVal);
      }
      if (RegGuardVal != RegGuard)
        II->setOperand(1, RegGuard);
    }
  }
  void dropMux() {
    RegVal = NULL; RegGuard = NULL;

    Fanins.clear();
    FaninGuards.clear();
    FaninSlots.clear();

    assert(Fanins.size() == 0 && "Fanins not cleared!");
    assert(FaninGuards.size() == 0 && "FaininGuards not cleared!");
    assert(FaninSlots.size() == 0 && "FaninSlots not cleared!");
  }

  // Declare the register and assign the initial value.
  void printDecl(raw_ostream &OS) const;
  // Declare the register as Ports of the module.
  // This method is only called in Co-Simulation.
  void printVirtualPortDecl(raw_ostream &OS, bool IsInput) const;
};

// Represent the ports in the Verilog.
class SIRPort {
public:
  enum SIRPortTypes {
    Clk,
    Rst,
    Start,
    ArgPort,
    InPort = ArgPort,
    Finish,
    RetPort,
    OutPort = RetPort
  };

private:
  unsigned BitWidth;
  const std::string Name;
  SIRPortTypes T;

public:
  SIRPort(SIRPortTypes T, unsigned BitWidth, const std::string Name)
    : T(T), BitWidth(BitWidth), Name(Name) {}
  ~SIRPort();

  const std::string getName() const { return Name; }
  unsigned getBitWidth() const { return BitWidth; }
  SIRPortTypes getPortType() const { return T; }
  bool isInput() const { return T <= InPort; }

  // Print the port
  void printDecl(raw_ostream &OS) const;
};

// Represent the In-Port in Verilog.
class SIRInPort : public SIRPort {
private:
  Argument *V;

public:
  SIRInPort(SIRPort::SIRPortTypes T, unsigned BitWidth,
            const std::string Name, LLVMContext &C)
    : SIRPort(T, BitWidth, Name) {
    // Hack: Do we need to provide the ParentFunction to it?
    V = new Argument(Type::getInt1Ty(C), Name);
  };

  Value *getValue() { return V; }

  // Methods for support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const SIRInPort *A) { return true; }
  static inline bool classof(const SIRPort *A) {
    return (A->isInput());
  }
};

// Represent the Out-Port in Verilog.
class SIROutPort : public SIRPort {
private:
  SIRRegister *Reg;

public:
  SIROutPort(SIRPort::SIRPortTypes T, SIRRegister *Reg, 
             unsigned BitWidth, const std::string Name)
    : Reg(Reg), SIRPort(T, BitWidth, Name){}

  SIRRegister *getRegister() const { return Reg; }

  // Methods for support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const SIROutPort *A) { return true; }
  static inline bool classof(const SIRPort *A) {
    return !(A->isInput());
  }
};
}

namespace llvm {
class SIRSubModuleBase : public ilist_node<SIRSubModuleBase> {
public:
  enum Type {
    SubModule,
    MemoryBank
  };

private:
  SmallVector<SIRRegister *, 8> Fanins;
  SmallVector<SIRRegister *, 8> Fanouts;

protected:
  // The Idx of all sub-modules in SIR.
  const unsigned Idx;
  const Type Ty;

  SIRSubModuleBase(Type Ty, unsigned Idx)
    : Ty(Ty), Idx(Idx) {}

public:
  ~SIRSubModuleBase() {}

  void addFanin(SIRRegister *Fanin);
  void addFanout(SIRRegister *Fanout);

  Type getType() const { return Ty; }
  unsigned getNum() const { return Idx; }

  typedef SmallVectorImpl<SIRRegister *>::iterator fanin_iterator;
  fanin_iterator fanin_begin() { return Fanins.begin(); }
  fanin_iterator fanin_end() { return Fanins.end(); }

  typedef SmallVectorImpl<SIRRegister *>::const_iterator const_fanin_iterator;
  const_fanin_iterator fanin_begin() const { return Fanins.begin(); }
  const_fanin_iterator fanin_end()   const { return Fanins.end(); }

  SIRRegister *getFanin(unsigned Idx) const { return Fanins[Idx]; }
  SIRRegister *getFanout(unsigned Idx) const { return Fanouts[Idx]; }
};

class SIRSubModule : public SIRSubModuleBase {
  // Special ports in the sub-module.
  SIRRegister *StartPort, *FinPort, *RetPort;

  // The latency of the sub-module.
  unsigned Latency;

  SIRSubModule(unsigned FUNum)
    : SIRSubModuleBase(SubModule, FUNum), StartPort(0),
    FinPort(0), RetPort(0), Latency(0) {}

public:
  SIRRegister *getStartPort() const { return StartPort; }
  SIRRegister *getFinPort() const { return FinPort; }
  SIRRegister *getRetPort() const { return RetPort; }

  unsigned getLatency() const { return Latency; }
  std::string getPortName(const Twine &PortName) const;

  void printDecl(raw_ostream &OS) const;

};

class SIRMemoryBank : public SIRSubModuleBase {
  const unsigned AddrSize, DataSize;
  const unsigned ReadLatency;
  const bool RequireByteEnable;
  const bool IsReadOnly;
  // For each MemoryBank, we have two input port
  // including Address and WData.
  static const unsigned InputsPerPort = 2;
  // The address of last byte.
  unsigned EndByteAddr;

  // The map between GVs and its offset in memory bank
  typedef std::map<GlobalVariable *, unsigned> BaseAddrMap;
  BaseAddrMap	BaseAddrs;
  // The map between GVs and its OriginalPtrSize
  // for GetElementPtrInst in memory bank
  typedef std::map<GlobalVariable *, unsigned> GVs2PtrSizeMap;
  GVs2PtrSizeMap GVs2PtrSize;

  typedef BaseAddrMap::iterator baseaddrs_iterator;
  typedef BaseAddrMap::const_iterator const_baseaddrs_iterator;

  typedef GVs2PtrSizeMap::iterator gvs2ptrsize_iterator;
  typedef GVs2PtrSizeMap::const_iterator const_gvs2ptrsize_iterator;

public:
  SIRMemoryBank(unsigned BusNum, unsigned AddrSize, unsigned DataSize,
                bool RequireByteEnable, bool IsReadOnly, unsigned ReadLatency);

  unsigned getDataWidth() const { return DataSize; }
  unsigned getAddrWidth() const { return AddrSize; }
  unsigned getReadLatency() const { return ReadLatency; }
  unsigned getEndByteAddr() const { return EndByteAddr; }
  unsigned getByteEnWidth() const { return DataSize / 8; }
  unsigned getByteAddrWidth() const;

  bool requireByteEnable() const { return RequireByteEnable; }
  bool isReadOnly() const { return IsReadOnly; }

  // Signal names of the memory bank.
  std::string getAddrName() const;
  std::string getRDataName() const;
  std::string getWDataName() const;
  std::string getEnableName() const;
  std::string getWriteEnName() const;
  std::string getByteEnName() const;
  std::string getArrayName() const;

  SIRRegister *getAddr() const;
  SIRRegister *getRData() const;
  SIRRegister *getWData() const;
  SIRRegister *getEnable() const;
  SIRRegister *getWriteEn() const;
  SIRRegister *getByteEn() const;

  void addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes);
  unsigned getOffset(GlobalVariable *GV) const;

  bool indexGV2OriginalPtrSize(GlobalVariable *GV, unsigned PtrSize);
  unsigned lookupPtrSize(GlobalVariable *GV) const {
    const_gvs2ptrsize_iterator at = GVs2PtrSize.find(GV);
    return at == GVs2PtrSize.end() ? 0 : at->second;
  }

  baseaddrs_iterator baseaddrs_begin() { return BaseAddrs.begin(); }
  baseaddrs_iterator baseaddrs_end() { return BaseAddrs.end(); }

  const_baseaddrs_iterator const_baseaddrs_begin() const { return BaseAddrs.begin(); }
  const_baseaddrs_iterator const_baseaddrs_end() const { return BaseAddrs.end(); }

  gvs2ptrsize_iterator gvs2ptrsize_begin() { return GVs2PtrSize.begin(); }
  gvs2ptrsize_iterator gvs2ptrsize_end() { return GVs2PtrSize.end(); }

  const_gvs2ptrsize_iterator const_gvs2ptrsize_begin() const { return GVs2PtrSize.begin(); }
  const_gvs2ptrsize_iterator const_gvs2ptrsize_end() const { return GVs2PtrSize.end(); }

  // Declare all the regs used in memory bank.
  void printDecl(raw_ostream &OS) const;
  // Declare all the regs used in memory bank as Ports of module.
  // This method is only called in Co-Simulation.
  void printVirtualPortDecl(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast;
  static inline bool classof(const SIRMemoryBank *SMB) { return true; }
  static inline bool classof(const SIRSubModuleBase *SSMB) {
    return SSMB->getType() == MemoryBank;
  }
};
}

namespace llvm {
// Represent the state in the state-transition graph.
class SIRSlot : public ilist_node<SIRSlot> {
public:
// The types of the edges in the STG, the number representing the timing distance
// of the edge, only the successor edge represents a real state transition
// which have a timing distance of 1.
enum EdgeType {
    Sucessor = 0
};

  // The pointer to successor which is also encoded with the distance and the condition.
struct EdgePtr {
private:
  SIRSlot *S;
  EdgeType Ty;
  Value *Cnd;

  // Hide the function getInt from PointerIntPair.
  void getInt() const { }

public:
  EdgePtr(SIRSlot *S, EdgeType Ty, Value *Cnd)
    : S(S), Ty(Ty), Cnd(Cnd) {}

  operator SIRSlot *() const { return S; }
  SIRSlot *operator->() const { return S; }

  SIRSlot *getSlot() const { return S; }
  EdgeType getType() const { return Ty; }
  unsigned getDistance() const {
    return Ty == Sucessor ? 1 : 0;
  }
  Value *getCnd() const { return Cnd; }
};

  typedef SmallVector<EdgePtr, 4> SuccVecTy;
  typedef SuccVecTy::iterator succ_iterator;
  typedef SuccVecTy::const_iterator const_succ_iterator;

  typedef SmallVector<EdgePtr, 4> PredVecTy;
  typedef PredVecTy::iterator pred_iterator;
  typedef PredVecTy::const_iterator const_pred_iterator;

private:
  BasicBlock *ParentBB;

  // The SeqOps in the current slot
  typedef std::vector<SIRSeqOp *> OpVector;
  OpVector Operations;

  // The link to other slots
  PredVecTy PredSlots;
  SuccVecTy NextSlots;

  SIRRegister *SlotReg;

  // The creating order which is not equal to
  // its real schedule step in FSM.
  unsigned SlotNum;
  // The schedule step in local BB.
  unsigned Step;

public:
  // Construction for ilist node.
  SIRSlot(): SlotNum(0), ParentBB(0), SlotReg(0), Step(0) {}
  // Construction for normal Slot.
  SIRSlot(unsigned SlotNum, BasicBlock *ParentBB,
          Value *SlotGuard, SIRRegister *Reg, unsigned Step)
    : SlotNum(SlotNum), ParentBB(ParentBB), SlotReg(Reg), Step(Step) {}

  unsigned getSlotNum() const { return SlotNum; }
  unsigned getStepInLocalBB() const { return Step; }
  BasicBlock *getParent() const { return ParentBB; }
  SIRRegister *getSlotReg() const { return SlotReg; }
  Value *getGuardValue() const { return getSlotReg()->getLLVMValue(); }

  void resetStep(unsigned S) { Step = S; }

  // If the SrcSlot already has this NextSlot as successor.
  bool hasNextSlot(SIRSlot *NextSlot);

  void addSeqOp(SIRSeqOp *Op) { Operations.push_back(Op); }
  void addSuccSlot(SIRSlot *NextSlot, EdgeType T, Value *Cnd);

  void unlinkSuccs();
  void unlinkSucc(SIRSlot *S);

  typedef OpVector::const_iterator const_op_iterator;
  const_op_iterator op_begin() const { return Operations.begin(); }
  const_op_iterator op_end() const { return Operations.end(); }
  typedef OpVector::iterator op_iterator;
  op_iterator op_begin() { return Operations.begin(); }
  op_iterator op_end() { return Operations.end(); }

  // Successor slots of this slot.
  succ_iterator succ_begin() { return NextSlots.begin(); }
  const_succ_iterator succ_begin() const { return NextSlots.begin(); }
  succ_iterator succ_end() { return NextSlots.end(); }
  const_succ_iterator succ_end() const { return NextSlots.end(); }
  bool succ_empty() const { return NextSlots.empty(); }
  unsigned succ_size() const { return NextSlots.size(); }

  // Predecessor slots of this slot.
  pred_iterator pred_begin() { return PredSlots.begin(); }
  pred_iterator pred_end() { return PredSlots.end(); }
  const_pred_iterator pred_begin() const { return PredSlots.begin(); }
  const_pred_iterator pred_end() const { return PredSlots.end(); }
  unsigned pred_size() const { return PredSlots.size(); }

  // Reset the SlotNum and Name.
  void resetNum(unsigned Num);

  // Functions for debug
  void print(raw_ostream &OS) const;
  void dump() const;
}; 

template<> struct GraphTraits<SIRSlot *> {
  typedef SIRSlot NodeType;
  typedef NodeType::succ_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->succ_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->succ_end();
  }
};

template<> struct GraphTraits<const SIRSlot *> {
  typedef const SIRSlot NodeType;
  typedef NodeType::const_succ_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->succ_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->succ_end();
  }
};

// Represent the assign operation in SIR.
class SIRSeqOp : public ilist_node<SIRSeqOp> {
public:
  enum Type {
    General,
    SlotTransition
  };

private:
  const Type T;
  Value *Src;
  SIRRegister *DstReg;
  Value *Guard;
  SIRSlot *S;

public:
  // Constructors used for the ilist_node.
  SIRSeqOp() : Src(0), DstReg(0), Guard(0), S(0), T(General) {}
  // Default Constructor.
  SIRSeqOp(Value *Src, SIRRegister *DstReg, Value *Guard,
           SIRSlot *S, Type T = General)
    : Src(Src), DstReg(DstReg), Guard(Guard), S(S), T(T) {}

  Value *getLLVMValue() const { return DstReg->getLLVMValue(); }
  Value *getSrc() const { return Src; }
  Value *getGuard() const { return Guard; }
  SIRSlot *getSlot() const { return S; }
  SIRRegister *getDst() const { return DstReg; }
  Type getType() const { return T; }

  bool isSlotTransition() const { return T == SlotTransition; }

  void setSlot(SIRSlot *Slot) { S = Slot; }
  void setSrc(Value *V) { Src = V; }
  void setGuard(Value *V) { Guard = V; }

  // Functions for debug
  void print(raw_ostream &OS) const;
  void dump() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const SIRSeqOp *SeqOp) { return true; }
  static inline bool classof(const SIRSlotTransition *SST) { return true; }
};

// Represent the state transition in SIR.
// Note that this is also assign operation
// to the SlotReg indeed.
class SIRSlotTransition : public SIRSeqOp {
private:
  SIRSlot *DstSlot;

public:
  // Dirty Hack: the Src Value here should be limited as 1'b1.
  SIRSlotTransition(Value *Src, SIRSlot *SrcSlot, SIRSlot *DstSlot, Value *Cnd)
    : SIRSeqOp(Src, DstSlot->getSlotReg(), Cnd, SrcSlot, SIRSeqOp::SlotTransition),
    DstSlot(DstSlot) {}

  SIRSlot *getSrcSlot() const { return getSlot(); }
  SIRSlot *getDstSlot() const { return DstSlot; }
  Value *getCnd() const { return getGuard(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const SIRSlotTransition *SST) { return true; }
  static inline bool classof(const SIRSeqOp *SeqOp) {
    return SeqOp->getType() == SlotTransition;
  }
};
}

namespace llvm {
// The module in Shang IR.
class SIR {
public:
  typedef SmallVector<SIRPort *, 8> SIRPortVector;
  typedef SIRPortVector::iterator port_iterator;
  typedef SIRPortVector::const_iterator const_port_iterator;

  typedef ilist<SIRRegister> RegisterList;
  typedef RegisterList::iterator register_iterator;
  typedef RegisterList::const_iterator const_register_iterator;

  typedef SmallVector<SIRSubModuleBase *, 8> SubModuleBaseVector;
  typedef SubModuleBaseVector::iterator submodulebase_iterator;
  typedef SubModuleBaseVector::const_iterator const_submodulebase_iterator;

  typedef SmallVector<Instruction *, 8> DataPathInstVector;
  typedef DataPathInstVector::iterator datapathinst_iterator;
  typedef DataPathInstVector::const_iterator const_datapathinst_iterator;

  typedef ilist<SIRSlot> SlotList;
  typedef SlotList::iterator slot_iterator;
  typedef SlotList::const_iterator const_slot_iterator;

  typedef ilist<SIRSeqOp> SeqOpList;
  typedef SeqOpList::iterator seqop_iterator;
  typedef SeqOpList::const_iterator const_seqop_iterator;

  typedef std::map<Value *, SmallVector<SIRSeqOp *, 4> > MemInst2SeqOpsMapTy;
  typedef MemInst2SeqOpsMapTy::iterator meminst2seqops_iterator;
  typedef MemInst2SeqOpsMapTy::const_iterator const_meminst2seqops_iterator;

  typedef DenseMap<Value *, Value *> Val2SeqValMapTy;
  typedef Val2SeqValMapTy::iterator val2valseq_iterator;
  typedef Val2SeqValMapTy::const_iterator const_val2valseq_iterator;

  typedef DenseMap<Value *, SIRRegister *> SeqVal2RegMapTy;
  typedef SeqVal2RegMapTy::iterator seqval2reg_iterator;
  typedef SeqVal2RegMapTy::const_iterator const_seqval2reg_iterator;

  typedef DenseMap<SIRRegister *, SIRSlot *> Reg2SlotMapTy;
  typedef Reg2SlotMapTy::iterator reg2slot_iterator;
  typedef Reg2SlotMapTy::const_iterator const_reg2slot_iterator;

  typedef std::map<BasicBlock *, unsigned> PipelinedBB2MIIMapTy;
  typedef PipelinedBB2MIIMapTy::iterator pipelinedbb2mii_iterator;
  typedef PipelinedBB2MIIMapTy::const_iterator const_pipelinedbb2mii_iterator;

private:
  // Input/Output ports of the module
  SIRPortVector Ports;
  // Registers in the module
  RegisterList Registers;
  // SubModules in the module
  SubModuleBaseVector SubModuleBases;
  // The DataPathInsts in the module
  DataPathInstVector DataPathInsts;
  // The Slots in CtrlRgn of the module
  SlotList Slots;
  // The SeqOps in CtrlRgn of the module
  SeqOpList SeqOps;
  // The map between MemInst and corresponding SeqOps in SIR
  MemInst2SeqOpsMapTy MemInst2SeqOps;
  // The map between Value in LLVM IR and SeqVal in SIR
  Val2SeqValMapTy Val2SeqVal;
  // The map between SeqVal in SIR and Reg in SIR
  SeqVal2RegMapTy SeqVal2Reg;
  // The map between Reg and SIRSlot
  Reg2SlotMapTy Reg2Slot;
  // The map between Pipelined BB and its MII
  PipelinedBB2MIIMapTy PipelinedBB2MII;

  // Registers that should be kept.
  std::set<SIRRegister *> KeepRegs;

  // Record the Idx of FinPort and RetPort.
  unsigned RetPortIdx;
  unsigned FinPortIdx;
  // Record the landing slot and the latest slot of BB.
  typedef std::pair<SIRSlot *, SIRSlot *> slot_pair;
  std::map<BasicBlock *, slot_pair> BB2SlotMap;

protected:
  Function *F;
  // Use LLVMContext to create Type and ConstantInt Value.
  LLVMContext &C;

public:
  SIR(Function *F) : F(F), C(F->getContext()) {}
  ~SIR() {
    Ports.clear();
    Registers.clear();
    SubModuleBases.clear();
    DataPathInsts.clear();
    Slots.clear();
    SeqOps.clear();
    MemInst2SeqOps.clear();
    Val2SeqVal.clear();
    SeqVal2Reg.clear();
    Reg2Slot.clear();
    PipelinedBB2MII.clear();
    KeepRegs.clear();
    BB2SlotMap.clear();
  }

  bool hasKeepReg(SIRRegister *Reg) const { return KeepRegs.count(Reg); }
  void indexKeepReg(SIRRegister *Reg) {
    if (!KeepRegs.count(Reg))
      KeepRegs.insert(Reg);
  }

  Function *getFunction() const { return F; }
  Module *getModule() const { return F->getParent(); }
  LLVMContext &getContext() const { return C; }

  port_iterator ports_begin() { return Ports.begin(); }
  port_iterator ports_end() { return Ports.end(); }

  const_port_iterator const_ports_begin() const { return Ports.begin(); }  
  const_port_iterator const_ports_end() const { return Ports.end(); }

  register_iterator registers_begin() { return Registers.begin(); }
  register_iterator registers_end() { return Registers.end(); }

  const_register_iterator const_registers_begin() const { return Registers.begin(); }
  const_register_iterator const_registers_end() const { return Registers.end(); }

  submodulebase_iterator submodules_begin() { return SubModuleBases.begin(); }
  submodulebase_iterator submodules_end() { return SubModuleBases.end(); }

  const_submodulebase_iterator const_submodules_begin() const { return SubModuleBases.begin(); }
  const_submodulebase_iterator const_submodules_end() const { return SubModuleBases.end(); }

  slot_iterator slot_begin() { return Slots.begin(); }
  slot_iterator slot_end() { return Slots.end(); }

  const_slot_iterator const_slot_begin() const { return Slots.begin(); }
  const_slot_iterator const_slot_end() const { return Slots.end(); }

  datapathinst_iterator datapathinst_begin() { return DataPathInsts.begin(); }
  datapathinst_iterator datapathinst_end() { return DataPathInsts.end(); }

  const_datapathinst_iterator const_datapathinst_begin() { return DataPathInsts.begin(); }
  const_datapathinst_iterator const_datapathinst_end() { return DataPathInsts.end(); }

  seqop_iterator seqop_begin() { return SeqOps.begin(); }
  seqop_iterator seqop_end() { return SeqOps.end(); }

  const_seqop_iterator const_seqop_begin() { return SeqOps.begin(); }
  const_seqop_iterator const_seqop_end() { return SeqOps.end(); }

  unsigned getSlotsSize() const { return Slots.size(); }
  unsigned getPortsSize() const { return Ports.size(); }

  void IndexSlot(SIRSlot *Slot) {
    Slots.push_back(Slot);
  }
  void IndexPort(SIRPort *Port) {
    Ports.push_back(Port);
  }
  void IndexSeqOp(SIRSeqOp *SeqOp) {
    SeqOps.push_back(SeqOp);
  }
  void IndexRegister(SIRRegister *Reg) {
    Registers.push_back(Reg);
  }
  void IndexSubModuleBase(SIRSubModuleBase *SMB) {
    SubModuleBases.push_back(SMB);
  }
  void IndexDataPathInst(Instruction *DataPathInst) {
    DataPathInsts.push_back(DataPathInst);
  }

  void deleteUselessSlot(SIRSlot *S) {
    typedef SIRSlot::op_iterator iterator;
    for (iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      SIRSeqOp *SeqOp = *I;

      if (isa<SIRSlotTransition>(SeqOp)) {
        bool hasSeqOp = false;
        for (seqop_iterator SI = seqop_begin(), SE = seqop_end(); SI != SE; ++SI) {
          SIRSeqOp *seqop = SI;

          if(SeqOp == seqop)
            hasSeqOp = true;
        }

        if (hasSeqOp)
          deleteUselessSeqOp(SeqOp);
      }
    }

    Registers.remove(S->getSlotReg());

    Slots.remove(S);
  }
  void deleteUselessSeqOp(SIRSeqOp *SeqOp) {
    SeqOps.remove(SeqOp);
  }

  void clearSeqOps() {
    for (seqop_iterator SI = seqop_begin(), SE = seqop_end(); SI != SE;) {
      SIRSeqOp *SeqOp = SI++;

      SeqOps.remove(SeqOp);
    }

    assert(SeqOps.size() == 0 && "Not cleared!");
  }

  bool IndexMemInst2SeqOps(Value *MemInst, SmallVector<SIRSeqOp *, 4> MemSeqOps) {
    assert(isa<LoadInst>(MemInst) || isa<StoreInst>(MemInst)
           && "Unexpected Value Type!");

    return MemInst2SeqOps.insert(std::make_pair(MemInst, MemSeqOps)).second;
  }
  ArrayRef<SIRSeqOp *> lookupMemSeqOps(Value *MemInst) const {
    MemInst2SeqOpsMapTy::const_iterator at = MemInst2SeqOps.find(MemInst);

    if (at == MemInst2SeqOps.end())
      return ArrayRef<SIRSeqOp *>();

    return at->second;
  }

  bool IndexVal2SeqVal(Value *Val, Value *SeqVal) {
    // The SeqVal should be the form of shang_reg_assign.
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(SeqVal);
    assert(II && II->getIntrinsicID() == Intrinsic::shang_reg_assign
           && "Unexpected SeqVal type!");

    return Val2SeqVal.insert(std::make_pair(Val, SeqVal)).second;
  }
  Value *lookupSeqVal(Value *Val) const {
    const_val2valseq_iterator at = Val2SeqVal.find(Val);
    return at == Val2SeqVal.end() ? 0 : at->second;
  }

  bool IndexSeqVal2Reg(Value *SeqVal, SIRRegister *Reg) {
    // The SeqVal should be the form of shang_reg_assign.
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(SeqVal);
    assert(II && II->getIntrinsicID() == Intrinsic::shang_reg_assign
           && "Unexpected SeqVal type!");

    return SeqVal2Reg.insert(std::make_pair(SeqVal, Reg)).second;
  }
  SIRRegister *lookupSIRReg(Value *SeqVal) const {
    const_seqval2reg_iterator at = SeqVal2Reg.find(SeqVal);
    return at == SeqVal2Reg.end() ? 0 : at->second;
  }

  bool IndexReg2Slot(SIRRegister *Reg, SIRSlot *S) {
    return Reg2Slot.insert(std::make_pair(Reg, S)).second;
  }
  SIRSlot *lookupSIRSlot(SIRRegister *Reg) const {
    const_reg2slot_iterator at = Reg2Slot.find(Reg);
    return at == Reg2Slot.end() ? 0 : at->second;
  }

  bool IndexPipelinedBB2MII(BasicBlock *PipelinedBB, unsigned MII) {
    return PipelinedBB2MII.insert(std::make_pair(PipelinedBB, MII)).second;
  }
  unsigned getMIIOfPipelinedBB(BasicBlock *PipelinedBB) const {
    const_pipelinedbb2mii_iterator at = PipelinedBB2MII.find(PipelinedBB);
    return at == PipelinedBB2MII.end() ? 0 : at->second;
  }
  bool IsBBPipelined(BasicBlock *BB) {
    return PipelinedBB2MII.count(BB);
  }

  void setRetPortIdx(unsigned Idx) { RetPortIdx = Idx; }
  void setFinPortIdx(unsigned Idx) { FinPortIdx = Idx; }
  unsigned getRetPortIdx() const { return RetPortIdx; }
  unsigned getFinPortIdx() const { return FinPortIdx; }

  bool IndexBB2Slots(BasicBlock *BB, SIRSlot *LandingSlot,
                     SIRSlot *LatestSlot) {
    std::map<BasicBlock*, slot_pair>::iterator at = BB2SlotMap.find(BB);
    // If there are already a map between BB and Landing/Latest Slot,
    // then we update it.
    if (at != BB2SlotMap.end()) {
      BB2SlotMap.erase(at);
    }

    return BB2SlotMap.insert(std::make_pair(BB,
                             std::make_pair(LandingSlot, LatestSlot))).second;
  }
  std::map<BasicBlock *, slot_pair> getBB2SlotMap() {
    return BB2SlotMap;
  }
  slot_pair getSlotsOfBB(BasicBlock *BB) {
    std::map<BasicBlock*, slot_pair>::const_iterator
      at = BB2SlotMap.find(BB);
    //assert(at != BB2SlotMap.end() && "Slots not found!");
    return at->second;
  }
  SIRSlot *getLandingSlot(BasicBlock *BB) const {
    std::map<BasicBlock*, slot_pair>::const_iterator
      at = BB2SlotMap.find(BB);
    assert(at != BB2SlotMap.end() && "Landing slot not found!");
    return at->second.first;
  }
  SIRSlot *getLatestSlot(BasicBlock *BB) const {
    std::map<BasicBlock*, slot_pair>::const_iterator
      at = BB2SlotMap.find(BB);
    assert(at != BB2SlotMap.end() && "Latest slot not found!");
    return at->second.second;
  }

  SIRPort *getPort(unsigned i) const {
    assert(i < Ports.size() && "Out of range!");
    return Ports[i];
  }
  SIRPort *getRetPort() const { return getPort(RetPortIdx); }
  SIRPort *getFinPort() const { return getPort(FinPortIdx); }

  // Give the position just in front of the terminator instruction
  // at back of the module. And all DataPath instruction created for
  // registers will be inserted here to avoid being used before declaration.
  Value *getPositionAtBackOfModule() const {
    BasicBlock *LastBB = &getFunction()->getBasicBlockList().back();
    return LastBB->getTerminator();
  }

  // -------------------Functions to generate Verilog-------------------- //

  // Print the declaration of module.
  void printModuleDecl(raw_ostream &OS) const;
  // Print the declaration of register.
  void printRegDecl(raw_ostream &OS) const;
  // Print the declaration of MemoryBank.
  void printMemoryBankDecl(raw_ostream &OS) const;

  // ---------------------------Other Functions---------------------------- //

  // Release the dead objects in SIR.
  bool gcImpl();
  bool gc();

  // Functions for debug
  void print(raw_ostream &OS);
  void dump(raw_ostream &OS);
  void dumpIR(raw_ostream &OS);
  void dumpBB2Slot(raw_ostream &OS);
  void dumpReg2Slot(raw_ostream &OS);
  void dumpSeqOp2Slot(raw_ostream &OS);
};
}

#endif