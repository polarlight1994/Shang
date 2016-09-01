#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <sstream>

using namespace llvm;
using namespace vast;

typedef std::pair<std::string, float> DotType;
typedef std::vector<DotType> MatrixRowType;
typedef std::vector<MatrixRowType> MatrixType;

static unsigned GPC_NUM = 0;

static float NET_DELAY = 0.500;

static float ADD_CHAIN_16_DELAY[9] = { 0.521, 0.59, 0.706, 1.584, 1.715, 1.822, 2.101, 2.183, 2.183 };
static float ADD_CHAIN_32_DELAY[23] = { 0.816, 0.876, 1.163, 2.199, 2.343,
                                        2.343, 2.047, 2.265, 2.504, 3.033,
                                        2.724, 3.189, 2.985, 3.664, 3.553,
                                        3.814, 3.905, 3.83, 4.072, 3.892,
                                        3.915, 3.84, 4.455 };
static float ADD_CHAIN_64_DELAY[9] = {1.24, 1.255, 1.574, 2.326, 2.784, 2.784, 2.9, 2.9, 3.033};

namespace {
struct SIRAddMulChain : public SIRPass {
  static char ID;
  DataLayout *TD;
  SIR *SM;

  unsigned ChainNum;

  std::set<IntrinsicInst *> Visited;
  std::set<IntrinsicInst *> Collected;
  std::map<IntrinsicInst *, std::vector<IntrinsicInst *> > ChainMap;
  std::map<IntrinsicInst *, IntrinsicInst *> ChainRoot2Compressor;
  std::map<IntrinsicInst *, IntrinsicInst *> Compressor2ChainRoot;

  std::map<Value *, float> ValArrivalTime;

  class SIRGPC {
  private:
    std::string GPCName;

    // Input dots & Output dots.
    std::vector<unsigned> InputDotNums;
    unsigned OutputDotNum;

    // Area cost in FPGA
    unsigned Area;
    // Delay cost in FPGA
    std::vector<std::vector<float> > DelayTable;

  public:
    // Default constructor
    SIRGPC(std::string Name, std::vector<unsigned> InputDotNums,
           unsigned OutputDotNum, unsigned Area, std::vector<float> Delay)
      : GPCName(Name), InputDotNums(InputDotNums), OutputDotNum(OutputDotNum),
        Area(Area) {
      unsigned Idx = 0;
      for (unsigned i = 0; i < InputDotNums.size(); ++i) {
        unsigned InputDotNum = InputDotNums[i];

        std::vector<float> DelayTableRow;
        for (unsigned j = 0; j < InputDotNum; ++j)
          DelayTableRow.push_back(Delay[Idx++]);

        DelayTable.push_back(DelayTableRow);
      }
    }

    std::string getName() { return GPCName; }
    std::vector<unsigned> getInputDotNums() { return InputDotNums; }
    unsigned getOutputDotNum() { return OutputDotNum; }
    // To be fixed.
    float getCriticalDelay() { return 0.043;  }
    unsigned getArea() { return Area; }
    float calcPerformance(std::vector<float> InputDelay);
  };

  std::vector<SIRGPC> GPCs;

  SIRAddMulChain() : SIRPass(ID), ChainNum(0), DebugOutput("DebugMatrix.txt", Error) {
    initializeSIRAddMulChainPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);
  void collectAddMulChain();
  void collect(IntrinsicInst *ChainRoot);

  std::vector<Value *> eliminateIdenticalOperands(std::vector<Value *> Operands,
                                                  Value *ChainRoot, unsigned BitWidth);
  std::vector<Value *> OptimizeOperands(std::vector<Value *> Operands,
                                        Value *ChainRoot, unsigned BitWidth);

  MatrixType createMatrixForOperands(std::vector<Value *> Operands,
                                     unsigned RowNum, unsigned ColNum);
  MatrixType sumAllSignBitsInMatrix(MatrixType Matrix, unsigned RowNum, unsigned ColumnNum);

  float getAddChainDelay(unsigned OpBitWidth, unsigned OpNum);
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
  void generateGPCInstance(unsigned GPCIdx, std::vector<std::vector<DotType> > InputDots,
                           std::string OutputName, float OutputArrivalTime,
                           raw_fd_ostream &Output);

  // Initial the GPCs.
  void initGPCs();

  bool needToCompress(std::vector<unsigned> BitNumList, unsigned RowNo);
  MatrixType compressTMatrixUsingGPC(MatrixType TMatrix, unsigned GPCIdx, unsigned RowNo,
                                     unsigned Stage, raw_fd_ostream &Output);
  unsigned getHighestPriorityGPC(MatrixType TMatrix, unsigned RowNo);
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

static bool LessThan(std::pair<unsigned, float> OpA, std::pair<unsigned, float> OpB) {
  return OpA.second < OpB.second;
}

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
  printAllChain();

  replaceWithCompressor();
  generateDotMatrix();

  return false;
}

void SIRAddMulChain::collectAddMulChain() {
  std::vector<IntrinsicInst *> AddChainRootVector;

  Function *F = SM->getFunction();

  ReversePostOrderTraversal<BasicBlock *> RPO(&F->getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator BI = RPO.begin(), BE = RPO.end(); BI != BE; ++BI) {
    BasicBlock *BB = *BI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      if (IntrinsicInst *InstII = dyn_cast<IntrinsicInst>(Inst)) {
        if (InstII->getIntrinsicID() == Intrinsic::shang_add ||
            InstII->getIntrinsicID() == Intrinsic::shang_addc) {
          unsigned UserNum = 0;
          unsigned UsedByChainNum = 0;
          typedef Value::use_iterator use_iterator;
          for (use_iterator UI = InstII->use_begin(), UE = InstII->use_end(); UI != UE; ++UI) {
            Value *UserVal = *UI;

            if (IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(UserVal)) {
              ++UserNum;

              if (UserInst->getIntrinsicID() == Intrinsic::shang_add || UserInst->getIntrinsicID() == Intrinsic::shang_addc)
                ++UsedByChainNum;
            }
          }

          if (UsedByChainNum == 0 || UserNum >= 2)
            AddChainRootVector.push_back(InstII);
        }
      }
    }
  }

  for (unsigned i = 0; i < AddChainRootVector.size(); ++i) {
    IntrinsicInst *AddChainRoot = AddChainRootVector[i];
    collect(AddChainRoot);
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
  std::vector<IntrinsicInst *> Chain;
  while(!VisitStack.empty()) {
    IntrinsicInst *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    assert(CurNode->getIntrinsicID() == Intrinsic::shang_add ||
           CurNode->getIntrinsicID() == Intrinsic::shang_addc && "Unexpected type!");

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      if (Depth != 0) {
         Chain.push_back(CurNode);
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

  std::set<Value *> Visited;
  std::vector<std::pair<Value *, unsigned> > OpNums;
  for (unsigned i = 0; i < Operands.size(); ++i) {
    Value *Op = Operands[i];

    // First index the constant integer.
    if (isa<ConstantInt>(Op)) {
      SM->indexKeepVal(Op);
      FinalOperands.push_back(Op);
      continue;
    }

    // Then index the repeated operand and its numbers.
    if (!Visited.count(Op))
      OpNums.push_back(std::make_pair(Op, 1));
    else {
      for (unsigned j = 0; j < OpNums.size(); ++j) {
        Value *SameOp = OpNums[j].first;

        if (Op == SameOp)
          OpNums[j].second++;
      }
    }
  }

  // Initial a datapath builder to handle the repeated operand.
  SIRDatapathBuilder Builder(SM, *TD);

  typedef std::vector<std::pair<Value *, unsigned> >::iterator iterator;
  for (iterator I = OpNums.begin(), E = OpNums.end(); I != E; ++I) {
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

  case Intrinsic::shang_not:
    return 0.0f;

  case Intrinsic::shang_and:
  case Intrinsic::shang_or:
  case Intrinsic::shang_xor: {
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

    // To be noted that, in LLVM IR the return value
    // is counted in Operands, so the real numbers
    // of operands should be minus one.
    unsigned IONums = II->getNumOperands() - 1;
    assert(IONums == 2 && "Unexpected Num!");

    unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
    return LogicLevels * VFUs::LUTDelay;
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

  float delay = 0.0f;
  if (IntrinsicInst *RootII = dyn_cast<IntrinsicInst>(Root)) {
    if (RootII->getIntrinsicID() == Intrinsic::shang_compressor) {
      delay = getOperandArrivalTime(Root);

      return delay;
    }
  }

  // The delay of the root node.
  delay = getLatency(Root);

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

  initGPCs();

  typedef std::map<IntrinsicInst *, std::vector<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    generateDotmatrixForChain(I->first, Output);
  }

   // Generate the 3-2 compressor.
   Output << "module GPC_3_2_LUT(\n";
   Output << "\tinput wire[2:0] col0,\n";
   Output << "\toutput wire[1:0] sum\n";
   Output << ");\n\n";
   Output << "\tassign sum = col0[0] + col0[1] + col0[2];\n\n";
   Output << "endmodule\n\n";
}

void SIRAddMulChain::generateDotmatrixForChain(IntrinsicInst *ChainRoot, raw_fd_ostream &Output) {
  assert(ChainMap.count(ChainRoot) && "Not a chain rooted on ChainRoot!");
  std::vector<IntrinsicInst *> &Chain = ChainMap[ChainRoot];

  // Collect all chain instructions and extract all their operands.
  std::set<IntrinsicInst *> ChainInstSet;
  typedef std::vector<IntrinsicInst *>::iterator iterator;
  for (iterator I = Chain.begin(), E = Chain.end(); I != E; ++I)
    ChainInstSet.insert(*I);

  std::vector<Value *> Operands;
  for (iterator I = Chain.begin(), E = Chain.end(); I != E; ++I) {
    IntrinsicInst *ChainInst = *I;

    for (unsigned i = 0; i < ChainInst->getNumOperands() - 1; ++i) {
      Value *Operand = ChainInst->getOperand(i);

      // Ignore the chain instruction itself.
      if (IntrinsicInst *OperandInst = dyn_cast<IntrinsicInst>(Operand)) {
        if (ChainInstSet.count(OperandInst))
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
  
  float ResultArrivalTime = hybridTreeCodegen(Matrix, MatrixName,
                                              MatrixRowNum, MatrixColNum, Output);

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
    Output << "compressor_3_2 compressor_3_2_" << utostr_32(GPC_NUM) << "( ";

    // Link the wire according to the path delay of compressor, since the delay order is listed as
    // col[1] < col[0] < col[2].
    Output << ".a(" << CompressCouple[0].first << "), .b(" << CompressCouple[1].first << "), .cin("
           << CompressCouple[2].first << "), .result(" << SumName << "), .cout(" << CoutName << ") );\n\n";

    // Calculate the output delay.
    float SumDelay_0 = CompressCouple[1].second;
    float SumDelay_1 = CompressCouple[2].second;
    float SumDelay_2 = CompressCouple[0].second;

    float CoutDelay_0 = CompressCouple[1].second;
    float CoutDelay_1 = CompressCouple[2].second;
    float CoutDelay_2 = CompressCouple[0].second;

    SumDelay = std::max(SumDelay_0, std::max(SumDelay_1, SumDelay_2));
    CoutDelay = std::max(CoutDelay_0, std::max(CoutDelay_1, CoutDelay_2));
  } else {
    // Print the declaration of the result.
    Output << "wire " << SumName << ";\n";
    Output << "wire " << CoutName << ";\n";

    // Print the instantiation of the compressor module.
    Output << "compressor_2_2 compressor_2_2_" << utostr_32(GPC_NUM) << "( ";

    // Link the wire according to the path delay of compressor, since the delay order is listed as
    // col[1] < col[0].
    Output << ".a(" << CompressCouple[0].first << "), .b(" << CompressCouple[1].first 
           << "), .result(" << SumName << "), .cout(" << CoutName << ") );\n\n";

    // Calculate the output delay.
    float SumDelay_0 = CompressCouple[1].second;
    float SumDelay_1 = CompressCouple[0].second;

    float CoutDelay_0 = CompressCouple[1].second;
    float CoutDelay_1 = CompressCouple[0].second;

    SumDelay = std::max(SumDelay_0, SumDelay_1);
    CoutDelay = std::max(CoutDelay_0, CoutDelay_1);
  }  

  ++GPC_NUM;

  return std::make_pair(SumDelay, CoutDelay);
}

void SIRAddMulChain::generateGPCInstance(unsigned GPCIdx,
                                         std::vector<std::vector<DotType> > InputDots,
                                         std::string OutputName,
                                         float OutputArrivalTime,
                                         raw_fd_ostream &Output) {
  // Get the GPC to be used and its information.
  SIRGPC GPC = GPCs[GPCIdx];
  std::vector<unsigned> InputDotNums = GPC.getInputDotNums();
  unsigned OutputDotNum = GPC.getOutputDotNum();
  std::string GPCName = GPC.getName();

  // Print the declaration of the result.  
  Output << "wire [" << utostr_32(OutputDotNum - 1) << ":0] " << OutputName << ";\n";

  // Print the instantiation of the compressor module.
  Output << GPCName << " " + GPCName + "_" << utostr_32(GPC_NUM) << "(";

  // Print the inputs and outputs instance.
  for (unsigned i = 0; i < InputDotNums.size(); ++i) {
    // Ignore the empty column.
    if (InputDotNums[i] == 0)
      continue;

    Output << ".col" << utostr_32(i) << "({";

    std::vector<DotType> InputDotRow = InputDots[i];
    assert(InputDotRow.size() == InputDotNums[i] && "Unexpected input dot number!");
    for (unsigned j = 0; j < InputDotRow.size(); ++j) {
      Output << InputDotRow[j].first;

      if (j != InputDotRow.size() - 1)
        Output << ", ";
    }

    Output << "}), ";
  }

  Output << ".sum(" << OutputName << ")";

  Output << ");\n";
}

void SIRAddMulChain::initGPCs() {
  /// GPC_3_2_LUT
  // Inputs & Outputs
  unsigned GPC_3_2_LUT_Inputs[1] = { 3 };
  std::vector<unsigned> GPC_3_2_LUT_InputsVector(GPC_3_2_LUT_Inputs,
                                                 GPC_3_2_LUT_Inputs + 1);
  // Delay table
  float GPC_3_2_LUT_DelayTable[6] = { 0.043f, 0.043f, 0.043f,
                                      0.052f, 0.051f, 0.049f };
  std::vector<float> GPC_3_2_LUT_DelayTable_Vector(GPC_3_2_LUT_DelayTable,
                                                   GPC_3_2_LUT_DelayTable + 6);
  SIRGPC GPC_3_2_LUT("GPC_3_2_LUT", GPC_3_2_LUT_InputsVector,
                     2, 1, GPC_3_2_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_3_2_LUT);

  /// GPC_4_3_LUT
  // Inputs & Outputs
  unsigned GPC_4_3_LUT_Inputs[1] = { 4 };
  std::vector<unsigned> GPC_4_3_LUT_InputsVector(GPC_4_3_LUT_Inputs,
                                                 GPC_4_3_LUT_Inputs + 1);
  // Delay table
  float GPC_4_3_LUT_DelayTable[12] = { 0.043f, 0.043f, 0.043f, 0.043f,
                                       0.043f, 0.043f, 0.043f, 0.043f,
                                       0.051f, 0.049f, 0.052f, 0.051f };
  std::vector<float> GPC_4_3_LUT_DelayTable_Vector(GPC_4_3_LUT_DelayTable,
                                                   GPC_4_3_LUT_DelayTable + 12);
  SIRGPC GPC_4_3_LUT("GPC_4_3_LUT", GPC_4_3_LUT_InputsVector,
                     3, 2, GPC_4_3_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_4_3_LUT);

  /// GPC_5_3_LUT
  // Inputs & Outputs
  unsigned GPC_5_3_LUT_Inputs[1] = { 5 };
  std::vector<unsigned> GPC_5_3_LUT_InputsVector(GPC_5_3_LUT_Inputs,
                                                 GPC_5_3_LUT_Inputs + 1);
  // Delay table
  float GPC_5_3_LUT_DelayTable[15] = { 0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                       0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                       0.051f, 0.049f, 0.052f, 0.051f, 0.049f };
  std::vector<float> GPC_5_3_LUT_DelayTable_Vector(GPC_5_3_LUT_DelayTable,
                                                   GPC_5_3_LUT_DelayTable + 15);
  SIRGPC GPC_5_3_LUT("GPC_5_3_LUT", GPC_5_3_LUT_InputsVector,
                     3, 2, GPC_5_3_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_5_3_LUT);

  /// GPC_6_3_LUT
  // Inputs & Outputs
  unsigned GPC_6_3_LUT_Inputs[1] = { 6 };
  std::vector<unsigned> GPC_6_3_LUT_InputsVector(GPC_6_3_LUT_Inputs,
                                                 GPC_6_3_LUT_Inputs + 1);
  // Delay table
  float GPC_6_3_LUT_DelayTable[18] = { 0.043f, 0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                       0.043f, 0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                       0.043f, 0.043f, 0.043f, 0.043f, 0.043f, 0.043f };
  std::vector<float> GPC_6_3_LUT_DelayTable_Vector(GPC_6_3_LUT_DelayTable,
                                                   GPC_6_3_LUT_DelayTable + 18);
  SIRGPC GPC_6_3_LUT("GPC_6_3_LUT", GPC_6_3_LUT_InputsVector,
                     3, 2, GPC_6_3_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_6_3_LUT);

  /// GPC_13_3_LUT
  // Inputs & Outputs
  unsigned GPC_13_3_LUT_Inputs[2] = { 3, 1 };
  std::vector<unsigned> GPC_13_3_LUT_InputsVector(GPC_13_3_LUT_Inputs,
                                                  GPC_13_3_LUT_Inputs + 2);
  // Delay table
  float GPC_13_3_LUT_DelayTable[12] = { 0.043f, 0.043f, 0.043f, 0.043f,
                                        0.043f, 0.043f, 0.043f, 0.043f,
                                        0.049f, 0.051f, 0.052f, 0.051f };
  std::vector<float> GPC_13_3_LUT_DelayTable_Vector(GPC_13_3_LUT_DelayTable,
                                                    GPC_13_3_LUT_DelayTable + 12);
  SIRGPC GPC_13_3_LUT("GPC_13_3_LUT", GPC_13_3_LUT_InputsVector,
                      3, 2, GPC_13_3_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_13_3_LUT);

  /// GPC_23_3_LUT
  // Inputs & Outputs
  unsigned GPC_23_3_LUT_Inputs[2] = { 3, 2 };
  std::vector<unsigned> GPC_23_3_LUT_InputsVector(GPC_23_3_LUT_Inputs,
                                                  GPC_23_3_LUT_Inputs + 2);
  // Delay table
  float GPC_23_3_LUT_DelayTable[15] = { 0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                        0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                        0.049f, 0.049f, 0.051f, 0.051f, 0.052f };
  std::vector<float> GPC_23_3_LUT_DelayTable_Vector(GPC_23_3_LUT_DelayTable,
                                                    GPC_23_3_LUT_DelayTable + 15);
  SIRGPC GPC_23_3_LUT("GPC_23_3_LUT", GPC_23_3_LUT_InputsVector,
                      3, 2, GPC_23_3_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_23_3_LUT);

  /// GPC_14_3_LUT
  // Inputs & Outputs
  unsigned GPC_14_3_LUT_Inputs[2] = { 4, 1 };
  std::vector<unsigned> GPC_14_3_LUT_InputsVector(GPC_14_3_LUT_Inputs,
                                                  GPC_14_3_LUT_Inputs + 2);
  // Delay table
  float GPC_14_3_LUT_DelayTable[15] = { 0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                        0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                        0.049f, 0.049f, 0.051f, 0.051f, 0.052f };
  std::vector<float> GPC_14_3_LUT_DelayTable_Vector(GPC_14_3_LUT_DelayTable,
                                                    GPC_14_3_LUT_DelayTable + 15);
  SIRGPC GPC_14_3_LUT("GPC_14_3_LUT", GPC_14_3_LUT_InputsVector,
                      3, 2, GPC_14_3_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_14_3_LUT);

  /// GPC_15_3_LUT
  // Inputs & Outputs
  unsigned GPC_15_3_LUT_Inputs[2] = { 5, 1 };
  std::vector<unsigned> GPC_15_3_LUT_InputsVector(GPC_15_3_LUT_Inputs,
                                                  GPC_15_3_LUT_Inputs + 2);
  // Delay table
  float GPC_15_3_LUT_DelayTable[18] = { 0.043f, 0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                        0.043f, 0.043f, 0.043f, 0.043f, 0.043f, 0.043f,
                                        0.043f, 0.043f, 0.043f, 0.043f, 0.043f, 0.043f };
  std::vector<float> GPC_15_3_LUT_DelayTable_Vector(GPC_15_3_LUT_DelayTable,
                                                    GPC_15_3_LUT_DelayTable + 18);
  SIRGPC GPC_15_3_LUT("GPC_15_3_LUT", GPC_15_3_LUT_InputsVector,
                      3, 2, GPC_15_3_LUT_DelayTable_Vector);
  GPCs.push_back(GPC_15_3_LUT);

  /// GPC_506_5
  // Inputs & Outputs
  unsigned GPC_506_5_Inputs[3] = { 6, 0, 5 };
  std::vector<unsigned> GPC_506_5_InputsVector(GPC_506_5_Inputs,
                                               GPC_506_5_Inputs + 3);
  // Delay table
  float GPC_506_5_DelayTable[55] = { 0.167f, 0.167f, 0.167f, 0.167f, 0.167f, 0.167f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                     0.261f, 0.261f, 0.261f, 0.261f, 0.261f, 0.261f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                     0.325f, 0.325f, 0.325f, 0.325f, 0.325f, 0.309f, 0.16f, 0.16f, 0.16f, 0.16f, 0.16f,
                                     0.355f, 0.339f, 0.355f, 0.355f, 0.355f, 0.339f, 0.238f, 0.238f, 0.238f, 0.221f, 0.221f,
                                     0.31f, 0.302f, 0.31f, 0.31f, 0.31f, 0.302f, 0.238f, 0.238f, 0.238f, 0.238f, 0.238f };
  std::vector<float> GPC_506_5_DelayTable_Vector(GPC_506_5_DelayTable,
                                                 GPC_506_5_DelayTable + 55);
  SIRGPC GPC_506_5("GPC_506_5", GPC_506_5_InputsVector,
                   5, 4, GPC_506_5_DelayTable_Vector);
  GPCs.push_back(GPC_506_5);

  // GPC_606_5
  // Inputs & Outputs
  unsigned GPC_606_5_Inputs[3] = { 6, 0, 6 };
  std::vector<unsigned> GPC_606_5_InputsVector(GPC_606_5_Inputs,
                                               GPC_606_5_Inputs + 3);
  // Delay table
  float GPC_606_5_DelayTable[60] = { 0.167f, 0.167f, 0.167f, 0.167f, 0.167f, 0.167f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                     0.261f, 0.261f, 0.261f, 0.261f, 0.261f, 0.261f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                     0.309f, 0.325f, 0.325f, 0.325f, 0.325f, 0.309f, 0.16f, 0.16f, 0.16f, 0.16f, 0.16f, 0.16f,
                                     0.339f, 0.355f, 0.355f, 0.355f, 0.355f, 0.339f, 0.221f, 0.221f, 0.221f, 0.221f, 0.221f, 0.216f,
                                     0.302f, 0.31f, 0.31f, 0.31f, 0.31f, 0.302f, 0.238f, 0.236f, 0.236f, 0.236f, 0.238f, 0.238f };
  std::vector<float> GPC_606_5_DelayTable_Vector(GPC_606_5_DelayTable,
                                                 GPC_606_5_DelayTable + 60);
  SIRGPC GPC_606_5("GPC_606_5", GPC_606_5_InputsVector,
                   5, 4, GPC_606_5_DelayTable_Vector);
  GPCs.push_back(GPC_606_5);

  // GPC_1325_5
  // Inputs & Outputs
  unsigned GPC_1325_5_Inputs[4] = { 5, 2, 3, 1 };
  std::vector<unsigned> GPC_1325_5_InputsVector(GPC_1325_5_Inputs,
                                                GPC_1325_5_Inputs + 4);
  // Delay table
  float GPC_1325_5_DelayTable[55] = { 0.167f, 0.167f, 0.167f, 0.167f, 0.251f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                      0.261f, 0.261f, 0.261f, 0.261f, 0.311f, 0.152f, 0.152f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                      0.325f, 0.325f, 0.325f, 0.3f, 0.293f, 0.325f, 0.325f, 0.16f, 0.16f, 0.16f, 9999.9999f,
                                      0.355f, 0.355f, 0.355f, 0.339f, 0.326f, 0.355f, 0.355f, 0.221f, 0.221f, 0.221f, 0.159f,
                                      0.31f, 0.31f, 0.31f, 0.302f, 0.287f, 0.31f, 0.31f, 0.238f, 0.238f, 0.238f, 0.238f };
  std::vector<float> GPC_1325_5_DelayTable_Vector(GPC_1325_5_DelayTable,
                                                  GPC_1325_5_DelayTable + 55);
  SIRGPC GPC_1325_5("GPC_1325_5", GPC_1325_5_InputsVector,
                    5, 4, GPC_1325_5_DelayTable_Vector);
  GPCs.push_back(GPC_1325_5);

  // GPC_1406_5
  // Inputs & Outputs
  unsigned GPC_1406_5_Inputs[4] = { 6, 0, 4, 1 };
  std::vector<unsigned> GPC_1406_5_InputsVector(GPC_1406_5_Inputs,
                                                GPC_1406_5_Inputs + 4);
  // Delay table
  float GPC_1406_5_DelayTable[55] = { 0.167f, 0.167f, 0.167f, 0.167f, 0.167f, 0.167f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                      0.152f, 0.261f, 0.261f, 0.261f, 0.261f, 0.261f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                      0.325f, 0.309f, 0.325f, 0.325f, 0.325f, 0.309f, 0.16f, 0.16f, 0.16f, 0.16f, 9999.9999f,
                                      0.355f, 0.339f, 0.355f, 0.355f, 0.355f, 0.339f, 0.159f, 0.221f, 0.221f, 0.221f, 0.159f,
                                      0.31f, 0.302f, 0.31f, 0.31f, 0.31f, 0.302f, 0.236f, 0.238f, 0.238f, 0.238f, 0.236f };
  std::vector<float> GPC_1406_5_DelayTable_Vector(GPC_1406_5_DelayTable,
                                                  GPC_1406_5_DelayTable + 55);
  SIRGPC GPC_1406_5("GPC_1406_5", GPC_1406_5_InputsVector,
                    5, 4, GPC_1406_5_DelayTable_Vector);
  GPCs.push_back(GPC_1406_5);

  // GPC_1415_5
  // Inputs & Outputs
  unsigned GPC_1415_5_Inputs[4] = { 5, 1, 4, 1 };
  std::vector<unsigned> GPC_1415_5_InputsVector(GPC_1415_5_Inputs,
                                                GPC_1415_5_Inputs + 4);
  // Delay table
  float GPC_1415_5_DelayTable[55] = { 0.167f, 0.167f, 0.167f, 0.167f, 0.251f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                      0.261f, 0.152f, 0.261f, 0.261f, 0.311f, 0.152f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f, 9999.9999f,
                                      0.309f, 0.325f, 0.325f, 0.309f, 0.293f, 0.262f, 0.16f, 0.16f, 0.16f, 0.16f, 9999.9999f,
                                      0.339f, 0.355f, 0.355f, 0.339f, 0.326f, 0.291f, 0.221f, 0.221f, 0.221f, 0.221f, 0.159f,
                                      0.302f, 0.31f, 0.31f, 0.302f, 0.287f, 0.25f, 0.238f, 0.238f, 0.238f, 0.238f, 0.238f };
  std::vector<float> GPC_1415_5_DelayTable_Vector(GPC_1415_5_DelayTable,
                                                  GPC_1415_5_DelayTable + 55);
  SIRGPC GPC_1415_5("GPC_1415_5", GPC_1415_5_InputsVector,
                    5, 4, GPC_1415_5_DelayTable_Vector);
  GPCs.push_back(GPC_1415_5);
}

bool SIRAddMulChain::needToCompress(std::vector<unsigned> BitNumList, unsigned RowNo) {
  return true;
}

MatrixType SIRAddMulChain::compressTMatrixUsingGPC(MatrixType TMatrix, unsigned GPCIdx,
                                                   unsigned RowNo, unsigned Stage,
                                                   raw_fd_ostream &Output) {
  // Get the GPC to be used.
  SIRGPC GPC = GPCs[GPCIdx];

  // Collect input dots.
  float InputArrivalTime = 0.0f;
  std::vector<std::vector<DotType> > InputDots;
  std::vector<unsigned> InputDotNums = GPC.getInputDotNums();
  for (unsigned i = 0; i < InputDotNums.size(); ++i) {
    unsigned InputDotNum = InputDotNums[i];

    // The dots to be compressed in current row in TMatrix.
    std::vector<DotType> InputDotRow;
    for (unsigned j = 0; j < InputDotNum; ++j) {
      DotType Dot = TMatrix[RowNo + i][j];
      InputDotRow.push_back(Dot);

      InputArrivalTime = std::max(InputArrivalTime, Dot.second);
    }
    
    InputDots.push_back(InputDotRow);
  }

  // Clear input dots in TMatrix.
  for (unsigned i = 0; i < InputDotNums.size(); ++i) {
    unsigned InputDotNum = InputDotNums[i];

    // The dots to be compressed in current row in TMatrix.
    std::vector<DotType> InputDotRow;
    for (unsigned j = 0; j < InputDotNum; ++j)
      TMatrix[RowNo + i][j] = std::make_pair("1'b0", 0.0f);
  }

  // Get name and delay for output dots.
  std::string OutputName = "gpc_result_" + utostr_32(GPC_NUM++) + "_" + utostr_32(Stage);
  float OutputArrivalTime = InputArrivalTime + GPC.getCriticalDelay();

  // Insert the output dots into TMatrix.
  unsigned OutputDotNum = GPC.getOutputDotNum();
  for (unsigned i = 0; i < OutputDotNum; ++i) {
    // Do not insert if exceed the range of TMatrix.
    if (i >= TMatrix.size())
      break;

    std::string OutputDotName = OutputName + "[" + utostr_32(i) + "]";
    TMatrix[RowNo + i].push_back(std::make_pair(OutputDotName, OutputArrivalTime));
  }

  // Generate GPC instance.
  generateGPCInstance(GPCIdx, InputDots, OutputName, OutputArrivalTime, Output);

  printTMatrixForDebug(TMatrix);

  return TMatrix;
}

unsigned SIRAddMulChain::getHighestPriorityGPC(MatrixType TMatrix, unsigned RowNo) {
  unsigned HighestPriorityGPCIdx;

  // Try all GPCs and evaluate its performance.
  std::vector<std::pair<unsigned, float> > PriorityList;
  for (unsigned i = 0; i < GPCs.size(); ++i) {
    SIRGPC GPC = GPCs[i];

    // Get the information of current GPC.
    std::vector<unsigned> InputDotNums = GPC.getInputDotNums();
    unsigned OutputDotNum = GPC.getOutputDotNum();
    float CriticalDelay = GPC.getCriticalDelay();
    unsigned Area = GPC.getArea();

    // Get the real input dots number.
    unsigned RealInputDotNum = 0;
    for (unsigned j = 0; j < InputDotNums.size(); ++j)
      RealInputDotNum += std::min(InputDotNums[j], TMatrix[RowNo + j].size());

    // Get the earliest and latest input arrival time.
    float EarliestInputArrivalTime = 9999.9999f;
    float LatestInputArrivalTime = 0.0f;
    for (unsigned j = 0; j < InputDotNums.size(); ++j) {
      unsigned InputDotNum = std::min(InputDotNums[j], TMatrix[RowNo + j].size());

      for (unsigned k = 0; k < InputDotNum; ++k) {
        // The earliest input arrival time is only considered in first row.
        if (j == 0)
          EarliestInputArrivalTime = std::min(EarliestInputArrivalTime,
                                              TMatrix[RowNo + j][k].second);
        LatestInputArrivalTime = std::max(LatestInputArrivalTime,
                                          TMatrix[RowNo + j][k].second);
      }
    }

    // Evaluate the performance.
    unsigned CompressedDotNum = RealInputDotNum - OutputDotNum;
    float RealDelay = CriticalDelay + LatestInputArrivalTime - EarliestInputArrivalTime;
    float Performance = CompressedDotNum / (RealDelay * Area);

    PriorityList.push_back(std::make_pair(i, Performance));
  }

  // Sort the PriorityList and get the highest one.
  std::sort(PriorityList.begin(), PriorityList.end(), LessThan);

  // Debug
  errs() << "GPC performance list is as follows:\n";
  for (unsigned i = 0; i < PriorityList.size(); ++i) {
    unsigned GPCIdx = PriorityList[i].first;

    SIRGPC GPC = GPCs[GPCIdx];

    errs() << GPC.getName() << "--" << PriorityList[i].second << "\n";
  }

  return PriorityList.begin()->first;
}

MatrixType SIRAddMulChain::compressTMatrixInStage(MatrixType TMatrix,
                                                  unsigned Stage,
                                                  raw_fd_ostream &Output) {
  // Get the informations of the TMatrix.
  std::vector<unsigned> BitNumList = getBitNumListInTMatrix(TMatrix);

  // Compress the final row using XOR gate.
  if (BitNumList[TMatrix.size() - 1] >= 2) {
    std::vector<DotType> CompressCouple;

    // Collect the compressed bits into CompressCouple
    // and clear the compressed bits in TMatrix.
    for (unsigned i = 0; i < BitNumList[TMatrix.size() - 1]; ++i) {
      CompressCouple.push_back(TMatrix.back()[i]);
      TMatrix.back()[i] = std::make_pair("1'b0", 0.0f);
    }

    // Get the information of the result.
    std::string ResultName = "result_" + utostr_32(TMatrix.size() - 1) +
                             "_" + utostr_32(Stage);

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

  // Compress row by row. To be noted that, the last row is ignored since
  // it can be compressed using XOR gate.
  for (unsigned i = 0; i < TMatrix.size() - 1; ++i) {
    // Compress current row if it has more than target final bit numbers.
    if (BitNumList[i] > 3) {
      unsigned GPCIdx = getHighestPriorityGPC(TMatrix, i);
      TMatrix = compressTMatrixUsingGPC(TMatrix, GPCIdx, i, Stage, Output);
    }

    // Do some clean up and optimize work.
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
      if (BitNumList[i] > 3)
        Continue = true;
    }

    // Increase the stage and start next compress progress.
    if (Continue)
      ++Stage;
  }

  /// Finish the compress by sum the left-behind bits using ternary CPA.
  MatrixRowType CPADataA, CPADataB, CPADataC;
  float CPADataA_ArrivalTime = 0.0f;
  float CPADataB_ArrivalTime = 0.0f;
  float CPADataC_ArrivalTime = 0.0f;
  
  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    CPADataA.push_back(TMatrix[i][0]);
    CPADataA_ArrivalTime = std::max(CPADataA_ArrivalTime, TMatrix[i][0].second);

    if (TMatrix[i].size() == 1) {
      CPADataB.push_back(std::make_pair("1'b0", 0.0f));
      CPADataC.push_back(std::make_pair("1'b0", 0.0f));
    }
    else if (TMatrix[i].size() == 2) {
      CPADataB.push_back(TMatrix[i][1]);
      CPADataB_ArrivalTime = std::max(CPADataB_ArrivalTime, TMatrix[i][1].second);

      CPADataC.push_back(std::make_pair("1'b0", 0.0f));
    }
    else if (TMatrix[i].size() == 3) {
      CPADataB.push_back(TMatrix[i][1]);
      CPADataB_ArrivalTime = std::max(CPADataB_ArrivalTime, TMatrix[i][1].second);

      CPADataC.push_back(TMatrix[i][2]);
      CPADataC_ArrivalTime = std::max(CPADataC_ArrivalTime, TMatrix[i][2].second);
    }
  }
  assert(CPADataA.size() == CPADataB.size() &&
         CPADataA.size() == CPADataC.size() && "Should be same size!");

  Output << "\n";

  // Print the declaration and definition of DataA & DataB & DataC of CPA.
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

  Output << "wire[" << utostr_32(CPADataC.size() - 1) << ":0] CPA_DataC = {";
  for (unsigned i = 0; i < CPADataC.size(); ++i) {
    Output << CPADataC[CPADataC.size() - 1 - i].first;

    if (i != CPADataC.size() - 1)
      Output << ", ";
  }
  Output << "};\n";

  // Print the implementation of the CPA.
  Output << "wire[" << utostr_32(CPADataA.size() - 1)
         << ":0] CPA_Result = CPA_DataA + CPA_DataB + CPA_DataC;\n";

  // Print the implementation of the result.
  Output << "assign result = CPA_Result;\n";

  // Print the end of module.
  Output << "\nendmodule\n\n";

  // Index the arrival time of the compressor result.
  float CPADelay;
  if (CPADataA.size() == 16)
    CPADelay = ADD_CHAIN_16_DELAY[0];
  else if (CPADataA.size() == 32)
    CPADelay = ADD_CHAIN_32_DELAY[0];
  else if (CPADataA.size() == 64)
    CPADelay = ADD_CHAIN_64_DELAY[0];

  float ResultArrivalTime = std::max(std::max(CPADataA_ArrivalTime, CPADataB_ArrivalTime),
                                     CPADataC_ArrivalTime) + CPADelay;

  return ResultArrivalTime;
}

void SIRAddMulChain::printTMatrixForDebug(MatrixType TMatrix) {
  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType Row = TMatrix[i];

    for (unsigned j = 0; j < Row.size(); ++j) {
      DotType Dot = Row[j];

      DebugOutput << Dot.first/* << "--" << Dot.second*/;

      if (j != Row.size() - 1)
        DebugOutput << "  ";
    }

    DebugOutput << "\n";
  }

  DebugOutput << "\n\n";
}

float SIRAddMulChain::getAddChainDelay(unsigned OpBitWidth, unsigned OpNum) {
  float Period = VFUs::Period;

  if (OpBitWidth == 16 || OpBitWidth == 17)
    return (ADD_CHAIN_16_DELAY[OpNum - 2] + NET_DELAY) / Period;
  else if (OpBitWidth == 32)
    return (ADD_CHAIN_32_DELAY[OpNum - 2] + NET_DELAY) / Period;
  else if (OpBitWidth == 64)
    return (ADD_CHAIN_64_DELAY[OpNum - 2] + NET_DELAY) / Period;

  llvm_unreachable("Unexpected BitWidth!");
}

float SIRAddMulChain::predictAddChainResultTime(std::vector<std::pair<unsigned, float> > AddChain,
                                                MatrixType Matrix) {
  // Handle the trivial case.
  if (AddChain.size() == 1)
    return AddChain[0].second;

  // Sort the add chain operands in ascending order of delay.
  std::sort(AddChain.begin(), AddChain.end(), LessThan);

  // Predict the result arrival time according to the operand bitwidth and number.
  float OpArrivalTime = 0.0f;
  unsigned OpBitWidth = 0;
  for (unsigned i = 0; i < AddChain.size(); ++i) {
    OpArrivalTime = std::max(OpArrivalTime, AddChain[i].second);
    OpBitWidth = std::max(OpBitWidth, getOperandBitWidth(Matrix[AddChain[i].first]));
  }

  float ResultArrivalTime
    = OpArrivalTime + getAddChainDelay(OpBitWidth, AddChain.size());

  return ResultArrivalTime;
}

void SIRAddMulChain::generateAddChain(MatrixType Matrix,
                                      std::vector<std::pair<unsigned, float> > Ops,
                                      std::string ResultName, raw_fd_ostream &Output) {
  // Sort the operands of add chain in ascending order of delay.
  std::sort(Ops.begin(), Ops.end(), LessThan);

  // Get the bitwidth information. To be noted that, this bitwidth is not corresponding
  // to the adder delay since it don't consider the sign bit pattern.
  unsigned BitWidth = Matrix[0].size();

  // Generate the implementation of the add chain operands.
  std::vector<std::string> OpNames;
  for (unsigned i = 0; i < Ops.size(); ++i) {
    // The name of current add chain operand.
    std::string OpName = ResultName + "_op_" + utostr_32(i);
    OpNames.push_back(OpName);

    // The bits of current add chain operand.
    Output << "wire[" + utostr_32(BitWidth - 1) + ":0] " << OpName << " = {";
    for (unsigned j = 0; j < BitWidth; ++j) {
      Output << Matrix[Ops[i].first][BitWidth - 1 - j].first;

      if (j != BitWidth - 1)
        Output << ", ";
    }
    Output << "};\n";
  }

  // Generate the implementation of the add chain.
  Output << "wire[" + utostr_32(BitWidth - 1) + ":0] " << ResultName << " = ";
  for (unsigned i = 0; i < Ops.size(); ++i) {
    Output << OpNames[i];

    if (i != Ops.size() - 1)
      Output << " + ";
  }
  Output << ";\n";
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
  std::sort(OpArrivalTime.begin(), OpArrivalTime.end(), LessThan);
  
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
      if (PotentialAddChainResultTime <= LimitTime && PotentialAddChain.size() <= 3) {
        AddChain = PotentialAddChain;
        continue;
      } else
        break;
    }
  
    // If we succeed to build a add chain, index it.
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
  MatrixType NewTMatrix = transportMatrix(NewMatrix, NewMatrix.size(), ColNum);
  float ResultArrivalTime = compressMatrix(NewTMatrix, MatrixName, NewMatrix.size(), ColNum, Output);

  return ResultArrivalTime;
}

void SIRAddMulChain::replaceWithCompressor() {
  SIRDatapathBuilder Builder(SM, *TD);

  typedef std::map<IntrinsicInst *, std::vector<IntrinsicInst *> >::iterator iterator;
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
  typedef std::map<IntrinsicInst *, std::vector<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    IntrinsicInst *II = I->first;
    std::vector<IntrinsicInst *> Chain = I->second;

    ChainOutput << "Root instruction is " << II->getName();
    ChainOutput << "\n";

    typedef std::vector<IntrinsicInst *>::iterator chain_iterator;
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