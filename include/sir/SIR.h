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
#include <list>

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

class BitMask;

class SIR;
}

namespace llvm {
class BitMask {
private:
  APInt KnownZeros, KnownOnes, KnownSames;

public:
  BitMask() : KnownZeros(APInt::getNullValue(1)),
                 KnownOnes(APInt::getNullValue(1)),
                 KnownSames(APInt::getNullValue(1)) {}

  BitMask(unsigned BitWidth)
    : KnownZeros(APInt::getNullValue(BitWidth)),
      KnownOnes(APInt::getNullValue(BitWidth)),
      KnownSames(APInt::getNullValue(BitWidth)) {}

  BitMask(Value *V, unsigned BitWidth)
    : KnownZeros(APInt::getNullValue(BitWidth)),
      KnownOnes(APInt::getNullValue(BitWidth)),
      KnownSames(APInt::getNullValue(BitWidth)) {
    // Construct the mask from the constant directly.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
      APInt BitValue = CI->getValue();

      KnownOnes = BitValue;
      KnownZeros = ~BitValue;
    }
  }

  BitMask(APInt KnownZeros, APInt KnownOnes, APInt KnownSames)
    : KnownZeros(KnownZeros), KnownOnes(KnownOnes), KnownSames(KnownSames) {
    assert(KnownZeros.getBitWidth() == KnownOnes.getBitWidth() &&
           KnownZeros.getBitWidth() == KnownSames.getBitWidth() && "BitWidth not matches!");

    verify();
  }

  // Get the BitWidth of the Masks.
  unsigned getMaskWidth() const {
    verify();

    return KnownZeros.getBitWidth();
  }

  // Verify the correctness of KnownZeros and KnownOnes
  void verify() const {
    assert(KnownZeros.getBitWidth() == KnownOnes.getBitWidth() &&
           KnownZeros.getBitWidth() == KnownSames.getBitWidth()
           && "BitWidth not matches!");
    assert(!(KnownZeros & KnownOnes) && "BitMasks conflicts!");
  }

  bool isCompatibleWith(const BitMask &Mask) {
    return getMaskWidth() == Mask.getMaskWidth() &&
           !(KnownOnes & Mask.KnownZeros) && !(KnownZeros & Mask.KnownOnes);
  }

  // Set bit of KnownZeros, KnownOnes and KnownSigns
  void setKnownZeroAt(unsigned i) {
    KnownZeros.setBit(i);
    KnownOnes.clearBit(i);
    KnownSames.clearBit(i);
    verify();
  }
  void setKnownOneAt(unsigned i) {
    KnownOnes.setBit(i);
    KnownZeros.clearBit(i);
    KnownSames.clearBit(i);
    verify();
  }
  void setKnownSignAt(unsigned i) {
    KnownSames.setBit(i);
    verify();
  }

  void mergeKnownByOr(const BitMask &Mask) {
    assert(isCompatibleWith(Mask) && "BitMask conflicts!");

    KnownZeros |= Mask.KnownZeros;
    KnownOnes |= Mask.KnownOnes;
    KnownSames |= Mask.KnownSames;
    verify();
  }
  void mergeKnownByAnd(const BitMask &Mask) {
    assert(this->getMaskWidth() == Mask.getMaskWidth() && "BitWidth not matches!");

    KnownZeros &= Mask.KnownZeros;
    KnownOnes &= Mask.KnownOnes;
    KnownSames &= Mask.KnownSames;
    verify();
  }

  // Bit extraction of BitMask's APInt value.
  APInt getBitExtraction(const APInt &OriginMaskAPInt,
                         unsigned UB, unsigned LB) const {
    if (UB != OriginMaskAPInt.getBitWidth() || LB != 0)
      return OriginMaskAPInt.lshr(LB).sextOrTrunc(UB - LB);

    return OriginMaskAPInt;
  }

  // Shift left and fill the lower bits with zeros.
  BitMask shl(unsigned i) {
    APInt PaddingZeros = APInt::getLowBitsSet(getMaskWidth(), i);

    return BitMask(KnownZeros.shl(i) | PaddingZeros, KnownOnes.shl(i), KnownSames.shl(i));
  }
  // Logic shift right and fill the higher bits with zeros.
  BitMask lshr(unsigned i) {
    APInt PaddingZeros = APInt::getHighBitsSet(getMaskWidth(), i);

    return BitMask(KnownZeros.lshr(i) | PaddingZeros, KnownOnes.lshr(i), KnownSames.lshr(i));
  }
  // Arithmetic shift right and fill the higher bits with sign bit. However,
  // this can only be down when it is known.
  BitMask ashr(unsigned i) {
    return BitMask(KnownZeros.ashr(i), KnownOnes.ashr(i), KnownSames.ashr(i));
  }
  // Extend the width of mask
  BitMask extend(unsigned BitWidth) {
    return BitMask(KnownZeros.zextOrSelf(BitWidth), KnownOnes.zextOrSelf(BitWidth), KnownSames.zextOrSelf(BitWidth));
  }

#define CREATEBITACCESSORS(WHAT, VALUE) \
  APInt getKnown##WHAT##s() const { return VALUE; } \
  APInt getKnown##WHAT##s(unsigned UB, unsigned LB = 0) const { \
    return getBitExtraction(getKnown##WHAT##s(), UB, LB); \
  } \
  unsigned getNumKnown##WHAT##s() const { \
    return getKnown##WHAT##s().countPopulation(); \
  } \
  unsigned getNumKnown##WHAT##s(unsigned UB, unsigned LB = 0) const { \
    return getKnown##WHAT##s(UB, LB).countPopulation(); \
  } \
  bool is##WHAT##KnownAt(unsigned N) const { \
    return get##Known##WHAT##s()[N]; \
  } \
  bool isAll##WHAT##Known() const { \
    return get##Known##WHAT##s().isAllOnesValue(); \
  } \
  bool isAll##WHAT##Known(unsigned UB, unsigned LB = 0) const { \
    return getBitExtraction(get##Known##WHAT##s(), UB, LB).isAllOnesValue(); \
  } \
  bool hasAny##WHAT##Known() const { \
    return get##Known##WHAT##s().getBoolValue(); \
  } \
  bool hasAny##WHAT##Known(unsigned UB, unsigned LB = 0) const { \
    return getBitExtraction(get##Known##WHAT##s(), UB, LB).getBoolValue(); \
  }

  CREATEBITACCESSORS(One, KnownOnes)
  CREATEBITACCESSORS(Zero, KnownZeros)
  CREATEBITACCESSORS(Same, KnownSames)
  CREATEBITACCESSORS(Bit, KnownOnes | KnownZeros)
  CREATEBITACCESSORS(Value, (KnownOnes & ~KnownZeros))

  unsigned countLeadingZeros() {
    unsigned LeadingZeros = 0;
    unsigned MaskWidth = getMaskWidth();

    for (unsigned i = 0; i < MaskWidth; ++i) {
      if (isZeroKnownAt(MaskWidth - 1 - i))
        ++LeadingZeros;
      else
        break;
    }

    return LeadingZeros;
  }
  unsigned countTrailingZeros() {
    unsigned TrailingZeros = 0;
    unsigned MaskWidth = getMaskWidth();

    for (unsigned i = 0; i < MaskWidth; ++i) {
      if (isZeroKnownAt(i))
        ++TrailingZeros;
      else
        break;
    }

    return TrailingZeros;
  }
  unsigned countLeadingOnes() {
    unsigned LeadingOnes = 0;
    unsigned MaskWidth = getMaskWidth();

    for (unsigned i = 0; i < MaskWidth; ++i) {
      if (isOneKnownAt(MaskWidth - 1 - i))
        ++LeadingOnes;
      else
        break;
    }

    return LeadingOnes;
  }
  unsigned countTrailingOnes() {
    unsigned TrailingOnes = 0;
    unsigned MaskWidth = getMaskWidth();

    for (unsigned i = 0; i < MaskWidth; ++i) {
      if (isOneKnownAt(i))
        ++TrailingOnes;
      else
        break;
    }

    return TrailingOnes;
  }
  unsigned countLeadingSigns() {
    unsigned LeadingSigns = 0;
    unsigned MaskWidth = getMaskWidth();

    for (unsigned i = 0; i < MaskWidth; ++i) {
      if (isSameKnownAt(MaskWidth - 1 - i))
        ++LeadingSigns;
      else
        break;
    }

    return LeadingSigns;
  }

  bool operator==(BitMask Mask) {
    return this->KnownOnes == Mask.KnownOnes &&
           this->KnownZeros == Mask.KnownZeros &&
           this->KnownSames == Mask.KnownSames;
  }

  void print(raw_ostream &Output) {
    unsigned BitWidth = getMaskWidth();
    for (unsigned i = 0; i < BitWidth; ++i) {
      if (KnownZeros[BitWidth - 1 - i] == 1)
        Output << 0;
      else if (KnownOnes[BitWidth - 1 - i] == 1)
        Output << 1;
      else if (KnownSames[BitWidth - 1 -i] == 1)
        Output << 's';
      else
        Output << 'x';
    }
  }
};

struct SMGNode : public ilist_node<SMGNode> {
public:
  struct EdgePtr {
  private:
    SMGNode *Node;
    float Distance;

  public:
    EdgePtr(SMGNode *Node, float Distance) : Node(Node), Distance(Distance) {}

    operator SMGNode *() const { return Node; }
    SMGNode *operator->() const { return Node; }

    float getDistance() const { return Distance; }
  };

  typedef SmallVector<EdgePtr, 4> ChildVecTy;
  typedef ChildVecTy::iterator child_iterator;
  typedef ChildVecTy::const_iterator const_child_iterator;

  typedef SmallVector<EdgePtr, 4> ParentVecTy;
  typedef ParentVecTy::iterator parent_iterator;
  typedef ParentVecTy::const_iterator const_parent_iterator;

private:
  Value *Node;

  ChildVecTy Childs;
  ParentVecTy Parents;

public:
  // Construction for ilist node.
  SMGNode() : Node(0) {}
  // Construction for normal node.
  SMGNode(Value *V) : Node(V) {}

  Value *getValue() const { return Node; }

  // Childs.
  child_iterator child_begin() { return Childs.begin(); }
  const_child_iterator child_begin() const { return Childs.begin(); }
  child_iterator child_end() { return Childs.end(); }
  const_child_iterator child_end() const { return Childs.end(); }
  bool child_empty() const { return Childs.empty(); }
  unsigned child_size() const { return Childs.size(); }

  // Parents.
  parent_iterator parent_begin() { return Parents.begin(); }
  parent_iterator parent_end() { return Parents.end(); }
  const_parent_iterator parent_begin() const { return Parents.begin(); }
  const_parent_iterator parent_end() const { return Parents.end(); }
  bool parent_empty() const { return Parents.empty(); }
  unsigned parent_size() const { return Parents.size(); }

  bool hasChildNode(SMGNode *ChildNode) {
    for (const_child_iterator I = child_begin(), E = child_end(); I != E; ++I)
      if (ChildNode == EdgePtr(*I))
        return true;

    return false;
  }

  void addChildNode(SMGNode *ChildNode, float delay) {
    assert(!hasChildNode(ChildNode) && "Add same child twice!");

    ChildNode->Parents.push_back(EdgePtr(this, delay));
    Childs.push_back(EdgePtr(ChildNode, delay));
  }
};

template<> struct GraphTraits<SMGNode *> {
  typedef SMGNode NodeType;
  typedef NodeType::child_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->child_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->child_end();
  }
};

template<> struct GraphTraits<const SMGNode *> {
  typedef SMGNode NodeType;
  typedef NodeType::const_child_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->child_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->child_end();
  }
};

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

  void setLLVMValue(Value *V) { LLVMValue = V; }
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
  bool isSynthesized() { return (RegVal != NULL); }

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
class DFGNode : public ilist_node<DFGNode> {
public:
  enum NodeType {
    /// Virtual node as Entry & Exit
    Entry,
    Exit,
    /// the argument of design
    Argument,
    /// the constant values
    ConstantInt,
    GlobalVal,
    UndefVal,
    /// normal datapath value type
    Add,
    Mul,
    Div,
    LShr,
    AShr,
    Shl,
    Not,
    And,
    Or,
    Xor,
    RAnd,
    GT,
    LT,
    Eq,
    NE,
    BitExtract,
    BitCat,
    BitRepeat,
    BitManipulate,
    TypeConversion,
    Ret,
    /// special datapath value type
    CompressorTree,
    LogicOperationChain,
    /// sequential value type
    Register,
    /// invalid
    InValid,
  };

private:
  // Index
  unsigned Idx;
  // Name
  std::string Name;
  // Corresponding llvm value in IR.
  Value *Val;
  // Integer value if it is a ConstantInt DFGNode.
  uint64_t IntVal;
  // Corresponding basic block in IR.
  BasicBlock *ParentBB;
  // Node type.
  NodeType Ty;
  // BitWidth
  unsigned BitWidth;

  // Child nodes & Parent nodes.
  std::vector<DFGNode *> Childs;
  std::vector<DFGNode *> Parents;

public:
  // Construction for ilist node.
  DFGNode() : Ty(InValid) {}
  // Construction for normal node.
  DFGNode(unsigned Idx, std::string Name, Value *V, uint64_t IntV,
          BasicBlock *BB, NodeType Ty, unsigned BitWidth)
    : Idx(Idx), Name(Name), Val(V), IntVal(IntV),
      ParentBB(BB), Ty(Ty), BitWidth(BitWidth) {
    if (V) {
      assert(!isa<Function>(V) && "Unexpected value type!");
      assert(IntV == NULL && "Unexpected integer value!");
    }
  }

  unsigned getIdx() const { return Idx; }
  std::string getName() const { return Name; }
  Value *getValue() const { return Val; }
  uint64_t getIntValue() const {
    assert(Ty == DFGNode::ConstantInt && "Unexpected DFGNode type!");
    return IntVal;
  }
  BasicBlock *getParentBB() const { return ParentBB; }
  NodeType getType() const { return Ty; }
  unsigned getBitWidth() const { return BitWidth; }

  bool isEntryOrExit() const {
    return Ty == Entry || Ty == Exit;
  }
  bool isSequentialNode() const {
    return Ty == Register || Ty == GlobalVal;
  }

  typedef std::vector<DFGNode *>::iterator iterator;
  typedef std::vector<DFGNode *>::const_iterator const_iterator;

  // Childs.  
  iterator child_begin() { return Childs.begin(); }
  const_iterator child_begin() const { return Childs.begin(); }
  iterator child_end() { return Childs.end(); }
  const_iterator child_end() const { return Childs.end(); }
  bool child_empty() const { return Childs.empty(); }
  unsigned child_size() const { return Childs.size(); }

  // Parents.
  iterator parent_begin() { return Parents.begin(); }
  const_iterator parent_begin() const { return Parents.begin(); }
  iterator parent_end() { return Parents.end(); }
  const_iterator parent_end() const { return Parents.end(); }
  bool parent_empty() const { return Parents.empty(); }
  unsigned parent_size() const { return Parents.size(); }

  DFGNode *getParentNode(unsigned Idx) {
    assert(Idx < Parents.size() && "Unexpected index!");

    return Parents[Idx];
  }
  std::vector<unsigned> getParentNodeIdxs(DFGNode *ParentNode) {
    std::vector<unsigned> Idxs;
    unsigned Idx = 0;
    for (const_iterator I = parent_begin(), E = parent_end(); I != E; ++I) {
      DFGNode *Node = *I;

      if (Node == ParentNode)
        Idxs.push_back(Idx);

      ++Idx;
    }

    return Idxs;
  }

  bool hasChildNode(DFGNode *ChildNode) const {
    bool Has = false;
    for (const_iterator I = child_begin(), E = child_end(); I != E; ++I) {
      DFGNode *CNode = *I;

      if (CNode == ChildNode) {
        Has = true;
        break;
      }
    }

    return Has;
  }
  bool hasParentNode(DFGNode *ParentNode) const {
    bool Has = false;
    for (const_iterator I = parent_begin(), E = parent_end(); I != E; ++I) {
      DFGNode *PNode = *I;

      if (PNode == ParentNode) {
        Has = true;
        break;
      }
    }

    return Has;
  }

  void addParentNode(DFGNode *ParentNode) {
    Parents.push_back(ParentNode);
  }

  void removeParentNode(DFGNode *ParentNode) {
    std::vector<DFGNode *> NewParents;

    for (iterator I = parent_begin(), E = parent_end(); I != E; ++I) {
      DFGNode *PNode = *I;

      if (PNode != ParentNode)
        NewParents.push_back(PNode);
    }

    Parents = NewParents;
  }

  void addChildNode(DFGNode *ChildNode) {
    Childs.push_back(ChildNode);
  }

  void removeChildNode(DFGNode *ChildNode) {
    std::vector<DFGNode *> NewChilds;

    for (iterator I = child_begin(), E = child_end(); I != E; ++I) {
      DFGNode *CNode = *I;

      if (CNode != ChildNode)
        NewChilds.push_back(CNode);
    }

    Childs = NewChilds;
  }

  void replaceParentNode(DFGNode *OldParent, DFGNode *NewParent) {
    // Get the index of the old parent node.
    std::vector<unsigned> Idxs = getParentNodeIdxs(OldParent);

    // Replace the parent node.
    for (unsigned i = 0; i < Idxs.size(); ++i) {
      unsigned Idx = Idxs[i];
      Parents[Idx] = NewParent;
    }

    assert(!hasParentNode(OldParent) && "Failed to replace!");
    assert(hasParentNode(NewParent) && "Failed to replace!");
  }

  void clear() {
    Childs.clear();
    Parents.clear();
  }
};

template<> struct GraphTraits<DFGNode *> {
  typedef DFGNode NodeType;
  typedef NodeType::iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->child_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->child_end();
  }
};

template<> struct GraphTraits<const DFGNode *> {
  typedef DFGNode NodeType;
  typedef NodeType::const_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->child_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->child_end();
  }
};

class DataFlowGraph {
public:
  typedef iplist<DFGNode> DFGNodeListTy;
  typedef std::pair<std::vector<DFGNode *>,
                    std::vector<DFGNode *> > OperationsAndOperandsTy;
  typedef std::map<DFGNode *, OperationsAndOperandsTy> LOCType;

private:
  SIR *SM;

  // Index the Entry and Exit node.
  DFGNode *Entry;
  DFGNode *Exit;

  // The list of all nodes in DFG.
  DFGNodeListTy DFGNodeList;
  // The logic operation chains
  LOCType LOC;
  // The map between Root and the LOC node we creat
  std::map<DFGNode *, DFGNode *> RootOfLOC;
  // The map between Operation nodes and the LOC node we creat
  std::map<DFGNode *, std::vector<DFGNode *> > OperationsOfLOC;

  std::map<DFGNode *, DFGNode *> ReplacedNodes;

public:
  DataFlowGraph(SIR *SM) : SM(SM) {
    // Create the entry SU.
    this->Entry = new DFGNode(0, "Entry", NULL, NULL, NULL, DFGNode::Entry, 0);
    DFGNodeList.push_back(Entry);
    // Create the exit SU.
    this->Exit = new DFGNode(UINT_MAX, "Exit", NULL, NULL, NULL, DFGNode::Exit, 0);
    DFGNodeList.push_back(Exit);
  }

  unsigned size() const { return DFGNodeList.size(); }
  DFGNode *getEntry() { return Entry; }
  const DFGNode *getEntry() const { return Entry; }

  DFGNode *getExit() { return Exit; }
  const DFGNode *getExit() const { return Exit; }

  // Sort the DFG nodes in topological order.
  void toposortCone(DFGNode *Root, std::set<DFGNode *> &Visited);
  void topologicalSortNodes();

  DFGNode *createDFGNode(std::string Name, Value *Val, uint64_t ConstantVal,
                         BasicBlock *BB, DFGNode::NodeType Ty, unsigned BitWidth,
                         bool ToReplace);

  DFGNode *createDataPathNode(Instruction *Inst, unsigned BitWidth);
  DFGNode *createConstantIntNode(uint64_t Val, unsigned BitWidth);
  DFGNode *createConstantIntNode(ConstantInt *CI, unsigned BitWidth);
  DFGNode *createGlobalValueNode(GlobalValue *GV, unsigned BitWidth);
  DFGNode *createUndefValueNode(UndefValue *UV, unsigned BitWidth);
  DFGNode *createArgumentNode(Argument *Arg, unsigned BitWidth);

  void createDependency(DFGNode *Src, DFGNode *Dst);
  void removeDependency(DFGNode *Src, DFGNode *Dst);

  void replaceDepSrc(DFGNode *Dst, DFGNode *OldSrc, DFGNode *NewSrc);

  std::vector<DFGNode *> getOperationsOfLOCRoot(DFGNode *RootNode) {
    assert(LOC.count(RootNode) && "LOC not existed!");

    return LOC[RootNode].first;
  }
  std::vector<DFGNode *> getOperandsOfLOCRoot(DFGNode *RootNode) {
    assert(LOC.count(RootNode) && "LOC not existed!");

    return LOC[RootNode].second;
  }
  void indexLOC(DFGNode *RootNode, std::vector<DFGNode *> Operations,
                std::vector<DFGNode *> Operands) {
    assert(!LOC.count(RootNode) && "Already existed!");

    LOC.insert(std::make_pair(RootNode, std::make_pair(Operations, Operands)));
  }

  bool hasLOCNode(DFGNode *RootNode) {
    return RootOfLOC.count(RootNode);
  }
  DFGNode *getLOCNode(DFGNode *RootNode) {
    assert(RootOfLOC.count(RootNode) && "Not existed!");

    return RootOfLOC[RootNode];
  }
  void indexRootOfLOC(DFGNode *RootNode, DFGNode *LOCNode) {
    assert(!RootOfLOC.count(RootNode) && "Already existed!");

    RootOfLOC.insert(std::make_pair(RootNode, LOCNode));
  }

  std::vector<DFGNode *> getOperations(DFGNode *LOCNode) {
    assert(OperationsOfLOC.count(LOCNode) && "Not existed!");

    return OperationsOfLOC[LOCNode];
  }
  void indexOperationsOfLOC(DFGNode *LOCNode, std::vector<DFGNode *> Operations) {
    assert(!OperationsOfLOC.count(LOCNode) && "Already existed!");

    OperationsOfLOC.insert(std::make_pair(LOCNode, Operations));
  }


  typedef DFGNodeListTy::iterator node_iterator;
  typedef DFGNodeListTy::const_iterator const_node_iterator;

  node_iterator begin() { return DFGNodeList.begin(); }
  node_iterator end() { return DFGNodeList.end(); }

  const_node_iterator begin() const { return DFGNodeList.begin(); }
  const_node_iterator end() const { return DFGNodeList.end(); }

  typedef LOCType::iterator loc_iterator;
  typedef LOCType::const_iterator const_loc_iterator;

  loc_iterator loc_begin() { return LOC.begin(); }
  loc_iterator loc_end() { return LOC.end(); }

  const_loc_iterator loc_begin() const { return LOC.begin(); }
  const_loc_iterator loc_end() const { return LOC.end(); }

  void clear() {
    for (node_iterator I = begin(), E = end(); I != E; ++I) {
      DFGNode *Node = I;

      Node->clear();
    }

    DFGNodeList.clear();
    LOC.clear();
    RootOfLOC.clear();
    OperationsOfLOC.clear();
    ReplacedNodes.clear();
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

  typedef std::map<DFGNode *, BitMask> Node2BitMaskMapTy;
  typedef Node2BitMaskMapTy::iterator node2bitmask_iterator;
  typedef Node2BitMaskMapTy::const_iterator const_node2bitmask_iterator;

  typedef std::pair<Value *, std::pair<Value *, Value *> > MulOpTy;
  typedef std::pair<std::vector<Value *>, std::vector<MulOpTy> > CompressorOpsTy;
  typedef std::map<Value *, CompressorOpsTy> Ops2CTTy;
  typedef Ops2CTTy::iterator ops2ct_iterator;
  typedef Ops2CTTy::const_iterator const_ops2ct_iterator;

  typedef DenseMap<Value *, Value *> Val2SeqValMapTy;
  typedef Val2SeqValMapTy::iterator val2valseq_iterator;
  typedef Val2SeqValMapTy::const_iterator const_val2valseq_iterator;

  typedef DenseMap<Value *, SIRRegister *> SeqVal2RegMapTy;
  typedef SeqVal2RegMapTy::iterator seqval2reg_iterator;
  typedef SeqVal2RegMapTy::const_iterator const_seqval2reg_iterator;

  typedef DenseMap<SIRRegister *, SIRSlot *> Reg2SlotMapTy;
  typedef Reg2SlotMapTy::iterator reg2slot_iterator;
  typedef Reg2SlotMapTy::const_iterator const_reg2slot_iterator;

  typedef DenseMap<Value *, float> Val2ValidTimeMapTy;
  typedef Val2ValidTimeMapTy::iterator val2validtime_iterator;
  typedef Val2ValidTimeMapTy::const_iterator const_val2validtime_iterator;

  typedef std::map<Instruction *, std::vector<Value *> > OpsOfLOCTy;
  typedef OpsOfLOCTy::iterator opsofloc_iterator;
  typedef OpsOfLOCTy::const_iterator const_opsofloc_iterator;

  typedef DenseMap<Value *, DFGNode *> DFGNodeOfValMapTy;

  typedef std::set<SIRRegister *> ArgRegsTy;

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
  // The map between DFG node and BitMask.
  Node2BitMaskMapTy Node2BitMask;
  // The map between CompressorTree and its operands
  Ops2CTTy Ops2CT;
  // The map between SeqVal in SIR and Reg in SIR
  SeqVal2RegMapTy SeqVal2Reg;
  // The map between Reg and SIRSlot
  Reg2SlotMapTy Reg2Slot;
  // The map between Value and its valid time
  Val2ValidTimeMapTy Val2ValidTimeMap;
  // The map between logic operation chain and its operands
  OpsOfLOCTy OpsOfLOC;
  // The map between value and its corresponding DFGNode
  DFGNodeOfValMapTy DFGNodeOfVal;
  // The registers created for Arguments of the module
  ArgRegsTy ArgRegs;

  // Registers that should be kept.
  std::set<SIRRegister *> KeepRegs;

  std::set<Value *> KeepVals;

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

  bool hasKeepReg(SIRRegister *Reg) const { return KeepRegs.count(Reg); }
  void indexKeepReg(SIRRegister *Reg) {
    if (!KeepRegs.count(Reg))
      KeepRegs.insert(Reg);
  }

  bool hasKeepVal(Value *Val) const { return KeepVals.count(Val); }
  void indexKeepVal(Value *Val) {
    if (!KeepVals.count(Val))
      KeepVals.insert(Val);
  }
  void removeKeepVal(Value *Val) {
    assert(KeepVals.count(Val) && "Value not existed!");

    KeepVals.erase(Val);
  }

  void indexValidTime(Value *Val, float ValidTime) {
    assert(!Val2ValidTimeMap.count(Val) && "Already existed!");

    Val2ValidTimeMap.insert(std::make_pair(Val, ValidTime));
  }
  float getValidTime(Value *Val) {
    assert(Val2ValidTimeMap.count(Val) && "Not existed!");

    return Val2ValidTimeMap[Val];
  }

  bool isLOCRoot(Instruction *Inst) { return OpsOfLOC.count(Inst); }
  void indexOpsOfLOC(Instruction *LOCRott, std::vector<Value *> Ops) {
    assert(!OpsOfLOC.count(LOCRott) && "Already existed!");

    OpsOfLOC.insert(std::make_pair(LOCRott, Ops));
  }
  std::vector<Value *> getOpsOfLOC(Instruction *LOCRott) {
    assert(OpsOfLOC.count(LOCRott) && "Not existed!");

    return OpsOfLOC[LOCRott];
  }

  bool isDFGNodeExisted(Value *Val) const { return DFGNodeOfVal.count(Val); }
  void indexDFGNodeOfVal(Value *Val, DFGNode *Node) {
    assert(!DFGNodeOfVal.count(Val) && "Already existed!");

    DFGNodeOfVal.insert(std::make_pair(Val, Node));
  }
  void replaceDFGNodeOfVal(Value *Val, DFGNode *Node) {
    assert(DFGNodeOfVal.count(Val) && "Not existed!");

    DFGNodeOfVal[Val] = Node;
  }
  DFGNode *getDFGNodeOfVal(Value *Val) {
    assert(DFGNodeOfVal.count(Val) && "Not existed!");

    return DFGNodeOfVal[Val];
  }

  void indexArgReg(SIRRegister *Reg) {
    ArgRegs.insert(Reg);
  }
  bool isArgReg(Value *Val) {
    SIRRegister *Reg = lookupSIRReg(Val);

    if (Reg) {
      return ArgRegs.count(Reg);
    }

    return false;
  }
  std::set<SIRRegister *> getArgRegs() {
    return ArgRegs;
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

  opsofloc_iterator loc_begin() { return OpsOfLOC.begin(); }
  opsofloc_iterator loc_end() { return OpsOfLOC.end(); }

  const_opsofloc_iterator const_loc_begin() { return OpsOfLOC.begin(); }
  const_opsofloc_iterator const_loc_end() { return OpsOfLOC.end(); }

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

  bool hasBitMask(Value *Val) {
    DFGNode *Node = getDFGNodeOfVal(Val);

    return hasBitMask(Node);
  }
  BitMask getBitMask(Value *Val) {
    DFGNode *Node = getDFGNodeOfVal(Val);

    return getBitMask(Node);
  }

  bool IndexNode2BitMask(DFGNode *Node, BitMask Mask) {
    Mask.verify();

    if (hasBitMask(Node)) {
      Node2BitMask[Node] = Mask;

      return true;
    }

    return Node2BitMask.insert(std::make_pair(Node, Mask)).second;
  }
  bool hasBitMask(DFGNode *Node) const {
    const_node2bitmask_iterator at = Node2BitMask.find(Node);
    return at != Node2BitMask.end();
  }
  BitMask getBitMask(DFGNode *Node) const {
    assert(hasBitMask(Node) && "Mask not created yet?");

    const_node2bitmask_iterator at = Node2BitMask.find(Node);
    return at->second;
  }

  bool IndexOps2CT(Value *Root, std::vector<Value *> NormalOps,
                   std::vector<MulOpTy> MulOps) {
    CompressorOpsTy Ops = std::make_pair(NormalOps, MulOps);
    return Ops2CT.insert(std::make_pair(Root, Ops)).second;
  }
  std::vector<Value *> getNormalOpsOfCT(Value *Root) const {
    const_ops2ct_iterator at = Ops2CT.find(Root);
    return at == Ops2CT.end() ? std::vector<Value *>() : at->second.first;
  }
  std::vector<MulOpTy> getMulOpsOfCT(Value *Root) const {
    const_ops2ct_iterator at = Ops2CT.find(Root);
    return at == Ops2CT.end() ? std::vector<MulOpTy>() : at->second.second;
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

  void clear() {
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
    KeepRegs.clear();
    KeepVals.clear();
    BB2SlotMap.clear();
    Ops2CT.clear();
    Node2BitMask.clear();
  }

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