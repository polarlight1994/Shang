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

namespace llvm {
	static uint64_t getConstantIntValue(Value *V) {
		ConstantInt *CI = dyn_cast<ConstantInt>(V);
		assert(CI && "Unexpected Value type!");

		APInt AI = CI->getValue();
		if (AI.isNonNegative()) return AI.getZExtValue();
		else {
			return 0 - (AI.abs().getZExtValue());
		}
	}

  static std::string buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue) {
    std::string ret;
    ret = utostr_32(bitwidth) + '\'';
    if (bitwidth == 1) ret += "b";
    else               ret += "h";
    // Mask the value that small than 4 bit to prevent printing something
    // like 1'hf out.
    if (bitwidth < 4) Value &= (1 << bitwidth) - 1;

    if(isMinValue) {
      ret += utohexstr(Value);
      return ret;
    }

    std::string ss = utohexstr(Value);
    unsigned int uselength = (bitwidth/4) + (((bitwidth&0x3) == 0) ? 0 : 1);
    if(uselength < ss.length())
      ss = ss.substr(ss.length() - uselength, uselength);
    ret += ss;

    return ret;
  }

  static std::string getFUName(IntrinsicInst &I) {
    switch (I.getIntrinsicID()) {
    case Intrinsic::shang_add:  return "shang_addc";
    case Intrinsic::shang_mul:  return "shang_mult";
    case Intrinsic::shang_rand: return "shang_rand";
    case Intrinsic::shang_shl:  return "shang_shl";
    case Intrinsic::shang_lshr: return "shang_srl";
    case Intrinsic::shang_ashr: return "shang_ashr";
    case Intrinsic::shang_ugt:  return "shang_ugt";
    case Intrinsic::shang_sgt:  return "shang_sgt";
    default: break;
    }

    return NULL;
  }

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

  static void printName(raw_ostream &OS, Instruction &I) {
    OS << Mangle(I.getName());
  }
}

namespace llvm {
// Represent the registers in the Verilog.
class SIRRegister {
public:
  enum SIRRegisterTypes {
    General,            // Common registers which hold data for data-path.
    SlotReg,            // Register for slot.
    OutPort,            // Register for OutPort of module.
  };

private:
  SIRRegisterTypes T;
  const uint64_t InitVal;
	BasicBlock *ParentBB;
	Value *LLVMValue;

	/// Each register contains a corresponding Mux to holds
	/// all assignment to it.
	// Map the transaction condition to transaction value.
	typedef std::vector<Value *> FaninVector;
	FaninVector Fanins;

	typedef std::vector<Value *> FaninGuardVector;
	FaninGuardVector FaninGuards;

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

	const std::string Name;
	unsigned BitWidth;

public:
  SIRRegister(std::string Name = "", unsigned BitWidth = 0,
		          uint64_t InitVal = 0, BasicBlock *ParentBB = NULL,
              SIRRegisterTypes T = SIRRegister::General,
              Value *LLVMValue = 0)
    : Name(Name), BitWidth(BitWidth), ParentBB(ParentBB),
		InitVal(InitVal), T(T), LLVMValue(LLVMValue) {}

	typedef FaninVector::const_iterator const_iterator;
	const_iterator assign_begin() const { return Fanins.begin(); }
	const_iterator assign_end() const { return Fanins.end(); }
	typedef FaninVector::iterator iterator;
	iterator assign_begin() { return Fanins.begin(); }
	iterator assign_end() { return Fanins.end(); }
	unsigned assign_size() const { return Fanins.size(); }
	bool assign_empty() const { return Fanins.empty(); }

	typedef FaninGuardVector::const_iterator const_guard_iterator;
	const_guard_iterator guard_begin() const { return FaninGuards.begin(); }
	const_guard_iterator guard_end() const { return FaninGuards.end(); }
	typedef FaninGuardVector::iterator guard_iterator;
	guard_iterator guard_begin() { return FaninGuards.begin(); }
	guard_iterator guard_end() { return FaninGuards.end(); }
	unsigned guard_size() const { return FaninGuards.size(); }
	bool guard_empty() const { return FaninGuards.empty(); }

  void setLLVMValue(Instruction *I) { LLVMValue = I; }
  Value *getLLVMValue() const { return LLVMValue; }

	void setParentBB(BasicBlock *BB) { ParentBB = BB; }
	BasicBlock *getParentBB() const { return ParentBB; }

  bool isGeneral() { return T == SIRRegister::General; }
  bool isSlot() { return T == SIRRegister::SlotReg; }
  bool isOutPort() { return T == SIRRegister::OutPort; }
  
  std::string getName() const { return Name; }
  unsigned getBitWidth() const { return BitWidth; }
  const uint64_t getInitVal() const { return InitVal; }
  SIRRegisterTypes getRegisterType() const { return T; }
  Value *getRegVal() const { return RegVal; }
  Value *getRegGuard() const { return RegGuard; }
  void addAssignment(Value *Fanin, Value *FaninGuard);
  bool assignmentEmpty() { return Fanins.empty(); }
	void setMux(Value *V, Value *G) { 
		RegVal = V; RegGuard = G; 

		// Set the real operands to this register assign instruction.
		IntrinsicInst *II = dyn_cast<IntrinsicInst>(getLLVMValue());
		if (II && II->getIntrinsicID() == Intrinsic::shang_pseudo) {
			Value *PseudoSrcVal = II->getOperand(0);
			Value *PseudoGuardVal = II->getOperand(1);
			if (PseudoSrcVal != RegVal) {
				II->setOperand(0, RegVal);
			}
			if (PseudoGuardVal != RegGuard)
				II->setOperand(1, RegGuard);
		}
	}
	void dropMux() { 
		RegVal = NULL; RegGuard = NULL;

		Fanins.clear();
		FaninGuards.clear();
	}

  void printDecl(raw_ostream &OS) const;
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
  bool isInput() const { return T != RetPort; }

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
class SIRSlot;
class SIRSeqOp;

// Represent the assign operation in SIR.
class SIRSeqOp {
private:
	Value *Src;
	SIRRegister *DstReg;
	Value *Guard;
	SIRSlot *S;

public:
	SIRSeqOp(Value *Src, SIRRegister *DstReg,
		       Value *Guard, SIRSlot *S) 
		: Src(Src), DstReg(DstReg), Guard(Guard), S(S) {}

	Value *getLLVMValue() const { return DstReg->getLLVMValue(); }
	Value *getSrc() const { return Src; }
	Value *getGuard() const { return Guard; }
	SIRSlot *getSlot() const { return S; }
	SIRRegister *getDst() const { return DstReg; }

	void setSlot(SIRSlot *Slot) { S = Slot; }

	// Functions for debug
	void print(raw_ostream &OS) const;
	void dump() const; 
};

// Represent the state in the state-transition graph.
class SIRSlot {
public:
  // The types of the edges in the STG, the number representing the timing distance
  // of the edge, only the successor edge represents a real state transition
  // which have a timing distance of 1.
  enum EdgeType {
    SubGrp = 0,
    Sucessor = 1,
    ImplicitFlow = 2
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

  // The schedule result
  typedef uint16_t SlotNumTy;
  const SlotNumTy SlotNum;
  const SlotNumTy Schedule;

public:
  SIRSlot(unsigned SlotNum, BasicBlock *ParentBB,
          Value *SlotGuard, SIRRegister *Reg, unsigned Schedule)
    : SlotNum(SlotNum), ParentBB(ParentBB),
    SlotReg(Reg), Schedule(Schedule) {}
  ~SIRSlot();

  SlotNumTy getSlotNum() const { return SlotNum; }
  SlotNumTy getSchedule() const { return Schedule; }
  BasicBlock *getParent() const { return ParentBB; }
  SIRRegister *getSlotReg() const { return SlotReg; }
  Value *getGuardValue() const { return getSlotReg()->getLLVMValue(); }

  SIRSlot *getSubGroup(BasicBlock *BB) const;

  // If the SrcSlot already has this NextSlot as successor.
  bool hasNextSlot(SIRSlot *NextSlot);

  void addSeqOp(SIRSeqOp *Op) { Operations.push_back(Op); }
  void addSuccSlot(SIRSlot *NextSlot, EdgeType T, Value *Cnd);

  void unlinkSuccs();
  void unlinkSucc(SIRSlot *S);

	void replaceAllUsesWith(SIRSlot *S);

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
}

namespace llvm {
// The module in Shang IR.
class SIR {
public:
  typedef SmallVector<SIRPort *, 8> SIRPortVector;
  typedef SIRPortVector::iterator port_iterator;
  typedef SIRPortVector::const_iterator const_port_iterator;

  typedef SmallVector<SIRRegister *, 8> RegisterVector;
  typedef RegisterVector::iterator register_iterator;
  typedef RegisterVector::const_iterator const_register_iterator;

  typedef SmallVector<Instruction *, 8> DataPathInstVector;
  typedef DataPathInstVector::iterator datapathinst_iterator;
  typedef DataPathInstVector::const_iterator const_datapathinst_iterator;

  typedef SmallVector<SIRSlot *, 8> SlotVector;
  typedef SlotVector::iterator slot_iterator;
  typedef SlotVector::const_iterator const_slot_iterator;

  typedef SmallVector<SIRSeqOp *, 8> SeqOpVector;
  typedef SeqOpVector::iterator seqop_iterator;
  typedef SeqOpVector::const_iterator const_seqop_iterator;

  typedef DenseMap<Instruction *, SIRRegister *> SeqInst2RegMapTy;
  typedef SeqInst2RegMapTy::iterator seqinst2reg_iterator;
  typedef SeqInst2RegMapTy::const_iterator const_seqinst2reg_iterator;

  typedef DenseMap<SIRRegister *, SIRSlot *> Reg2SlotMapTy;
  typedef Reg2SlotMapTy::iterator reg2slot_iterator;
  typedef Reg2SlotMapTy::const_iterator const_reg2slot_iterator;

private:
  // Input/Output ports of the module
  SIRPortVector Ports;
  // Registers in the module
  RegisterVector Registers;
  // The DataPathInsts in the module
  DataPathInstVector DataPathInsts;
  // The Slots in CtrlRgn of the module
  SlotVector Slots;
  // The SeqOps in CtrlRgn of the module
  SeqOpVector SeqOps;
  // The map between SeqInst and SIRRegister
  SeqInst2RegMapTy SeqInst2Reg;
  // The map between Register and SIRSlot
  Reg2SlotMapTy Reg2Slot;

  // Record the Idx of RetPort.
  unsigned RetPortIdx;
  // Record the landing slot and the latest slot of BB.
  std::map<BasicBlock *, std::pair<SIRSlot *, SIRSlot *>> BB2SlotMap;

protected:
  Function *F;
  // Use LLVMContext to create Type and ConstantInt Value.
  LLVMContext &C;

public:
  SIR(Function *F) : F(F), C(F->getContext()) {}
  ~SIR();

	// Release the dead objects in SIR.
	bool gcImpl();
	bool gc();

  Function *getFunction() { return F; }
  Module *getModule() { return F->getParent(); }
  LLVMContext &getContext() { return C; }

  port_iterator ports_begin() { return Ports.begin(); }
  const_port_iterator ports_begin() const { return Ports.begin(); }

  port_iterator ports_end() { return Ports.end(); }
  const_port_iterator ports_end() const { return Ports.end(); }

  register_iterator registers_begin() { return Registers.begin(); }
  register_iterator registers_end() { return Registers.end(); }

  const_register_iterator registers_begin() const { return Registers.begin(); }
  const_register_iterator registers_end() const { return Registers.end(); }

  slot_iterator slot_begin() { return Slots.begin(); }
  slot_iterator slot_end() { return Slots.end(); }

  const_slot_iterator slot_begin() const { return Slots.begin(); }
  const_slot_iterator slot_end() const { return Slots.end(); }

  datapathinst_iterator datapathinst_begin() { return DataPathInsts.begin(); }
  datapathinst_iterator datapathinst_end() { return DataPathInsts.end(); }

  const_datapathinst_iterator const_datapathinst_begin() { return DataPathInsts.begin(); }
  const_datapathinst_iterator const_datapathinst_end() { return DataPathInsts.end(); }

  seqop_iterator seqop_begin() { return SeqOps.begin(); }
  seqop_iterator seqop_end() { return SeqOps.end(); }

  const_seqop_iterator const_seqop_begin() { return SeqOps.begin(); }
  const_seqop_iterator const_seqop_end() { return SeqOps.end(); }

  unsigned getSlotsSize() { return Slots.size(); }
  SlotVector &getSlotList() { return Slots; }
  SIRSlot *getStartSlot() { return Slots.front(); }

  unsigned getPortsSize() { return Ports.size(); }

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
  void IndexDataPathInst(Instruction *DataPathInst) {
    DataPathInsts.push_back(DataPathInst);
  }

  bool IndexSeqInst2Reg(Instruction *SeqInst, SIRRegister *Reg) {
    // Remember the connection between sequential instruction
    // and the register.
    return SeqInst2Reg.insert(std::make_pair(SeqInst, Reg)).second;
  }
  SIRRegister *lookupSIRReg(Instruction *SeqInst) {
    const_seqinst2reg_iterator at = SeqInst2Reg.find(SeqInst);
    return at == SeqInst2Reg.end() ? 0 : at->second;
  }

  bool IndexReg2Slot(SIRRegister *Reg, SIRSlot *S) {
    return Reg2Slot.insert(std::make_pair(Reg, S)).second;
  }
  SIRSlot *lookupSIRSlot(SIRRegister *Reg) {
    const_reg2slot_iterator at = Reg2Slot.find(Reg);
    return at == Reg2Slot.end() ? 0 : at->second;
  }

  void setRetPortIdx(unsigned Idx) { RetPortIdx = Idx; }
  unsigned getRetPortIdx() const { return RetPortIdx; }

  bool IndexBB2Slots(BasicBlock *BB,
                     SIRSlot *LandingSlot, SIRSlot *LatestSlot) {
    return BB2SlotMap.insert(std::make_pair(BB,
                             std::make_pair(LandingSlot, LatestSlot))).second;
  }
  std::map<BasicBlock *, std::pair<SIRSlot *, SIRSlot *>> getBB2SlotMap() {
    return BB2SlotMap;
  }
  std::pair<SIRSlot *, SIRSlot *> getSlotsOfBB(BasicBlock *BB) {
    std::map<BasicBlock*, std::pair<SIRSlot *, SIRSlot *> >::const_iterator
      at = BB2SlotMap.find(BB);
    //assert(at != BB2SlotMap.end() && "Slots not found!");
    return at->second;
  }
  SIRSlot *getLandingSlot(BasicBlock *BB) {
    std::map<BasicBlock*, std::pair<SIRSlot *, SIRSlot *> >::const_iterator
      at = BB2SlotMap.find(BB);
    assert(at != BB2SlotMap.end() && "Landing slot not found!");
    return at->second.first;
  }
  SIRSlot *getLatestSlot(BasicBlock *BB) {
    std::map<BasicBlock*, std::pair<SIRSlot *, SIRSlot *> >::const_iterator
      at = BB2SlotMap.find(BB);
    assert(at != BB2SlotMap.end() && "Latest slot not found!");
    return at->second.second;
  }

  SIRPort *getPort(unsigned i) const {
    assert(i < Ports.size() && "Out of range!");
    return Ports[i];
  }
  SIRPort *getRetPort() const { return getPort(RetPortIdx); }

  // --------------Functions to create ConstantInt Value------------------//

  // Create Type
  IntegerType *createIntegerType(unsigned BitWidth);
  // Create Value
  Value *createIntegerValue(unsigned BitWidth, unsigned Val);

  // -------------------Functions to generate Verilog-------------------- //
  
  // Print the declaration of module.
  void printModuleDecl(raw_ostream &OS) const;
	// Print the declaration of register.
	void printRegDecl(raw_ostream &OS) const;

  void printAsOperandImpl(raw_ostream &OS, Value *U, unsigned UB, unsigned LB) {
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U)) {
      assert(UB == CI->getBitWidth() && LB == 0 && "The slice of constant is not supported yet!");
      OS << "((";
      OS << buildLiteral(CI->getZExtValue(), UB, false);
      OS << "))";
      return;
    }

    // Print correctly if this value is a argument.
    if (Argument *Arg = dyn_cast<Argument>(U)) {
      OS << "((" << Mangle(Arg->getName());
    } 
    else if (Instruction *SeqInst = dyn_cast<Instruction>(U)) {
      // Print correctly if this value is a SeqValue.
      if (SIRRegister *Reg = lookupSIRReg(SeqInst))
        OS << "((" << Mangle(Reg->getName());
      else
        OS << "((" << Mangle(SeqInst->getName());
    } 

    unsigned OperandWidth = UB - LB;
    if (UB)
      OS << BitRange(UB, LB, OperandWidth > 1);

    // Ignore the mask for now
    OS << "))";

  }

  void printAsOperand(raw_ostream &OS, Value *U, unsigned BitWidth) {
    // Need to find a more proper way to get BitWidth 
    printAsOperandImpl(OS, U, BitWidth, 0);
  }

  void printSimpleOpImpl(raw_ostream &OS, ArrayRef<Value *> Ops,
                         const char *Opc, unsigned BitWidth) {
    unsigned NumOps = Ops.size();
    assert(NumOps && "Unexpected zero operand!");
    printAsOperand(OS, Ops[0], BitWidth);

    for (unsigned i = 1; i < NumOps; ++i) {
      OS << Opc;
      printAsOperand(OS, Ops[i], BitWidth);
    }
  }

};

}

#endif