#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"

#include <sstream>

using namespace llvm;
using namespace vast;

typedef std::pair<std::string, float> DotType;
typedef std::vector<DotType> MatrixRowType;
typedef std::vector<MatrixRowType> MatrixType;

static unsigned COMPRESSOR_NUM = 0;

static float COMPRESSOR_3_2_DELAY[2][3] = {{0.43, 0.18, 0.44}, {0.43, 0.21, 0.45}};
static float COMPRESSOR_2_2_DELAY[2][2] = {{0.24, 0.22}, {0.17, 0.15}};

static float ADD_16_DELAY = 0.30;
static float ADD_32_DELAY = 0.57;
static float ADD_64_DELAY = 1.13;

namespace {
struct SIRAddMulChain : public SIRPass {
  static char ID;
  DataLayout *TD;
  SIR *SM;

  unsigned ChainNum;

  std::set<IntrinsicInst *> Visited;
  std::set<IntrinsicInst *> Collected;
  std::map<IntrinsicInst *, std::set<IntrinsicInst *> > ChainMap;
  std::map<IntrinsicInst *, IntrinsicInst *> ChainRoot2Compressor;
  std::map<IntrinsicInst *, IntrinsicInst *> Compressor2ChainRoot;

  std::map<Value *, float> ValArrivalTime;

  SIRAddMulChain() : SIRPass(ID), ChainNum(0), DebugOutput("DebugMatrix.txt", Error) {
    initializeSIRAddMulChainPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);
  void collectAddMulChain();
  void visit(Value *Root);
  void collect(IntrinsicInst *ChainRoot);
  void collectAddShiftChain(IntrinsicInst *ChainRoot);

  std::vector<Value *> eliminateIdenticalOperands(std::vector<Value *> Operands,
                                                  Value *ChainRoot, unsigned BitWidth);
  std::vector<Value *> OptimizeOperands(std::vector<Value *> Operands,
                                        Value *ChainRoot, unsigned BitWidth);

  MatrixType createMatrixForOperands(std::vector<Value *> Operands,
                                     unsigned RowNum, unsigned ColNum);
  MatrixType sumAllSignBitsInMatrix(MatrixType Matrix, unsigned RowNum, unsigned ColumnNum);

  float predictAddChainResultTime(std::vector<std::pair<unsigned, float> > AddChain,
                                  MatrixType Matrix);

  float getLatency(Instruction *Inst);
  float getOperandArrivalTime(Value *Operand);

  bool isConstantInt(MatrixRowType Row);
  unsigned getOperandBitWidth(MatrixRowType Row);
  std::vector<unsigned> getSignBitNumListInMatrix(MatrixType Matrix);
  MatrixType transportMatrix(MatrixType Matrix, unsigned RowNum, unsigned ColumnNum);

  std::vector<unsigned> getOneBitNumListInTMatrix(MatrixType TMatrix);
  std::vector<unsigned> getBitNumListInTMatrix(MatrixType TMatrix);
  MatrixType simplifyTMatrix(MatrixType TMatrix);
  MatrixType sortTMatrix(MatrixType TMatrix);
  MatrixType sumAllOneBitsInTMatrix(MatrixType TMatrix);
  MatrixType eliminateOneBitInTMatrix(MatrixType TMatrix);

  std::pair<float, float> generateCompressor(std::vector<DotType> CompressCouple,
                                             std::string SumName, std::string CoutName,
                                             raw_fd_ostream &Output);

  bool needToCompress(std::vector<unsigned> BitNumList, unsigned RowNo);
  MatrixType compressTMatrixInStage(MatrixType TMatrix,
                                    unsigned Stage, raw_fd_ostream &Output);
  float compressMatrix(MatrixType TMatrix, std::string MatrixName,
                       unsigned OperandNum, unsigned OperandWidth,
                       raw_fd_ostream &Output);

  void generateAddChain(MatrixType Matrix, std::vector<std::pair<unsigned, float> > Ops,
                        std::string ResultName, raw_fd_ostream &Output);
  float hybridTreeCodegen(MatrixType Matrix, std::string MatrixName,
                          unsigned RowNum, unsigned ColNum, raw_fd_ostream &Output);

  std::string Error;
  raw_fd_ostream DebugOutput;
  void printTMatrixForDebug(MatrixType TMatrix);

  void generateDotMatrix();
  void generateDotmatrixForChain(IntrinsicInst *ChainRoot, raw_fd_ostream &Output);
  void replaceWithCompressor();

  void printAllChain();

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRBitMaskAnalysisID);
    AU.setPreservesAll();
  }
};
}

char SIRAddMulChain::ID = 0;
char &llvm::SIRAddMulChainID = SIRAddMulChain::ID;
INITIALIZE_PASS_BEGIN(SIRAddMulChain, "sir-add-mul-chain",
                      "Perform the add-mul chain optimization",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRBitMaskAnalysis)
INITIALIZE_PASS_END(SIRAddMulChain, "sir-add-mul-chain",
                    "Perform the add-mul chain optimization",
                    false, true)

static bool isLeafValue(SIR *SM, Value *V) {
  // When we visit the Srcs of value, the Leaf Value
  // means the top nodes of the Expr-Tree. There are
  // four kinds of Leaf Value:
  // 1) Argument 2) Register 3) ConstantValue
  // 4) GlobalValue 5) UndefValue
  // The path between Leaf Value and other values
  // will cost no delay (except wire delay).
  // However, since the ConstantValue will have
  // no impact on the scheduling process, so
  // we will just ignore the ConstantInt in
  // previous step.

  if (isa<ConstantInt>(V)) return true;

  if (isa<ConstantVector>(V)) return true;

  if (isa<ConstantAggregateZero>(V)) return true;

  if (isa<ConstantPointerNull>(V)) return true;

  if (isa<Argument>(V))	return true;

  if (isa<GlobalValue>(V)) return true;

  if (isa<UndefValue>(V)) return true;

  if (Instruction *Inst = dyn_cast<Instruction>(V))
    if (SIRRegister *Reg = SM->lookupSIRReg(Inst))
      return true;

  return false;
}

bool SIRAddMulChain::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  collectAddMulChain();
  //printAllChain();

  replaceWithCompressor();
  generateDotMatrix();

  return false;
}

void SIRAddMulChain::collectAddMulChain() {
  Function *F = SM->getFunction();

  typedef SIR::register_iterator reg_iterator;
  for (reg_iterator RI = SM->registers_begin(), RE = SM->registers_end(); RI != RE; ++RI) {
    SIRRegister *Reg = RI;

    visit(Reg->getLLVMValue());
  }
}

void SIRAddMulChain::visit(Value *Root) {
  IntrinsicInst *RootInst = dyn_cast<IntrinsicInst>(Root);
  assert(RootInst && "Unexpected value type!");

  typedef Instruction::op_iterator op_iterator;
  std::vector<std::pair<IntrinsicInst *, op_iterator> > VisitStack;
  std::vector<IntrinsicInst *> AddInstVector;

  VisitStack.push_back(std::make_pair(RootInst, RootInst->op_begin()));

  while(!VisitStack.empty()) {
    IntrinsicInst *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      if (CurNode->getIntrinsicID() == Intrinsic::shang_add || CurNode->getIntrinsicID() == Intrinsic::shang_addc) {
        unsigned UserNum = 0;
        unsigned UsedByChainNum = 0;
        typedef Value::use_iterator use_iterator;
        for (use_iterator UI = CurNode->use_begin(), UE = CurNode->use_end(); UI != UE; ++UI) {
          Value *UserVal = *UI;

          if (IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(UserVal)) {
            ++UserNum;

            if (UserInst->getIntrinsicID() == Intrinsic::shang_add || UserInst->getIntrinsicID() == Intrinsic::shang_addc)
              ++UsedByChainNum;
          }
        }

        if (UsedByChainNum == 0 || UserNum >= 2)
          AddInstVector.push_back(CurNode);
      }

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *I;

    ++I;
    IntrinsicInst *ChildInst = dyn_cast<IntrinsicInst>(ChildVal);

    if (!ChildInst)
      continue;

    if (Visited.count(ChildInst))
      continue;

    if (ChildInst->getIntrinsicID() == Intrinsic::shang_reg_assign)
      continue;

    VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
    Visited.insert(ChildInst);
  }

  for (unsigned i = 0; i < AddInstVector.size(); ++i) {
    IntrinsicInst *AddInst = AddInstVector[i];
    collect(AddInst);
  }
}

void SIRAddMulChain::collect(IntrinsicInst *ChainRoot) {
  assert(ChainRoot->getIntrinsicID() == Intrinsic::shang_add ||
         ChainRoot->getIntrinsicID() == Intrinsic::shang_addc &&
         "Unexpected intrinsic instruction type!");

  typedef Instruction::op_iterator op_iterator;
  std::vector<std::pair<IntrinsicInst *, op_iterator> > VisitStack;

  VisitStack.push_back(std::make_pair(ChainRoot, ChainRoot->op_begin()));

  unsigned Depth = 0;
  std::set<IntrinsicInst *> Chain;
  while(!VisitStack.empty()) {
    IntrinsicInst *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    assert(CurNode->getIntrinsicID() == Intrinsic::shang_add ||
           CurNode->getIntrinsicID() == Intrinsic::shang_addc && "Unexpected type!");

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      if (Depth != 0) {
         Chain.insert(CurNode);
         Collected.insert(CurNode);
      }

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *I;
    ++I;
    IntrinsicInst *ChildInst = dyn_cast<IntrinsicInst>(ChildVal);

    if (!ChildInst)
      continue;

    if (Collected.count(ChildInst))
      continue;

    unsigned UsedByChainNum = 0;
    typedef Value::use_iterator use_iterator;
    for (use_iterator UI = ChildInst->use_begin(), UE = ChildInst->use_end(); UI != UE; ++UI) {
      Value *UserVal = *UI;

      if (IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(UserVal))
        ++UsedByChainNum;
    }
    if (UsedByChainNum >= 2)
      continue;

    if (ChildInst->getIntrinsicID() == Intrinsic::shang_add || ChildInst->getIntrinsicID() == Intrinsic::shang_addc) {
      VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
      Depth++;
    }
  }

  if (Depth != 0 && Chain.size() > 2) {
    ChainMap.insert(std::make_pair(ChainRoot, Chain));
    Collected.insert(ChainRoot);
  }
}

std::vector<Value *> SIRAddMulChain::eliminateIdenticalOperands(std::vector<Value *> Operands, Value *ChainRoot, unsigned BitWidth) {
  std::vector<Value *> FinalOperands;

  // Eliminate the same operands.
  std::map<Value *, unsigned> Op2Nums;
  for (unsigned i = 0; i < Operands.size(); ++i) {
    Value *Op = Operands[i];

    if (isa<ConstantInt>(Op)) {
      SM->indexKeepVal(Op);
      FinalOperands.push_back(Op);
      continue;
    }

    if (!Op2Nums.count(Op))
      Op2Nums.insert(std::make_pair(Op, 1));
    else
      Op2Nums[Op]++;
  }

  SIRDatapathBuilder Builder(SM, *TD);

  typedef std::map<Value *, unsigned>::iterator iterator;
  for (iterator I = Op2Nums.begin(), E = Op2Nums.end(); I != E; ++I) {
    Value *Op = I->first;
    unsigned OpBitWidth = Builder.getBitWidth(Op);
    unsigned Num = I->second;

    if (Num == 1) {
      SM->indexKeepVal(Op);
      FinalOperands.push_back(Op);
      continue;
    }

    if (Num == 2) {
      Value *ExtractResult = Builder.createSBitExtractInst(Op, OpBitWidth - 1, 0, Builder.createIntegerType(OpBitWidth - 1), ChainRoot, true);
      Value *ShiftResult = Builder.createSBitCatInst(ExtractResult, Builder.createIntegerValue(1, 0), Op->getType(), ChainRoot, true);

      SIRBitMask OpMask = SM->getBitMask(Op);
      OpMask = OpMask.shl(1);

      SM->IndexVal2BitMask(ShiftResult, OpMask);
      SM->indexKeepVal(ShiftResult);
      FinalOperands.push_back(ShiftResult);
    } else {
      llvm_unreachable("Not handled yet!");
    }
  }

  return FinalOperands;
}

std::vector<Value *> SIRAddMulChain::OptimizeOperands(std::vector<Value *> Operands, Value *ChainRoot, unsigned BitWidth) {
  // Eliminate the identical operands in add chain.
  std::vector<Value *> OptOperands = eliminateIdenticalOperands(Operands, ChainRoot, BitWidth);

  return OptOperands;
}

MatrixType SIRAddMulChain::createMatrixForOperands(std::vector<Value *> Operands, unsigned RowNum,
                                                   unsigned ColNum) {
  MatrixType Matrix;

  // Initial a empty matrix first.
  for (unsigned i = 0; i < RowNum; ++i) {
    MatrixRowType Row;

    for (unsigned j = 0; j < ColNum; ++j)
      Row.push_back(std::make_pair("1'b0", 0.0f));  

    Matrix.push_back(Row);
  }

  for (unsigned i = 0; i < RowNum; ++i) {
    Value *Operand = Operands[i];

    unsigned OpWidth = TD->getTypeSizeInBits(Operand->getType());
    std::string OpName = Operand->getName();

    // If the operand is a constant integer, then the dots will be
    // its binary representation.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(Operand)) {
      unsigned CIVal = CI->getZExtValue();

      for (unsigned j = 0; j < ColNum; ++j) {
        unsigned BaseVal = int(std::pow(double(2.0), double(ColNum - 1 - j)));
        unsigned BitVal = CIVal / (BaseVal);
        CIVal = (CIVal >= BaseVal) ? CIVal - BaseVal : CIVal;

        // Insert the binary representation to the matrix in reverse order.
        if (BitVal)
          Matrix[i][ColNum - 1 - j] = std::make_pair("1'b1", 0.0f);
        else
          Matrix[i][ColNum - 1 - j] = std::make_pair("1'b0", 0.0f);
      }
    }
    // Or the dots will be the form like operand[0], operand[1]...
    else {
      // Get the arrival time of the dots.
      float ArrivalTime;
      if (SIRRegister *Reg = SM->lookupSIRReg(Operand))
        ArrivalTime = 0.0f;
      else
        ArrivalTime = getOperandArrivalTime(Operand);

      // Get the name of the operand to denote the name of the dot later.
      std::string OpName = "operand_" + utostr_32(i);
      // Used to denote the sign bit of the operand if it exists.
      std::string SameBit;
      for (unsigned j = 0; j < ColNum; ++j) {
        // When the dot position is within the range of operand bit width,
        // we get the name of dot considering the bit mask.
        if (j < OpWidth) {
          // If it is a known bit, then use the known value.
          if (SM->hasBitMask(Operand)) {
            SIRBitMask Mask = SM->getBitMask(Operand);

            if (Mask.isOneKnownAt(j)) {
              Matrix[i][j] = std::make_pair("1'b1", 0.0f);
              continue;
            }
            else if (Mask.isZeroKnownAt(j)) {
              Matrix[i][j] = std::make_pair("1'b0", 0.0f);
              continue;
            } else if (Mask.isSameKnownAt(j)) {
              if (SameBit.size() != 0)
                Matrix[i][j] = std::make_pair(SameBit, ArrivalTime);
              else {
                SameBit = Mangle(OpName) + "[" + utostr_32(j) + "]";
                Matrix[i][j] = std::make_pair(SameBit, ArrivalTime);
              }
              continue;
            }
          }

          // Or use the form like operand[0], operand[1]...
          std::string DotName = Mangle(OpName) + "[" + utostr_32(j) + "]";
          Matrix[i][j] = std::make_pair(DotName, ArrivalTime);
        }
        // When the dot position is beyond the range of operand bit width,
        // we need to pad zero into the matrix.
        else {
          Matrix[i][j] = std::make_pair("1'b0", ArrivalTime);
        }
      }
    }    
  }

  return Matrix;
}

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

float SIRAddMulChain::getLatency(Instruction *Inst) {
  assert(Inst && "Unexpected SMGNode!");

  /// Get the delay of this node.
  float delay;

  // These instructions have not been transformed into SIR,
  // but clearly they cost no delay.
  if (isa<PtrToIntInst>(Inst) || isa<IntToPtrInst>(Inst) || isa<BitCastInst>(Inst))
    return 0.0f;

  // Otherwise it must be shang intrinsic instructions.
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst);
  assert(II && "Unexpected non-IntrinsicInst!");

  Intrinsic::ID ID = II->getIntrinsicID();

  switch (ID) {
    // Bit-level operations cost no delay.
  case Intrinsic::shang_bit_cat:
  case Intrinsic::shang_bit_repeat:
  case Intrinsic::shang_bit_extract:
    return 0.0f;

  case Intrinsic::shang_not: {
    if (isa<ConstantInt>(II->getOperand(0)))
      return 0.0f;

    return 0.00163f;
  }  

  case Intrinsic::shang_and: {
    // To be noted that, in LLVM IR the return value
    // is counted in Operands, so the real numbers
    // of operands should be minus one.
    unsigned IONums = II->getNumOperands() - 1;
    assert(IONums == 2 && "Unexpected Num!");

    if (isa<ConstantInt>(II->getOperand(0)) || isa<ConstantInt>(II->getOperand(1)))
      return 0.0f;

    if (IntrinsicInst *OpII = dyn_cast<IntrinsicInst>(II->getOperand(0))) {
      if (OpII->getIntrinsicID() == Intrinsic::shang_not) {
        if (isa<ConstantInt>(OpII->getOperand(0)))
          return 0.0f;
      }
    }

    if (IntrinsicInst *OpII = dyn_cast<IntrinsicInst>(II->getOperand(1))) {
      if (OpII->getIntrinsicID() == Intrinsic::shang_not) {
        if (isa<ConstantInt>(OpII->getOperand(0)))
          return 0.0f;
      }
    }

    return 0.01422f;
  }
  case Intrinsic::shang_or: {
    // To be noted that, in LLVM IR the return value
    // is counted in Operands, so the real numbers
    // of operands should be minus one.
    unsigned IONums = II->getNumOperands() - 1;
    assert(IONums == 2 && "Unexpected Num!");

    if (isa<ConstantInt>(II->getOperand(0)) || isa<ConstantInt>(II->getOperand(1)))
      return 0.0f;

    if (IntrinsicInst *OpII = dyn_cast<IntrinsicInst>(II->getOperand(0))) {
      if (OpII->getIntrinsicID() == Intrinsic::shang_not) {
        if (isa<ConstantInt>(OpII->getOperand(0)))
          return 0.0f;
      }
    }

    if (IntrinsicInst *OpII = dyn_cast<IntrinsicInst>(II->getOperand(1))) {
      if (OpII->getIntrinsicID() == Intrinsic::shang_not) {
        if (isa<ConstantInt>(OpII->getOperand(0)))
          return 0.0f;
      }
    }

    return 0.01899f;
  }
  case Intrinsic::shang_xor: {
    // To be noted that, in LLVM IR the return value
    // is counted in Operands, so the real numbers
    // of operands should be minus one.
    unsigned IONums = II->getNumOperands() - 1;
    assert(IONums == 2 && "Unexpected Num!");

    if (isa<ConstantInt>(II->getOperand(0)) || isa<ConstantInt>(II->getOperand(1)))
      return 0.0f;

    if (IntrinsicInst *OpII = dyn_cast<IntrinsicInst>(II->getOperand(0))) {
      if (OpII->getIntrinsicID() == Intrinsic::shang_not) {
        if (isa<ConstantInt>(OpII->getOperand(0)))
          return 0.0f;
      }
    }

    if (IntrinsicInst *OpII = dyn_cast<IntrinsicInst>(II->getOperand(1))) {
      if (OpII->getIntrinsicID() == Intrinsic::shang_not) {
        if (isa<ConstantInt>(OpII->getOperand(0)))
          return 0.0f;
      }
    }

    return 0.01887f;
  }
  case Intrinsic::shang_rand: {
    unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
    return LuaI::Get<VFURAnd>()->lookupLatency(std::min(BitWidth, 64u));
  }

  case Intrinsic::shang_add:
  case Intrinsic::shang_addc: {
    unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
    return LuaI::Get<VFUAddSub>()->lookupLatency(std::min(BitWidth, 64u));
  }
  case Intrinsic::shang_mul: {
    unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
    return LuaI::Get<VFUMult>()->lookupLatency(std::min(BitWidth, 64u));
  }

  case Intrinsic::shang_sdiv:
  case Intrinsic::shang_udiv: {
    unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
    return LuaI::Get<VFUDiv>()->lookupLatency(std::min(BitWidth, 64u));
  }

  case Intrinsic::shang_shl:
  case Intrinsic::shang_ashr:
  case Intrinsic::shang_lshr: {
    if (isa<ConstantInt>(II->getOperand(1)))
      return 0.0f;

    unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
    return LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));
  }

  case Intrinsic::shang_sgt:
  case Intrinsic::shang_ugt: {
    unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
    return LuaI::Get<VFUICmp>()->lookupLatency(std::min(BitWidth, 64u));
  }

  default:
    llvm_unreachable("Unexpected opcode!");
  }
}

float SIRAddMulChain::getOperandArrivalTime(Value *Operand) {
  SM->indexKeepVal(Operand);

  if (isLeafValue(SM, Operand))
    return 0.0f;

  // If we already calculate the arrival time before, then we just
  // return the result.
  if (ValArrivalTime.count(Operand))
    return ValArrivalTime[Operand];

  std::map<SIRRegister *, float> ArrivalTimes;

  Instruction *Root = dyn_cast<Instruction>(Operand);

  typedef Instruction::op_iterator iterator;
  std::vector<std::pair<Instruction *, iterator> > VisitStack;

  float delay = getLatency(Root);

  VisitStack.push_back(std::make_pair(Root, Root->op_begin()));
  while(!VisitStack.empty()) {
    Instruction *Node = VisitStack.back().first;
    iterator &It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();

      delay -= getLatency(Node);
      continue;
    }

    Value *ChildNode = *It;
    ++It;

    if (Instruction *ChildInst = dyn_cast<Instruction>(ChildNode)) {
      SM->indexKeepVal(ChildNode);

      if (SIRRegister *Reg = SM->lookupSIRReg(ChildInst)) {
        if (ArrivalTimes.count(Reg))
          ArrivalTimes[Reg] = std::max(ArrivalTimes[Reg], delay);
        else
          ArrivalTimes[Reg] = delay;

        continue;
      }

      if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(ChildInst))
        if (II->getIntrinsicID() == Intrinsic::shang_compressor) {
          delay += getOperandArrivalTime(ChildInst);

          if (ArrivalTimes.count(NULL))
            ArrivalTimes[NULL] = std::max(ArrivalTimes[NULL], delay);
          else
            ArrivalTimes[NULL] = delay;

          continue;
        }

      if (isa<IntrinsicInst>(ChildInst) || isa<PtrToIntInst>(ChildInst) ||
          isa<IntToPtrInst>(ChildInst) || isa<BitCastInst>(ChildInst)) {
         VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
         delay += getLatency(ChildInst);
      }      
    }
  }

  float ArrivalTime = 0.0f;
  typedef std::map<SIRRegister *, float>::iterator map_iterator;
  for (map_iterator MI = ArrivalTimes.begin(), ME = ArrivalTimes.end(); MI != ME; ++MI)
    ArrivalTime = std::max(ArrivalTime, MI->second);

  // Index the valid time to the value.
  ValArrivalTime.insert(std::make_pair(Operand, ArrivalTime));

  return ArrivalTime;
}

void SIRAddMulChain::generateDotMatrix() {
  std::string CompressorOutputPath = LuaI::GetString("CompressorOutput");
  std::string Error;
  raw_fd_ostream Output(CompressorOutputPath.c_str(), Error);

  typedef std::map<IntrinsicInst *, std::set<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    generateDotmatrixForChain(I->first, Output);
  }

  //// Generate the 3-2 compressor.
  //Output << "module compressor_3_2 ( a, b, cin, result, cout );\n";
  //Output << "input a, b, cin;\n";
  //Output << "output result, cout;\n";
  //Output << "wire   n2, n3, n4;\n";
  //Output << "AOI2BB2X2 U4 ( .B0(b), .B1(a), .A0N(b), .A1N(a), .Y(n4) );\n";
  //Output << "NAND2X1 U5 ( .A(b), .B(a), .Y(n3) );\n";
  //Output << "NAND2X1 U6 ( .A(n4), .B(cin), .Y(n2) );\n";
  //Output << "NAND2X1 U7 ( .A(n3), .B(n2), .Y(cout) );\n";
  //Output << "AOI2BB2X1 U8 ( .B0(n4), .B1(cin), .A0N(n4), .A1N(cin), .Y(result) );";
  //Output << "endmodule\n\n";

  //// Generate the 2-2 compressor.
  //Output << "module compressor_2_2 ( a, b, result, cout );\n";
  //Output << "input a, b;\n";
  //Output << "output result, cout;\n";
  //Output << "AND2X1 U3 ( .A(b), .B(a), .Y(cout) );\n";
  //Output << "AOI2BB1X1 U4 ( .A0N(b), .A1N(a), .B0(cout), .Y(result) );\n";
  //Output << "endmodule\n\n";

   // Generate the 3-2 compressor.
   Output << "module compressor_3_2 ( a, b, cin, result, cout );\n";
   Output << "input a, b, cin;\n";
   Output << "output result, cout;\n";
   Output << "assign result = a ^ b ^ cin;\n";
   Output << "assign cout = (a & b) | (a & cin) | (b & cin);\n";
   Output << "endmodule\n\n";
 
   // Generate the 2-2 compressor.
   Output << "module compressor_2_2 ( a, b, result, cout );\n";
   Output << "input a, b;\n";
   Output << "output result, cout;\n";
   Output << "assign result = a ^ b;\n";
   Output << "assign cout = a & b;\n";
   Output << "endmodule\n\n";
}

void SIRAddMulChain::generateDotmatrixForChain(IntrinsicInst *ChainRoot, raw_fd_ostream &Output) {
  assert(ChainMap.count(ChainRoot) && "Not a chain rooted on ChainRoot!");
  std::set<IntrinsicInst *> &Chain = ChainMap[ChainRoot];

  // Extract all operands added by the Chain.
  std::vector<Value *> Operands;
  typedef std::set<IntrinsicInst *>::iterator iterator;
  for (iterator I = Chain.begin(), E = Chain.end(); I != E; ++I) {
    IntrinsicInst *ChainInst = *I;

    for (unsigned i = 0; i < ChainInst->getNumOperands() - 1; ++i) {
      Value *Operand = ChainInst->getOperand(i);

      // Ignore the chain instruction itself.
      if (IntrinsicInst *OperandInst = dyn_cast<IntrinsicInst>(Operand)) {
        if (Chain.count(OperandInst))
          continue;
      }

      Operands.push_back(Operand);
    }
  }

  // Optimize operands if there are known same sign bits in two or more operands.
  std::vector<Value *> OptOperands = Operands;
  OptOperands = OptimizeOperands(Operands, ChainRoot, TD->getTypeSizeInBits(ChainRoot->getType()));

  IntrinsicInst *Compressor = ChainRoot2Compressor[ChainRoot];
  SM->IndexOps2AdderChain(Compressor, OptOperands);

  unsigned BitWidth = 0;
  for (unsigned i = 0; i < OptOperands.size(); ++i) {
    BitWidth = BitWidth + TD->getTypeSizeInBits(OptOperands[i]->getType());
  }

  SIRDatapathBuilder Builder(SM, *TD);
  Value *PesudoOp = Builder.createSBitCatInst(OptOperands, Builder.createIntegerType(BitWidth), ChainRoot, true);
  Compressor->setOperand(0, PesudoOp);

  // Generate all elements in Dot Matrix.
  unsigned MatrixRowNum = OptOperands.size();
  unsigned MatrixColNum = TD->getTypeSizeInBits(ChainRoot->getType());

  // Print the declaration of the module.
  IntrinsicInst *CompressorInst = ChainRoot2Compressor[ChainRoot];
  std::string MatrixName = Mangle(CompressorInst->getName());
  Output << "module " << "compressor_" << MatrixName << "(\n";
  for (unsigned i = 0; i < MatrixRowNum; ++i) {
    Output << "\t(* altera_attribute = \"-name VIRTUAL_PIN on\" *) input wire[";
    Output << utostr_32(MatrixColNum - 1) << ":0] operand_" << utostr_32(i) << ",\n";
  }
  Output << "\t(* altera_attribute = \"-name VIRTUAL_PIN on\" *) output wire[";
  Output << utostr_32(MatrixColNum - 1) << ":0] result\n);\n\n";

  MatrixType Matrix = createMatrixForOperands(OptOperands, MatrixRowNum, MatrixColNum);
  printTMatrixForDebug(Matrix);

  errs() << "Operands for hybrid tree " << MatrixName << "is indexed as:\n";
  for (unsigned i = 0; i < OptOperands.size(); ++i) {
    std::stringstream ss;
    ss << getOperandArrivalTime(OptOperands[i]);

    std::string delay_string;
    ss >> delay_string;
    ss.clear();
    errs() << "[" + utostr_32(i) + "--" << delay_string << "],";
  }
  errs() << "\n";

  // Optimize the Matrix taking advantage of the sign bits.
  Matrix = sumAllSignBitsInMatrix(Matrix, MatrixRowNum, MatrixColNum);
  MatrixRowNum = Matrix.size();
  printTMatrixForDebug(Matrix);

  // Optimize the Matrix taking advantage of the known bits.
  MatrixType TMatrix = transportMatrix(Matrix, MatrixRowNum, MatrixColNum);
  //printTMatrixForDebug(TMatrix);
  TMatrix = sumAllOneBitsInTMatrix(TMatrix);
  ++MatrixRowNum;
  printTMatrixForDebug(TMatrix);

  Matrix = transportMatrix(TMatrix, MatrixColNum, MatrixRowNum);
  printTMatrixForDebug(Matrix);
  
  float ResultArrivalTime = hybridTreeCodegen(Matrix, MatrixName, MatrixRowNum, MatrixColNum, Output);

  // Index the arrival time of the hybrid tree result.
  ValArrivalTime.insert(std::make_pair(Compressor, ResultArrivalTime));
}

bool SIRAddMulChain::isConstantInt(MatrixRowType Row) {
  bool IsConstantInt = true;

  for (unsigned i = 0; i < Row.size(); ++i) {
    DotType Dot = Row[i];

    if (Dot.first != "1'b0" && Dot.first != "1'b1") {
      IsConstantInt = false;
      break;
    }
  }

  return IsConstantInt;
}

unsigned SIRAddMulChain::getOperandBitWidth(MatrixRowType Row) {
  unsigned BitWidth = 0;
  bool HeadingZero = true;
  for (unsigned i = 0; i < Row.size(); ++i) {
    if (Row[Row.size() - 1 - i].first == "1\'b0" && HeadingZero)
      continue;
    else {
      ++BitWidth;
      HeadingZero = false;
    }
  }

  return BitWidth;
}

std::vector<unsigned> SIRAddMulChain::getSignBitNumListInMatrix(MatrixType Matrix) {
  std::vector<unsigned> SignBitNumList;

  for (unsigned i = 0; i < Matrix.size(); ++i) {
    MatrixRowType Row = Matrix[i];

    // If this row is a integer, then there are only one sign bit really.
    if (isConstantInt(Row)) {
      SignBitNumList.push_back(1);
      continue;
    }

    // Extract the MSB as it is sign bit really.
    DotType SignBit = Row.back();

    // Ignore the 0 sign bit.
    if (SignBit.first == "1'b0") {
      SignBitNumList.push_back(1);
      continue;
    }

    // Then traverse the row from MSB to LSB and these bits that same as the
    // SignBit will be counted as SignBit.
    unsigned SignBitNum = 0;
    for (unsigned j = 0; j < Row.size(); ++j) {
      if (Row[Row.size() - 1 - j] == SignBit)
        ++SignBitNum;
      else
        break;
    }

    SignBitNumList.push_back(SignBitNum);
  }

  return SignBitNumList;
}

MatrixType SIRAddMulChain::sumAllSignBitsInMatrix(MatrixType Matrix, unsigned RowNum,
                                                  unsigned ColumnNum) {
  // Get the number of sign bit in each row in Matrix.
  std::vector<unsigned> SignBitNumList = getSignBitNumListInMatrix(Matrix);

  // The smallest sign bit width of all rows in Matrix.
  unsigned SignBitPatternWidth = UINT_MAX;
  for (unsigned i = 0; i < SignBitNumList.size(); ++i) {
    // Only one sign bit existed is not the pattern we looking for.
    if (SignBitNumList[i] != 1)
      SignBitPatternWidth = std::min(SignBitPatternWidth, SignBitNumList[i]);
  }

  // If there are no sign bit pattern, return the origin Matrix.
  if (SignBitPatternWidth == UINT_MAX)
    return Matrix;

  // Sum all sign bit using the equation:
  // ssssssss = 11111111 + 0000000~s,
  // then sum all the one bit.
  MatrixType SignBitMatrix = Matrix;
  for (unsigned i = 0; i < Matrix.size(); ++i) {
    // If there is sign bit pattern in current row.
    if (SignBitNumList[i] >= SignBitPatternWidth) {
      unsigned SignBitStartPoint = Matrix[i].size() - SignBitPatternWidth;

      // Set the ssssssss to 0000000~s in origin Matrix.
      DotType OriginDot = Matrix[i][SignBitStartPoint];
      Matrix[i][SignBitStartPoint] = std::make_pair("~" + OriginDot.first, OriginDot.second);
      for (unsigned j = SignBitStartPoint + 1; j < Matrix[i].size(); ++j)
        Matrix[i][j] = std::make_pair("1'b0", 0.0f);

      // Set the non-sign bit to 00000000 in sign bit Matrix.
      for (unsigned j = 0; j < SignBitStartPoint; ++j)
        SignBitMatrix[i][j] = std::make_pair("1'b0", 0.0f);
      // Set the ssssssss to 11111111 in sign bit Matrix.
      for (unsigned j = SignBitStartPoint; j < SignBitMatrix[i].size(); ++j)
        SignBitMatrix[i][j] = std::make_pair("1'b1", 0.0f);
    }
    // If there is no sign bit pattern in current row.
    else {
      // Set all bits to 00000000 in sign bit Matrix.
      for (unsigned j = 0; j < SignBitMatrix[i].size(); ++j)
        SignBitMatrix[i][j] = std::make_pair("1'b0", 0.0f);
    }
  }

  MatrixType SignBitTMatrix = transportMatrix(SignBitMatrix, RowNum, ColumnNum);
  SignBitTMatrix = sumAllOneBitsInTMatrix(SignBitTMatrix);

  SignBitMatrix = transportMatrix(SignBitTMatrix, ColumnNum, ++RowNum);  
  MatrixRowType Row = SignBitMatrix[0];
  Matrix.push_back(SignBitMatrix[0]);

  return Matrix;
}

MatrixType SIRAddMulChain::transportMatrix(MatrixType Matrix, unsigned RowNum, unsigned ColumnNum) {
  MatrixType TMatrix;

  for (unsigned j = 0; j < ColumnNum; ++j) {
    MatrixRowType TRow;

    for (unsigned i = 0; i < Matrix.size(); ++i) {
      MatrixRowType Row = Matrix[i];

      TRow.push_back(Row[j]);
    }

    TMatrix.push_back(TRow);
  }
  
  return TMatrix;
}

std::vector<unsigned> SIRAddMulChain::getOneBitNumListInTMatrix(MatrixType TMatrix) {
  std::vector<unsigned> OneBitNumList;

  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType TRow = TMatrix[i];

    unsigned OneBitNum = 0;
    for (unsigned j = 0; j < TRow.size(); ++j) {
      if (TRow[j].first == "1'b1")
        ++OneBitNum;
    }

    OneBitNumList.push_back(OneBitNum);
  }

  return OneBitNumList;
}

std::vector<unsigned> SIRAddMulChain::getBitNumListInTMatrix(MatrixType TMatrix) {
  std::vector<unsigned> BitNumList;

  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType Row = TMatrix[i];

    // If there are no dot in current row.
    if (Row.size() == 0) {
      BitNumList.push_back(0);
      continue;
    }

    unsigned BitNum = 0;
    for (unsigned j = 0; j < Row.size(); ++j) {
      if (Row[j].first != "1'b0")
        ++BitNum;
    }

    BitNumList.push_back(BitNum);   
  }

  return BitNumList;
}

MatrixType SIRAddMulChain::simplifyTMatrix(MatrixType TMatrix) {
  // Eliminate the useless 1'b0 and its delay-stage info
  MatrixType SimplifiedTMatrix;

  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType SimplifiedTRow;

    MatrixRowType TRow = TMatrix[i];

    for (unsigned j = 0; j < TRow.size(); ++j) {
      DotType Dot = TRow[j];

      if (Dot.first != "1'b0" && Dot.first != "~1'b1")
        SimplifiedTRow.push_back(Dot);
    }

    SimplifiedTMatrix.push_back(SimplifiedTRow);
  }

  return SimplifiedTMatrix;
}

bool DotCompare(const DotType &DotA, const DotType &DotB) {
  float DotADelay = DotA.second;
  float DotBDelay = DotB.second;

  if (DotADelay < DotBDelay)
    return true;
  else
    return false;
}

MatrixType SIRAddMulChain::sortTMatrix(MatrixType TMatrix) {
  // Sort the bits according to #1: stage, #2: delay.
  MatrixType SortedTMatrix;

  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType SimplifiedTRow = TMatrix[i];

    std::sort(SimplifiedTRow.begin(), SimplifiedTRow.end(), DotCompare);

    SortedTMatrix.push_back(SimplifiedTRow);
  }

  return SortedTMatrix;
}

MatrixType SIRAddMulChain::sumAllOneBitsInTMatrix(MatrixType TMatrix) {
  // Get the number of one bit in each row in TMatrix.
  std::vector<unsigned> OneBitNumList = getOneBitNumListInTMatrix(TMatrix);

  // The number of one bit in each row after sum.
  std::vector<unsigned> OneBitNumAfterSumList;
  for (unsigned i = 0; i < OneBitNumList.size(); ++i) {
    unsigned OneBitNumInCurrentRow = OneBitNumList[i];

    OneBitNumAfterSumList.push_back(OneBitNumInCurrentRow % 2);
    unsigned CarryOneNum = OneBitNumInCurrentRow / 2;

    if (i != OneBitNumList.size() - 1)
      OneBitNumList[i + 1] += CarryOneNum;
  }

  /// Insert the one bit after sum to TMatrix and eliminate all origin one bits.
  MatrixType TMatrixAfterSum;

  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType TRowAfterSum;

    // Insert the one bit.
    std::string DotName;
    if (OneBitNumAfterSumList[i] == 0)
      DotName = "1'b0";
    else
      DotName = "1'b1";

    TRowAfterSum.push_back(std::make_pair(DotName, 0.0f));

    // Insert other origin bits other than one bits which will be eliminated.
    MatrixRowType TRow = TMatrix[i];
    for (unsigned j = 0; j < TRow.size(); ++j) {
      if (TRow[j].first != "1'b1")
        TRowAfterSum.push_back(TRow[j]);
      else
        TRowAfterSum.push_back(std::make_pair("1'b0", 0.0f));
    }

    TMatrixAfterSum.push_back(TRowAfterSum);
  }

  return TMatrixAfterSum;
}

MatrixType SIRAddMulChain::eliminateOneBitInTMatrix(MatrixType TMatrix) {
  return TMatrix;

  // Simplify and sort the TMatrix to prepare for the eliminating.
  TMatrix = simplifyTMatrix(TMatrix);
  TMatrix = sortTMatrix(TMatrix);

  // Eliminate the 1'b1 in TMatrix using the equation:
  // 1'b1 + 1'bs = 2'bs~s
  std::vector<unsigned> OneBitNumList = getOneBitNumListInTMatrix(TMatrix);
  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType Row = TMatrix[i];

    unsigned OneBitNum = OneBitNumList[i];
    if (OneBitNum == 0 || Row.size() <= 1)
      continue;

    assert(Row[0].first == "1'b1" && "Unexpected Bit!");
    assert(Row[1].first != "1'b0" && Row[1].first != "1'b1" && "Unexpected Bit!");

    std::string SumName = "~" + Row[1].first;
    std::string CarryName = Row[1].first;

    TMatrix[i][0] = std::make_pair("1'b0", 0.0f);
    TMatrix[i][1] = std::make_pair("1'b0", 0.0f);

    TMatrix[i].push_back(std::make_pair(SumName, Row[1].second));
    if (i + 1 < TMatrix.size())
      TMatrix[i + 1].push_back(std::make_pair(CarryName, Row[1].second));
  }

  return TMatrix;
}

std::pair<float, float>
  SIRAddMulChain::generateCompressor(std::vector<DotType> CompressCouple,
                                     std::string SumName, std::string CoutName,
                                     raw_fd_ostream &Output) {
  float SumDelay = 0.0f;
  float CoutDelay = 0.0f;

  if (CompressCouple.size() == 3) {
    // Print the declaration of the result.
    Output << "wire " << SumName << ";\n";
    Output << "wire " << CoutName << ";\n";

    // Print the instantiation of the compressor module.
    Output << "compressor_3_2 compressor_3_2_" << utostr_32(COMPRESSOR_NUM) << "( ";

    // Link the wire according to the path delay of compressor, since the delay order is listed as
    // col[1] < col[0] < col[2].
    Output << ".a(" << CompressCouple[0].first << "), .b(" << CompressCouple[1].first << "), .cin("
           << CompressCouple[2].first << "), .result(" << SumName << "), .cout(" << CoutName << ") );\n\n";

    // Calculate the output delay.
    float SumDelay_0 = CompressCouple[1].second + COMPRESSOR_3_2_DELAY[0][0];
    float SumDelay_1 = CompressCouple[2].second + COMPRESSOR_3_2_DELAY[0][1];
    float SumDelay_2 = CompressCouple[0].second + COMPRESSOR_3_2_DELAY[0][2];

    float CoutDelay_0 = CompressCouple[1].second + COMPRESSOR_3_2_DELAY[1][0];
    float CoutDelay_1 = CompressCouple[2].second + COMPRESSOR_3_2_DELAY[1][1];
    float CoutDelay_2 = CompressCouple[0].second + COMPRESSOR_3_2_DELAY[1][2];

    SumDelay = std::max(SumDelay_0, std::max(SumDelay_1, SumDelay_2));
    CoutDelay = std::max(CoutDelay_0, std::max(CoutDelay_1, CoutDelay_2));
  } else {
    // Print the declaration of the result.
    Output << "wire " << SumName << ";\n";
    Output << "wire " << CoutName << ";\n";

    // Print the instantiation of the compressor module.
    Output << "compressor_2_2 compressor_2_2_" << utostr_32(COMPRESSOR_NUM) << "( ";

    // Link the wire according to the path delay of compressor, since the delay order is listed as
    // col[1] < col[0].
    Output << ".a(" << CompressCouple[0].first << "), .b(" << CompressCouple[1].first 
           << "), .result(" << SumName << "), .cout(" << CoutName << ") );\n\n";

    // Calculate the output delay.
    float SumDelay_0 = CompressCouple[1].second + COMPRESSOR_2_2_DELAY[0][0];
    float SumDelay_1 = CompressCouple[0].second + COMPRESSOR_2_2_DELAY[0][1];

    float CoutDelay_0 = CompressCouple[1].second + COMPRESSOR_2_2_DELAY[1][0];
    float CoutDelay_1 = CompressCouple[0].second + COMPRESSOR_2_2_DELAY[1][1];

    SumDelay = std::max(SumDelay_0, SumDelay_1);
    CoutDelay = std::max(CoutDelay_0, CoutDelay_1);
  }  

  ++COMPRESSOR_NUM;

  return std::make_pair(SumDelay, CoutDelay);
}

bool SIRAddMulChain::needToCompress(std::vector<unsigned> BitNumList, unsigned RowNo) {
  if (BitNumList[RowNo] == 2) {
    if (RowNo == 0)
      return false;

    if (BitNumList[RowNo - 1] <= 2)
      return false;

    if (BitNumList[RowNo - 1] == 3) {
      bool NeedToCompress = true;
      for (int j = RowNo - 2; j >= 0; --j) {
        if (BitNumList[j] > 3) {
          NeedToCompress = false;
          break;
        }
      }

      return NeedToCompress;
    }
    
    if (BitNumList[RowNo - 1] == 4) {
      bool NeedToCompress = true;
      for (int j = RowNo - 2; j >= 0; --j) {
        if (BitNumList[j] >= 3) {
          NeedToCompress = false;
          break;
        }
      }

      return NeedToCompress;
    }

    return false;
  }

  if (BitNumList[RowNo] == 3) {
    bool NeedToCompress = true;
    for (int j = RowNo - 1; j >= 0; --j) {
      if (BitNumList[j] >= 3) {
        NeedToCompress = false;
        break;
      }
    }

    return NeedToCompress;
  }

  return false;
}

MatrixType SIRAddMulChain::compressTMatrixInStage(MatrixType TMatrix, unsigned Stage, raw_fd_ostream &Output) {
  // Get the informations of the TMatrix.
  std::vector<unsigned> BitNumList = getBitNumListInTMatrix(TMatrix);

  // Compress the final row using XOR gate.
  if (BitNumList[TMatrix.size() - 1] >= 2) {
    std::vector<DotType> CompressCouple;

    // Collect the compressed bits into CompressCouple and clear the compressed bits in TMatrix.
    for (unsigned i = 0; i < BitNumList[TMatrix.size() - 1]; ++i) {
      CompressCouple.push_back(TMatrix.back()[i]);
      TMatrix.back()[i] = std::make_pair("1'b0", 0.0f);
    }

    // Get the information of the result.
    std::string ResultName = "result_" + utostr_32(TMatrix.size() - 1) + "_" + utostr_32(Stage);

    float ResultDelay = 0.0f;
    for (unsigned i = 0; i < CompressCouple.size(); ++i) {
      ResultDelay = std::max(ResultDelay, CompressCouple[i].second);
    }

    // Insert the result into TMatrix.
    TMatrix.back().push_back(std::make_pair(ResultName, 0.0f));

    // Generate the XOR gate to compress the final row.
    Output << "wire " << ResultName << " = ";
    for (unsigned i = 0; i < CompressCouple.size(); ++i) {
      Output << CompressCouple[i].first;

      if (i != CompressCouple.size() - 1)
        Output << " ^ ";
    }
    Output << ";\n\n";

    // After compress this row, do some clean up and optimize work.
    TMatrix = eliminateOneBitInTMatrix(TMatrix);
    TMatrix = simplifyTMatrix(TMatrix);
    TMatrix = sortTMatrix(TMatrix);
  }

  // Compress row by row. To be noted that, the last row is ignored since it can be
  // compressed using XOR gate.
  for (unsigned i = 0; i < TMatrix.size() - 1; ++i) {
    // Compress current row if it has more than target final bit numbers.
    if (BitNumList[i] >= 3) {
      // The couple of bits to be compressed.
      std::vector<DotType> CompressCouple;
      CompressCouple.push_back(TMatrix[i][0]);
      CompressCouple.push_back(TMatrix[i][1]);
      CompressCouple.push_back(TMatrix[i][2]);

      // Clear the compressed bits in TMatrix.
      TMatrix[i][0] = std::make_pair("1'b0", 0.0f);
      TMatrix[i][1] = std::make_pair("1'b0", 0.0f);
      TMatrix[i][2] = std::make_pair("1'b0", 0.0f);

      // Get the information of the result.
      std::string SumName = "sum_3_2" + utostr_32(i) + "_" + utostr_32(Stage);
      std::string CoutName = "cout_3_2" + utostr_32(i) + "_" + utostr_32(Stage);

      // Generate the compressor.
      std::pair<float, float> ResultDelay
        = generateCompressor(CompressCouple, SumName, CoutName, Output);

      // Insert the result into TMatrix.
      TMatrix[i].push_back(std::make_pair(SumName, ResultDelay.first));
      if (i + 1 < TMatrix.size())
        TMatrix[i + 1].push_back(std::make_pair(CoutName, ResultDelay.second));

      printTMatrixForDebug(TMatrix);
    }

    // Do some clean up and optimize work.
    TMatrix = eliminateOneBitInTMatrix(TMatrix);
    TMatrix = simplifyTMatrix(TMatrix);
    TMatrix = sortTMatrix(TMatrix);

    // Update the informations of the TMatrix.
    BitNumList = getBitNumListInTMatrix(TMatrix);

    if (BitNumList[i] == 2 && needToCompress(BitNumList, i)) {
      // The couple of bits to be compressed.
      std::vector<DotType> CompressCouple;
      CompressCouple.push_back(TMatrix[i][0]);
      CompressCouple.push_back(TMatrix[i][1]);

      // Clear the compressed bits in TMatrix.
      TMatrix[i][0] = std::make_pair("1'b0", 0.0f);
      TMatrix[i][1] = std::make_pair("1'b0", 0.0f);

      // Get the information of the result.
      std::string SumName = "sum_2_2" + utostr_32(i) + "_" + utostr_32(Stage);
      std::string CoutName = "cout_2_2" + utostr_32(i) + "_" + utostr_32(Stage);

      // Generate the compressor.
      std::pair<float, float> ResultDelay
        = generateCompressor(CompressCouple, SumName, CoutName, Output);

      // Insert the result into TMatrix.
      TMatrix[i].push_back(std::make_pair(SumName, ResultDelay.first));
      if (i + 1 < TMatrix.size())
        TMatrix[i + 1].push_back(std::make_pair(CoutName, ResultDelay.second));

      printTMatrixForDebug(TMatrix);
    }

    // After compress this row, do some clean up and optimize work.
    TMatrix = eliminateOneBitInTMatrix(TMatrix);
    TMatrix = simplifyTMatrix(TMatrix);
    TMatrix = sortTMatrix(TMatrix);

    // Update the informations of the TMatrix.
    BitNumList = getBitNumListInTMatrix(TMatrix);
  }

  return TMatrix;
}

float SIRAddMulChain::compressMatrix(MatrixType TMatrix, std::string MatrixName,
                                     unsigned OperandNum, unsigned OperandWidth,
                                     raw_fd_ostream &Output) {
  printTMatrixForDebug(TMatrix);

  /// Prepare for the compress progress
  // Sum all one bits in TMatrix.
  TMatrix = sumAllOneBitsInTMatrix(TMatrix);
  // Eliminate the one bit in TMatrix.
  TMatrix = eliminateOneBitInTMatrix(TMatrix);
  // Simplify the TMatrix.
  TMatrix = simplifyTMatrix(TMatrix);
  // Sort the TMatrix.
  TMatrix = sortTMatrix(TMatrix);

  printTMatrixForDebug(TMatrix);

  /// Start to compress the TMatrix
  bool Continue = true;
  unsigned Stage = 0;
  while (Continue) {
    TMatrix = compressTMatrixInStage(TMatrix, Stage, Output);

    // Determine if we need to continue compressing.
    std::vector<unsigned> BitNumList = getBitNumListInTMatrix(TMatrix);
    Continue = false;
    for (unsigned i = 0; i < TMatrix.size(); ++i) {
      if (BitNumList[i] > 2)
        Continue = true;
    }

    // Increase the stage and start next compress progress.
    if (Continue)
      ++Stage;
  }

  /// Finish the compress by sum the left-behind bits using CPA.
  MatrixRowType CPADataA, CPADataB;
  float CPADataA_ArrivalTime = 0.0f;
  float CPADataB_ArrivalTime = 0.0f;
  
  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    CPADataA.push_back(TMatrix[i][0]);
    CPADataA_ArrivalTime = std::max(CPADataA_ArrivalTime, TMatrix[i][0].second);

    if (TMatrix[i].size() < 2)
      CPADataB.push_back(std::make_pair("1'b0", 0.0f));
    else {
      CPADataB.push_back(TMatrix[i][1]);
      CPADataB_ArrivalTime = std::max(CPADataB_ArrivalTime, TMatrix[i][1].second);
    }
  }
  assert(CPADataA.size() == CPADataB.size() && "Should be same size!");

  // Print the declaration and definition of DataA & DataB of CPA.
  Output << "wire[" << utostr_32(CPADataA.size() - 1) << ":0] CPA_DataA = {";
  for (unsigned i = 0; i < CPADataA.size(); ++i) {
    Output << CPADataA[CPADataA.size() - 1 - i].first;

    if (i != CPADataA.size() - 1)
      Output << ", ";
  }
  Output << "};\n";

  Output << "wire[" << utostr_32(CPADataB.size() - 1) << ":0] CPA_DataB = {";
  for (unsigned i = 0; i < CPADataB.size(); ++i) {
    Output << CPADataB[CPADataB.size() - 1 - i].first;

    if (i != CPADataB.size() - 1)
      Output << ", ";
  }
  Output << "};\n";

  // Print the implementation of the CPA.
  Output << "wire[" << utostr_32(CPADataA.size() - 1) << ":0] CPA_Result = CPA_DataA + CPA_DataB;\n";

  // Print the implementation of the result.
  Output << "assign result = CPA_Result;\n";

  // Print the end of module.
  Output << "\nendmodule\n\n";

  // Index the arrival time of the compressor result.
  float CPADelay;
  if (CPADataA.size() == 16)
    CPADelay = ADD_16_DELAY;
  else if (CPADataA.size() == 32)
    CPADelay = ADD_32_DELAY;
  else if (CPADataA.size() == 64)
    CPADelay = ADD_64_DELAY;

  float ResultArrivalTime = std::max(CPADataA_ArrivalTime, CPADataB_ArrivalTime) + CPADelay;

  return ResultArrivalTime;
}

void SIRAddMulChain::printTMatrixForDebug(MatrixType TMatrix) {
  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType Row = TMatrix[i];

    for (unsigned j = 0; j < Row.size(); ++j) {
      DotType Dot = Row[j];

      DebugOutput << Dot.first << "--" << Dot.second;

      if (j != Row.size() - 1)
        DebugOutput << "  ";
    }

    DebugOutput << "\n";
  }

  DebugOutput << "\n\n";
}

bool OperandCompare(std::pair<unsigned, float> OpA, std::pair<unsigned, float> OpB) {
  return OpA.second < OpB.second;
}

float SIRAddMulChain::predictAddChainResultTime(std::vector<std::pair<unsigned, float> > AddChain,
                                                MatrixType Matrix) {
  if (AddChain.size() == 1)
    return AddChain[0].second;

  // Sort the operands of add chain in ascending order of delay.
  std::sort(AddChain.begin(), AddChain.end(), OperandCompare);

  // Add the first two operands, and insert the result into chain until all operands are added.
  bool Continue = true;
  unsigned AddResultIdx = Matrix.size();
  while (Continue) {
    float DataA_ArrivalTime = AddChain[0].second;
    float DataB_ArrivalTime = AddChain[1].second;

    float AddDelay;
    if (Matrix[0].size() == 16)
      AddDelay = ADD_16_DELAY;
    else if (Matrix[0].size() == 32)
      AddDelay = ADD_32_DELAY;
    else if (Matrix[0].size() == 64)
      AddDelay = ADD_64_DELAY;

    float AddResult_ArrivalTime = std::max(DataA_ArrivalTime, DataB_ArrivalTime) + AddDelay;

    std::vector<std::pair<unsigned, float> > TempAddChain;
    for (unsigned i = 2; i < AddChain.size(); ++i)
      TempAddChain.push_back(AddChain[i]);
    TempAddChain.push_back(std::make_pair(AddChain.size(), AddResult_ArrivalTime));

    AddChain = TempAddChain;
    std::sort(AddChain.begin(), AddChain.end(), OperandCompare);

    if (AddChain.size() == 1)
      Continue = false;
  }

  assert(AddChain.size() == 1 && "Should be only one element");
  return AddChain[0].second;
}

void SIRAddMulChain::generateAddChain(MatrixType Matrix,
                                      std::vector<std::pair<unsigned, float> > Ops,
                                      std::string ResultName, raw_fd_ostream &Output) {
  // Sort the operands of add chain in ascending order of delay.
  std::sort(Ops.begin(), Ops.end(), OperandCompare);

  // Add the first two operands, and insert the result into chain
  // until all operands are added.
  unsigned AddNum = 0;
  unsigned AddResultIdx = Matrix.size();
  std::map<unsigned, std::string> AddResultIdxName;
  bool Continue = true;
  while (Continue) {
    float DataA_ArrivalTime = Ops[0].second;
    float DataB_ArrivalTime = Ops[1].second;
    
    unsigned DataA_BitWidth;
    if (Ops[0].first >= Matrix.size())
      // The Idx means it is the add result.
      DataA_BitWidth = Matrix[0].size();
    else
      DataA_BitWidth = getOperandBitWidth(Matrix[Ops[0].first]);

    unsigned DataB_BitWidth;
    if (Ops[1].first >= Matrix.size())
      // The Idx means it is the add result.
      DataB_BitWidth = Matrix[1].size();
    else
      DataB_BitWidth = getOperandBitWidth(Matrix[Ops[1].first]);

    unsigned AddBitWidth = std::max(DataA_BitWidth, DataB_BitWidth);

    float AddResult_ArrivalTime
      = std::max(DataA_ArrivalTime, DataB_ArrivalTime) + ADD_16_DELAY;

    // Generate the implementation of the DataA & DataB of current adder.
    Output << "wire[" + utostr_32(DataA_BitWidth - 1) + ":0] " << ResultName
           << "_" + utostr_32(AddNum) + "_DataA = ";
    if (Ops[0].first >= Matrix.size()) {
      Output << AddResultIdxName[Ops[0].first] << ";\n";
    } else {
      Output << "{";
      for (unsigned i = 0; i < DataA_BitWidth; ++i) {
        Output << Matrix[Ops[0].first][DataA_BitWidth - 1 - i].first;

        if (i != DataA_BitWidth - 1)
          Output << ", ";
      }
      Output << "};\n";
    }
    
    Output << "wire[" + utostr_32(DataB_BitWidth - 1) + ":0] " << ResultName
           << "_" + utostr_32(AddNum) + "_DataB = ";
    if (Ops[1].first >= Matrix.size()) {
      Output << AddResultIdxName[Ops[1].first] << ";\n";
    } else {
      Output << "{";
        for (unsigned i = 0; i < DataB_BitWidth; ++i) {
          Output << Matrix[Ops[1].first][DataB_BitWidth - 1 - i].first;

          if (i != DataB_BitWidth - 1)
            Output << ", ";
        }
        Output << "};\n";
    }

    // Generate the implementation of the adder.
    if (Ops.size() != 2) {
      Output << "wire[" + utostr_32(Matrix[0].size() - 1) + ":0] "
             << ResultName + "_" + utostr_32(AddNum);
      Output << " = " << ResultName + "_" + utostr_32(AddNum) + "_DataA";
      Output << " + " << ResultName + "_" + utostr_32(AddNum) + "_DataB;\n\n";
    } else {
      Output << "wire[" + utostr_32(Matrix[0].size() - 1) + ":0] "
             << ResultName;
      Output << " = " << ResultName + "_" + utostr_32(AddNum) + "_DataA";
      Output << " + " << ResultName + "_" + utostr_32(AddNum) + "_DataB;\n\n";
    }    

    std::vector<std::pair<unsigned, float> > TempAddChain;
    for (unsigned i = 2; i < Ops.size(); ++i)
      TempAddChain.push_back(Ops[i]);
    TempAddChain.push_back(std::make_pair(AddResultIdx, AddResult_ArrivalTime));

    // Index the add result and its name.
    AddResultIdxName.insert(std::make_pair(AddResultIdx, ResultName + "_" + utostr_32(AddNum)));
    ++AddResultIdx;
    ++AddNum;

    Ops = TempAddChain;
    std::sort(Ops.begin(), Ops.end(), OperandCompare);

    if (Ops.size() == 1)
      Continue = false;
  }
}

float SIRAddMulChain::hybridTreeCodegen(MatrixType Matrix, std::string MatrixName,
                                        unsigned RowNum, unsigned ColNum,
                                        raw_fd_ostream &Output) {
  assert(isConstantInt(Matrix[0]) && "Should be a constant integer!");

  // Consider a row in Matrix is a operand, get its arrival time.
  // Ignore the constant integer row since it is more efficient to
  // compress it instead of add it.
  std::vector<std::pair<unsigned, float> > OpArrivalTime;
  for (unsigned i = 0; i < RowNum; ++i) {
    MatrixRowType Row = Matrix[i];

    if (isConstantInt(Row))
      continue;

    float ArrivalTime = 0.0f;
    for (unsigned j = 0; j < ColNum; ++j) {
      DotType Dot = Row[j];

      if (ArrivalTime != 0.0f)
        assert(Dot.second == ArrivalTime ||
               Dot.second == 0.0f && "Unexpected Dot!");

      ArrivalTime = std::max(ArrivalTime, Dot.second);
    }

    OpArrivalTime.push_back(std::make_pair(i, ArrivalTime));
  }

  // Represent the operand using its index and sort them in ascending
  // order of arrival time.
  std::sort(OpArrivalTime.begin(), OpArrivalTime.end(), OperandCompare);
  
  /// Build hybrid tree according to the arrival time of operands.
  // The biggest arrival time will be the limit of add chains that can be built.
  float LimitTime = OpArrivalTime.back().second;
  
  // Traverse the OperandArrivalTimeMap to build add chain as many as possible.
  std::vector<std::pair<std::vector<std::pair<unsigned, float> >, float> > AddChainList;
  std::set<unsigned> OpsInAddChain;
  bool Continue = true;
  while (Continue) {
    Continue = false;
  
    std::vector<std::pair<unsigned, float> > AddChain;
    for (unsigned i = 0; i < OpArrivalTime.size(); ++i) {
      // Ignore the operand that is already included in other add chains.
      if (OpsInAddChain.count(OpArrivalTime[i].first))
        continue;

      std::vector<std::pair<unsigned, float> > PotentialAddChain = AddChain;
      PotentialAddChain.push_back(OpArrivalTime[i]);
  
      float PotentialAddChainResultTime = predictAddChainResultTime(PotentialAddChain, Matrix);
      // If the result time is within the limit, then the potential add chain
      // is valid. Or we will try to insert more operands into this chain until
      // it become invalid.
      if (PotentialAddChainResultTime <= LimitTime) {
        AddChain = PotentialAddChain;
        continue;
      } else
        break;
    }
  
    // If we succeed to built a add chain, index it.
    if (AddChain.size() > 1) {
      float AddChainResultTime = predictAddChainResultTime(AddChain, Matrix);
      AddChainList.push_back(std::make_pair(AddChain, AddChainResultTime));

      for (unsigned i = 0; i < AddChain.size(); ++i)
        OpsInAddChain.insert(AddChain[i].first);

      Continue = true;
    }
  }

  /// Rebuild the matrix according to the partition.
  MatrixType NewMatrix;

  // Insert the add chain result to the matrix to be compressed later.
  for (unsigned i = 0; i < AddChainList.size(); ++i) {
    std::pair<std::vector<std::pair<unsigned, float> >, float> AddChain = AddChainList[i];
    std::vector<std::pair<unsigned, float> > AddChainOps = AddChain.first;
    float AddChainResultTime = AddChain.second;

    // Create a operand represent the add chain result.
    std::string AddChainResultName = "add_chain_result_" + utostr_32(i);

    // Generate the implementation of the add chains.
    generateAddChain(Matrix, AddChainOps, AddChainResultName, Output);

    MatrixRowType Row;
    for (unsigned j = 0; j < ColNum; ++j) {
      std::string DotName = AddChainResultName + "[" + utostr_32(j) + "]";
      Row.push_back(std::make_pair(DotName, AddChainResultTime));
    }

    NewMatrix.push_back(Row);
  }
  // All operands that are not included in add chain will be compressed later.
  for (unsigned i = 0; i < RowNum; ++i) {
    // Ignore the operands in add chain.
    if (OpsInAddChain.count(i))
      continue;

    NewMatrix.push_back(Matrix[i]);
  }

  // Compress the NewMatrix.
  MatrixType TMatrix = transportMatrix(NewMatrix, NewMatrix.size(), ColNum);
  float ResultArrivalTime = compressMatrix(TMatrix, MatrixName, NewMatrix.size(), ColNum, Output);

  return ResultArrivalTime;
}

void SIRAddMulChain::replaceWithCompressor() {
  SIRDatapathBuilder Builder(SM, *TD);

  typedef std::map<IntrinsicInst *, std::set<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    Value *CompressorVal = Builder.createCompressorInst(I->first);

    SM->indexKeepVal(CompressorVal);

    IntrinsicInst *Compressor = dyn_cast<IntrinsicInst>(CompressorVal);
    ChainRoot2Compressor.insert(std::make_pair(I->first, Compressor));
    Compressor2ChainRoot.insert(std::make_pair(Compressor, I->first));
  }
}

void SIRAddMulChain::printAllChain() {
  std::string ChainFile = LuaI::GetString("Chain");
  std::string Error;
  raw_fd_ostream ChainOutput(ChainFile.c_str(), Error);

  unsigned OptNum = 0;
  typedef std::map<IntrinsicInst *, std::set<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    IntrinsicInst *II = I->first;
    std::set<IntrinsicInst *> Chain = I->second;

    ChainOutput << "Root instruction is " << II->getName();
    ChainOutput << "\n";

    typedef std::set<IntrinsicInst *>::iterator chain_iterator;
    for (chain_iterator I = Chain.begin(), E = Chain.end(); I != E; ++I) {
      IntrinsicInst *ChainInst = *I;

      if (ChainInst->hasOneUse())
        ChainOutput.indent(2) << "(*****)";
      else {
        ChainOutput.indent(2) << "(";
        typedef Value::use_iterator use_iterator;
        for (use_iterator UI = ChainInst->use_begin(), UE = ChainInst->use_end(); UI != UE; ++UI) {
          Value *UserInst = *UI;

          if (!UserInst->use_empty())
            ChainOutput << UserInst->getName() << ", ";
        }
        ChainOutput << ")  ";
      }

      ChainInst->print(ChainOutput.indent(2));
      ChainOutput << "\n";
    }

    ChainOutput << "\n\n";

    OptNum += Chain.size();
  }
}