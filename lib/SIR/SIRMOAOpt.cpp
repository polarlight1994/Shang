#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/DFGBuild.h"
#include "sir/Passes.h"
#include "sir/BitMaskAnalysis.h"

#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <sstream>
#include "math.h"

using namespace llvm;
using namespace vast;

typedef std::pair<std::string, std::pair<float, unsigned> > DotType;
typedef std::vector<DotType> MatrixRowType;
typedef std::vector<MatrixRowType> MatrixType;

static unsigned Component_NUM = 0;
static bool sortMatrixByArrivalTime = true;
static bool enableBitMaskOpt = true;
static bool useGPCWithCarryChain = true;
static bool sumFirstRowsByAdder = false;

namespace {
struct SIRMOAOpt : public SIRPass {
  static char ID;
  DataLayout *TD;
  SIR *SM;
  DataFlowGraph *DFG;

  // Output for debug.
  std::string Error;
  raw_fd_ostream DebugOutput;

  // To identify if the adder has been visited in collecting operands
  // of the MOAs. So we can avoid a adder been counted in two MOAs which
  // will lead to bad area performance.
  std::set<IntrinsicInst *> VisitedAdders;
  std::vector<IntrinsicInst *> MOAs;
  std::map<IntrinsicInst *, IntrinsicInst *> MOA2PseudoHybridTreeInst;
  std::map<IntrinsicInst *, std::vector<Value *> > MOA2Ops;

  std::map<Value *, float> ValArrivalTime;

  // The component to be used to compress the dot matrix.
  // 1) GPCs
  // 2) AddChains
  class CompressComponent {
  public:
    enum Type {
      GPC,
      GPCWithExtraOne,
      AddChain
    };

  private:
    Type T;
    std::string Name;

    // Input dots & Output dots.
    std::vector<unsigned> InputDotNums;
    unsigned OutputDotNum;

    // Area cost in FPGA
    unsigned Area;
    // Delay cost in FPGA
    float CriticalDelay;

  public:
    // Default constructor
    CompressComponent(Type T, std::string Name, std::vector<unsigned> InputDotNums,
                      unsigned OutputDotNum, unsigned Area, float CriticalDelay)
      : T(T), Name(Name), InputDotNums(InputDotNums), OutputDotNum(OutputDotNum),
        Area(Area), CriticalDelay(CriticalDelay) {}

    std::string getName() { return Name; }
    std::vector<unsigned> getInputDotNums() { return InputDotNums; }
    unsigned getOutputDotNum() { return OutputDotNum; }
    float getCriticalDelay() { return CriticalDelay;  }
    unsigned getArea() { return Area; }
    Type getType() const { return T; }
  };

  // Special GPCs built to sum extra 1'b1 without extra area cost.
  class GPC_with_extra_One : public CompressComponent {
  private:
    unsigned ExtraOneRank;

  public:
    // Default constructor
    GPC_with_extra_One(std::string Name, std::vector<unsigned> InputDotNums,
                       unsigned OutputDotNum, unsigned Area, float CriticalDelay,
                       unsigned ExtraOneRank)
      : CompressComponent(GPCWithExtraOne, Name, InputDotNums,
                          OutputDotNum, Area, CriticalDelay),
      ExtraOneRank(ExtraOneRank) {}

    unsigned getRankOfExtraOne() { return ExtraOneRank; }

    // Methods for support type inquiry through isa, cast and dyn_cast.
    static inline bool classof(const GPC_with_extra_One *Component) { return true;  }
    static inline bool classof(const CompressComponent *Component) {
      return Component->getType() == GPCWithExtraOne;
    }
  };

  // The library of compress components.
  std::vector<CompressComponent *> Library;

  SIRMOAOpt() : SIRPass(ID), DebugOutput("DebugMatrix.txt", Error) {
    initializeSIRMOAOptPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);

  // Optimization on operands of MOA.
  std::vector<Value *> eliminateIdenticalOperands(std::vector<Value *> Operands,
                                                  Value *MOA, unsigned BitWidth);
  std::vector<Value *> OptimizeOperands(std::vector<Value *> Operands,
                                        Value *MOA, unsigned BitWidth);

  void generateHybridTreeForMOA(IntrinsicInst *MOA, raw_fd_ostream &Output);
  void generateHybridTrees();

  void collectMOAOps(IntrinsicInst *MOA);
  void collectMOAs();

  
  MatrixRowType createDotMatrixRow(std::string OpName, unsigned OpWidth,
                                   unsigned ColNum, float ArrivalTime,
                                   unsigned Stage, BitMask Mask);
  MatrixType createDotMatrix(std::vector<Value *> Operands,
                             unsigned RowNum, unsigned ColNum);
  MatrixType sumAllSignBitsInMatrix(MatrixType Matrix,
                                    unsigned RowNum, unsigned ColNum);

  MatrixType sumRowsByAdder(MatrixType Matrix, unsigned ColNum, raw_fd_ostream &Output);

  float getCritialPathDelay(DFGNode *Node);
  float getLatency(Instruction *Inst);
  float getOperandArrivalTime(Value *Operand);

  bool isConstantInt(MatrixRowType Row);
  unsigned getOperandBitWidth(MatrixRowType Row);
  std::vector<unsigned>
    getSignBitNumListInMatrix(MatrixType Matrix);
  std::vector<unsigned>
    getActiveBitNumListInTMatrix(MatrixType TMatrix, unsigned Stage);
  MatrixType
    transportMatrix(MatrixType Matrix, unsigned RowNum, unsigned ColumnNum);

  std::vector<unsigned> getOneBitNumListInTMatrix(MatrixType TMatrix);
  std::vector<unsigned> getBitNumListInTMatrix(MatrixType TMatrix);
  MatrixType simplifyTMatrix(MatrixType TMatrix);
  MatrixType sortTMatrix(MatrixType TMatrix);
  MatrixType sumAllOneBitsInTMatrix(MatrixType TMatrix);
  MatrixType eliminateOneBitInTMatrix(MatrixType TMatrix);

  // Initial the GPCs.
  CompressComponent createAddChainComponent(std::string Name, unsigned OpNum,
                                            unsigned BitWidth, unsigned Area,
                                            float CriticalDelay);
  void initGPCs();
  void initAddChains();
  void initLibrary();

  MatrixType compressTMatrixUsingComponent(MatrixType TMatrix, unsigned ComponentIdx,
                                           unsigned RowNo, unsigned Stage,
                                           raw_fd_ostream &Output);
  unsigned getHighestPriorityComponent(MatrixType TMatrix, unsigned RowNo,
                                       unsigned Stage);
  MatrixType compressTMatrixInStage(MatrixType TMatrix, unsigned Stage,
                                    raw_fd_ostream &Output);
  float compressMatrix(MatrixType TMatrix, std::string MatrixName,
                       unsigned OperandNum, unsigned OperandWidth,
                       raw_fd_ostream &Output);

  float hybridTreeCodegen(MatrixType Matrix, std::string MatrixName,
                          unsigned RowNum, unsigned ColNum, raw_fd_ostream &Output);

  // The function to output verilog and debug files.
  void printTMatrixForDebug(MatrixType TMatrix);
  void printGPCModule(raw_fd_ostream &Output);
  void printAddChainModule(unsigned OpNum, unsigned BitWidth, raw_fd_ostream &Output);
  void printCompressComponent(raw_fd_ostream &Output);
  void printComponentInstance(unsigned ComponentIdx,
                              std::vector<std::vector<DotType> > InputDots,
                              std::string OutputName, raw_fd_ostream &Output);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(BitMaskAnalysisID);
    AU.addRequiredID(DFGBuildID);
    AU.addRequiredID(DFGOptID);
    AU.addRequiredID(DFGAnalysisID);
    AU.setPreservesAll();
  }
};
}

char SIRMOAOpt::ID = 0;
char &llvm::SIRMOAOptID = SIRMOAOpt::ID;
INITIALIZE_PASS_BEGIN(SIRMOAOpt, "sir-multi-operand-optimization",
                      "Perform the multi-operand adder optimization",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(BitMaskAnalysis)
  INITIALIZE_PASS_DEPENDENCY(DFGBuild)
  INITIALIZE_PASS_DEPENDENCY(DFGOpt)
  INITIALIZE_PASS_DEPENDENCY(DFGAnalysis)
INITIALIZE_PASS_END(SIRMOAOpt, "sir-multi-operand-optimization",
                    "Perform the multi-operand adder optimization",
                    false, true)

static bool LessThan(std::pair<unsigned, float> OpA,
                     std::pair<unsigned, float> OpB) {
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

bool SIRMOAOpt::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  // Get the DFG.
  DFGBuild &DB = getAnalysis<DFGBuild>();
  this->DFG = DB.getDFG();

  errs() << "==========Compressor Tree Synthesis Start==========\n";

  // Extract multi-operand adders
  collectMOAs();

  // Generate hybrid tree.
  generateHybridTrees();

  errs() << "==========Compressor Tree Synthesis End============\n";

  return false;
}

float SIRMOAOpt::hybridTreeCodegen(MatrixType Matrix, std::string MatrixName,
                                   unsigned RowNum, unsigned ColNum,
                                   raw_fd_ostream &Output) {
  // Print the declaration of the module.  
  Output << "module " << "compressor_" << MatrixName << "(\n";
  for (unsigned i = 0; i < RowNum; ++i) {
    Output << "\tinput wire[";
    Output << utostr_32(ColNum - 1) << ":0] operand_" << utostr_32(i) << ",\n";
  }
  Output << "\toutput wire[";
  Output << utostr_32(ColNum - 1) << ":0] result\n);\n\n";

  // Optimize the dot matrix taking advantage of the sign bits.
  Matrix = sumAllSignBitsInMatrix(Matrix, RowNum, ColNum);
  RowNum = Matrix.size();
  printTMatrixForDebug(Matrix);

  // Consider use normal adder to sum some rows which have earlier arrival time.
  if (sumFirstRowsByAdder) {
    Matrix = sumRowsByAdder(Matrix, ColNum, Output);
    RowNum = Matrix.size();
  }

  // Optimize the dot matrix taking advantage of the known bits.
  MatrixType TMatrix = transportMatrix(Matrix, RowNum, ColNum);
  TMatrix = sumAllOneBitsInTMatrix(TMatrix);
  ++RowNum;
  Matrix = transportMatrix(TMatrix, ColNum, RowNum);
  printTMatrixForDebug(Matrix);

  // After these optimization, the first row of dot matrix
  // should be a constant integer.
  assert(isConstantInt(Matrix[0]) && "Should be a constant integer!");

  // Compress the dot matrix.
  TMatrix = transportMatrix(Matrix, Matrix.size(), ColNum);
  float ResultArrivalTime = compressMatrix(TMatrix, MatrixName, Matrix.size(), ColNum, Output);

  return ResultArrivalTime;
}

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

float SIRMOAOpt::getCritialPathDelay(DFGNode *Node) {
  /// Get the delay of this node according to its type.
  DFGNode::NodeType Ty = Node->getType();
  switch (Ty) {
  case llvm::DFGNode::Entry:
  case llvm::DFGNode::Exit:
  case llvm::DFGNode::Argument:
  case llvm::DFGNode::ConstantInt:
  case llvm::DFGNode::BitExtract:
  case llvm::DFGNode::BitCat:
  case llvm::DFGNode::BitRepeat:
  case llvm::DFGNode::BitManipulate:
  case llvm::DFGNode::TypeConversion:
  case llvm::DFGNode::Ret:
  case llvm::DFGNode::InValid:
    return 0.0f;

  case llvm::DFGNode::GlobalVal:
    return VFUs::WireDelay;

  case llvm::DFGNode::Add: {
    unsigned BitWidth = Node->getBitWidth();
    return LuaI::Get<VFUAddSub>()->lookupLatency(std::min(BitWidth, 64u));
  }
  case llvm::DFGNode::Mul: {
    unsigned BitWidth = Node->getBitWidth();
    return LuaI::Get<VFUMult>()->lookupLatency(std::min(BitWidth, 64u));
  }
  case llvm::DFGNode::Div: {
    unsigned BitWidth = Node->getBitWidth();
    return LuaI::Get<VFUDiv>()->lookupLatency(std::min(BitWidth, 64u));
  }

  case llvm::DFGNode::LShr:
  case llvm::DFGNode::AShr:
  case llvm::DFGNode::Shl: {
    unsigned BitWidth = Node->getBitWidth();
    return LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));
  }

  case llvm::DFGNode::Not:
  case llvm::DFGNode::And:
  case llvm::DFGNode::Or:
  case llvm::DFGNode::Xor:
    return VFUs::LUTDelay + VFUs::WireDelay;

  case llvm::DFGNode::RAnd: {
    unsigned BitWidth = Node->getBitWidth();

    unsigned LogicLevels = LogCeiling(BitWidth, VFUs::MaxLutSize);
    return LogicLevels * (VFUs::LUTDelay + VFUs::WireDelay);
  }

  case llvm::DFGNode::LogicOperationChain: {
    unsigned OperandNum = Node->parent_size();

    unsigned LogicLevels = LogCeiling(OperandNum, VFUs::MaxLutSize);
    return LogicLevels * (VFUs::LUTDelay + VFUs::WireDelay);
  }

  case llvm::DFGNode::GT:
  case llvm::DFGNode::LT:
  case llvm::DFGNode::EQ:
  case llvm::DFGNode::NE: {
    unsigned BitWidth = Node->getBitWidth();
    return LuaI::Get<VFUICmp>()->lookupLatency(std::min(BitWidth, 64u));
  }

  case llvm::DFGNode::CompressorTree:
    llvm_unreachable("Not handled yet!");
    return 0.0f;
  case llvm::DFGNode::Register:
    return VFUs::WireDelay;

  default:
    llvm_unreachable("Not handled yet!");
    return 0.0f;
  }
}

float SIRMOAOpt::getOperandArrivalTime(Value *Operand) {
  SM->indexKeepVal(Operand);

  // Get the corresponding DFG node.
  DFGNode *Node = SM->getDFGNodeOfVal(Operand);

  DFGNode::NodeType Ty = Node->getType();
  if (Ty == DFGNode::Not || Ty == DFGNode::And ||
      Ty == DFGNode::Or || Ty == DFGNode::Xor) {
    if (DFG->hasReplaceNode(Node)) {
      DFGNode *ReplaceNode = DFG->getReplaceNode(Node);

      Node = ReplaceNode;
    }
    else if (DFG->hasLOCNode(Node)) {
      DFGNode *LOCNode = DFG->getLOCNode(Node);
      Node = LOCNode;
    }
  }

  /// Arrival time will be 0.0f if it is a register.
  if (Node->isSequentialNode())
    return VFUs::WireDelay;

  /// Otherwise, traverse the DFG to get the arrival time.
  // Arrival times from different source
  std::map<DFGNode *, float> ArrivalTimes;

  typedef DFGNode::iterator iterator;
  std::vector<std::pair<DFGNode *, iterator> > VisitStack;
  VisitStack.push_back(std::make_pair(Node, Node->parent_begin()));

  // Initialize a arrival time.
  float ArrivalTime = getCritialPathDelay(Node);

  while (!VisitStack.empty()) {
    DFGNode *CurNode = VisitStack.back().first;
    iterator &It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == CurNode->parent_end()) {
      VisitStack.pop_back();

      // Trace back to previous level, so the arrival time
      // also need to be decreased.
      ArrivalTime -= getCritialPathDelay(CurNode);

      continue;
    }

    DFGNode *ParentNode = *It;
    ++It;

    SM->indexKeepVal(ParentNode->getValue());

    // If we reach the sequential node, then record the arrival time.
    if (ParentNode->isSequentialNode()) {
      // Remember to sum up the latency of the sequential node.
      float FinalArrivalTime = ArrivalTime + getCritialPathDelay(ParentNode);

      if (ArrivalTimes.count(ParentNode))
        ArrivalTimes[ParentNode] = std::max(ArrivalTimes[ParentNode], FinalArrivalTime);
      else
        ArrivalTimes.insert(std::make_pair(ParentNode, FinalArrivalTime));

      continue;
    }
    // Else, continue the traverse the DFG in depth-first search.
    VisitStack.push_back(std::make_pair(ParentNode, ParentNode->parent_begin()));
    ArrivalTime += getCritialPathDelay(ParentNode);
  }

  float CritialPathArrivalTime = 0.0f;
  typedef std::map<DFGNode *, float>::iterator arrivaltime_iterator;
  for (arrivaltime_iterator AI = ArrivalTimes.begin(), AE = ArrivalTimes.end();
       AI != AE; ++AI) {
    CritialPathArrivalTime = std::max(CritialPathArrivalTime, AI->second);

    // Index the arrival time.
    ValArrivalTime.insert(std::make_pair(Operand, CritialPathArrivalTime));
  }

  return CritialPathArrivalTime;
}

std::vector<Value *>
SIRMOAOpt::eliminateIdenticalOperands(std::vector<Value *> Operands,
                                      Value *MOA, unsigned BitWidth) {
  // The operands after elimination
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
      Value *ExtractResult
        = Builder.createSBitExtractInst(Op, OpBitWidth - 1, 0,
                                        Builder.createIntegerType(OpBitWidth - 1),
                                        MOA, true);
      Value *ShiftResult
        = Builder.createSBitCatInst(ExtractResult, Builder.createIntegerValue(1, 0),
                                    Op->getType(), MOA, true);

      BitMask OpMask = SM->getBitMask(Op);
      OpMask = OpMask.shl(1);

      SM->IndexVal2BitMask(ShiftResult, OpMask);
      SM->indexKeepVal(ShiftResult);
      FinalOperands.push_back(ShiftResult);
    }
    else {
      llvm_unreachable("Not handled yet!");
    }
  }

  return FinalOperands;
}

MatrixRowType SIRMOAOpt::createDotMatrixRow(std::string OpName, unsigned OpWidth,
                                            unsigned ColNum, float ArrivalTime,
                                            unsigned Stage, BitMask Mask) {
  // The row we will create.
  MatrixRowType Row;

  // Used to denote the sign bit of the operand if it exists.
  std::string SameBit;
  for (unsigned i = 0; i < ColNum; ++i) {
    // When the dot position is within the range of operand bit width,
    // we get the name of dot considering the bit mask.
    if (i < OpWidth) {
      // If it is a known bit, then use the known value.
      if (Mask.isOneKnownAt(i)) {
        Row.push_back(std::make_pair("1'b1", std::make_pair(0.0f, 0)));
        continue;
      }
      else if (Mask.isZeroKnownAt(i)) {
        Row.push_back(std::make_pair("1'b0", std::make_pair(0.0f, 0)));
        continue;
      }
      else if (Mask.isSameKnownAt(i)) {
        if (SameBit.size() != 0)
          Row.push_back(std::make_pair(SameBit, std::make_pair(ArrivalTime, Stage)));
        else {
          SameBit = Mangle(OpName) + "[" + utostr_32(i) + "]";
          Row.push_back(std::make_pair(SameBit, std::make_pair(ArrivalTime, Stage)));
        }
        continue;
      }
      else {
        // Or use the form like operand[0], operand[1]...
        std::string DotName = Mangle(OpName) + "[" + utostr_32(i) + "]";
        Row.push_back(std::make_pair(DotName, std::make_pair(ArrivalTime, Stage)));
      }
    }
    // When the dot position is beyond the range of operand bit width,
    // we need to pad zero into the matrix.
    else {
      Row.push_back(std::make_pair("1'b0", std::make_pair(0.0f, 0)));
    }
  }

  return Row;
}

MatrixType SIRMOAOpt::createDotMatrix(std::vector<Value *> Operands,
                                      unsigned RowNum, unsigned ColNum) {
  MatrixType Matrix;

  // Initial a empty matrix first.
  for (unsigned i = 0; i < RowNum; ++i) {
    MatrixRowType Row;

    for (unsigned j = 0; j < ColNum; ++j)
      Row.push_back(std::make_pair("1'b0", std::make_pair(0.0f, 0)));

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
          Matrix[i][ColNum - 1 - j] = std::make_pair("1'b1", std::make_pair(0.0f, 0));
        else
          Matrix[i][ColNum - 1 - j] = std::make_pair("1'b0", std::make_pair(0.0f, 0));
      }
    }
    // Or the dots will be the form like operand[0], operand[1]...
    else {
      // Get the arrival time of the dots.
      float ArrivalTime = getOperandArrivalTime(Operand);
      // Get the bit mask of the dots.
      DFGNode *OpNode = SM->getDFGNodeOfVal(Operand);
      assert(OpNode && "DFG node not created?");
      BitMask Mask = SM->getBitMask(OpNode);

      errs() << "Operand_" + utostr_32(i) << ": ArrivalTime[ " << ArrivalTime << "], ";
      errs() << "BitMask[";
      Mask.print(errs());
      errs() << "];\n";

      // Get the name of the operand to denote the name of the dot later.
      std::string OpName = "operand_" + utostr_32(i);
      // Used to denote the sign bit of the operand if it exists.
      std::string SameBit;
      for (unsigned j = 0; j < ColNum; ++j) {
        // When the dot position is within the range of operand bit width,
        // we get the name of dot considering the bit mask.
        if (j < OpWidth) {
          // If we enable the optimization based on bitmask analysis. Then
          // the content of dot should be decided by mask.
          if (enableBitMaskOpt) {
            // If it is a known bit, then use the known value.
            if (Mask.isOneKnownAt(j)) {
              Matrix[i][j] = std::make_pair("1'b1", std::make_pair(0.0f, 0));
              continue;
            }
            else if (Mask.isZeroKnownAt(j)) {
              Matrix[i][j] = std::make_pair("1'b0", std::make_pair(0.0f, 0));
              continue;
            }
            else if (Mask.isSameKnownAt(j)) {
              if (SameBit.size() != 0)
                Matrix[i][j] = std::make_pair(SameBit, std::make_pair(ArrivalTime, 0));
              else {
                SameBit = Mangle(OpName) + "[" + utostr_32(j) + "]";
                Matrix[i][j] = std::make_pair(SameBit, std::make_pair(ArrivalTime, 0));
              }
              continue;
            }
          }

          // Or use the form like operand[0], operand[1]...
          std::string DotName = Mangle(OpName) + "[" + utostr_32(j) + "]";
          Matrix[i][j] = std::make_pair(DotName, std::make_pair(ArrivalTime, 0));
        }
        // When the dot position is beyond the range of operand bit width,
        // we need to pad zero into the matrix.
        else {
          Matrix[i][j] = std::make_pair("1'b0", std::make_pair(0.0f, 0));
        }
      }
    }
  }

  return Matrix;
}

std::vector<Value *>
SIRMOAOpt::OptimizeOperands(std::vector<Value *> Operands,
                            Value *MOA, unsigned BitWidth) {
  // Eliminate the identical operands in add chain.
  std::vector<Value *>
    OptOperands = eliminateIdenticalOperands(Operands, MOA, BitWidth);

  return OptOperands;
}

void SIRMOAOpt::generateHybridTreeForMOA(IntrinsicInst *MOA,
                                         raw_fd_ostream &Output) {
  // Get all the operands of MOA.
  std::vector<Value *> Operands = MOA2Ops[MOA];

  // Optimize operands if there exists
  // 1) identical operands.
  std::vector<Value *> OptOperands = Operands;
  OptOperands
    = OptimizeOperands(Operands, MOA, TD->getTypeSizeInBits(MOA->getType()));

  // Index the connection of operands and pseudo hybrid tree instruction.
  // So we can generate the instance of the hybrid tree module.
  IntrinsicInst *PseudoHybridTreeInst = MOA2PseudoHybridTreeInst[MOA];
  SM->IndexOps2AdderChain(PseudoHybridTreeInst, OptOperands);

  // Generate dot matrix.
  unsigned MatrixRowNum = OptOperands.size();
  unsigned MatrixColNum = TD->getTypeSizeInBits(MOA->getType());
  std::string MatrixName = Mangle(PseudoHybridTreeInst->getName());

  errs() << "Synthesize compressor tree for " << MatrixName << "......\n";

  MatrixType
    Matrix = createDotMatrix(OptOperands, MatrixRowNum, MatrixColNum);

  DebugOutput << "---------- Matrix for " << MatrixName << " ------------\n";
  printTMatrixForDebug(Matrix);

  float ResultArrivalTime = hybridTreeCodegen(Matrix, MatrixName,
                                              MatrixRowNum, MatrixColNum, Output);

  // Index the arrival time of the hybrid tree result.
  ValArrivalTime.insert(std::make_pair(PseudoHybridTreeInst, ResultArrivalTime));
}

void SIRMOAOpt::generateHybridTrees() {
  // Create a pseudo instruction to represent the
  // hybrid tree we created later in SIR.
  SIRDatapathBuilder Builder(SM, *TD);
  typedef std::vector<IntrinsicInst *>::iterator iterator;
  for (iterator I = MOAs.begin(), E = MOAs.end(); I != E; ++I) {
    Value *PseudoHybridTreeVal = Builder.createCompressorInst(*I);

    // Index the pseudo instruction as KeepVal so it will
    // not be eliminated in optimization.
    SM->indexKeepVal(PseudoHybridTreeVal);

    // Index the connection of pseudo instruction and
    // the multi-operand adder.
    IntrinsicInst *PseudoHybridTreeInst
      = dyn_cast<IntrinsicInst>(PseudoHybridTreeVal);
    MOA2PseudoHybridTreeInst.insert(std::make_pair(*I, PseudoHybridTreeInst));
  }

  // Initialize the library of compress component.
  initLibrary();

  // Initialize the output file for hybrid tree.
  std::string CompressorOutputPath = LuaI::GetString("CompressorOutput");
  std::string Error;
  raw_fd_ostream Output(CompressorOutputPath.c_str(), Error);

  // Generate hybrid tree for each multi-operand adder.
  for (iterator I = MOAs.begin(), E = MOAs.end(); I != E; ++I) {
    generateHybridTreeForMOA(*I, Output);
  }

  // Print the compress component modules.
  printCompressComponent(Output);
}

void SIRMOAOpt::collectMOAs() {
  std::vector<IntrinsicInst *> MOAVector;

  Function *F = SM->getFunction();

  typedef Function::iterator bb_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      if (IntrinsicInst *InstII = dyn_cast<IntrinsicInst>(Inst)) {
        if (InstII->getIntrinsicID() == Intrinsic::shang_add ||
            InstII->getIntrinsicID() == Intrinsic::shang_addc) {
          unsigned UserNum = 0;
          unsigned UsedByChainNum = 0;
          typedef Value::use_iterator use_iterator;
          for (use_iterator UI = InstII->use_begin(), UE = InstII->use_end();
               UI != UE; ++UI) {
            Value *UserVal = *UI;

            if (IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(UserVal)) {
              ++UserNum;

              if (UserInst->getIntrinsicID() == Intrinsic::shang_add ||
                  UserInst->getIntrinsicID() == Intrinsic::shang_addc)
                ++UsedByChainNum;
            }
          }

          if (UsedByChainNum == 0 || UserNum >= 2)
            MOAVector.push_back(InstII);
        }
      }
    }
  }

  for (unsigned i = 0; i < MOAVector.size(); ++i) {
    IntrinsicInst *MOA = MOAVector[i];
    collectMOAOps(MOA);
  }
}

void SIRMOAOpt::collectMOAOps(IntrinsicInst *MOA) {
  assert(MOA->getIntrinsicID() == Intrinsic::shang_add ||
         MOA->getIntrinsicID() == Intrinsic::shang_addc &&
         "Unexpected intrinsic instruction type!");

  /// The MOA in SIR is not a single instruction but a bunch
  /// of add instructions. So we need to find them and extract
  /// their operands.
  typedef Instruction::op_iterator op_iterator;
  std::vector<std::pair<IntrinsicInst *, op_iterator> > VisitStack;

  VisitStack.push_back(std::make_pair(MOA, MOA->op_begin()));

  unsigned Depth = 0;
  std::vector<IntrinsicInst *> AdderVector;
  while(!VisitStack.empty()) {
    IntrinsicInst *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    assert(CurNode->getIntrinsicID() == Intrinsic::shang_add ||
           CurNode->getIntrinsicID() == Intrinsic::shang_addc &&
           "Unexpected type!");

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      if (Depth != 0) {
        AdderVector.push_back(CurNode);
        VisitedAdders.insert(CurNode);
      }

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *I;
    ++I;
    IntrinsicInst *ChildInst = dyn_cast<IntrinsicInst>(ChildVal);

    if (!ChildInst)
      continue;

    if (VisitedAdders.count(ChildInst))
      continue;

    unsigned UsedByChainNum = 0;
    typedef Value::use_iterator use_iterator;
    for (use_iterator UI = ChildInst->use_begin(), UE = ChildInst->use_end();
         UI != UE; ++UI) {
      Value *UserVal = *UI;

      if (IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(UserVal))
        ++UsedByChainNum;
    }
    if (UsedByChainNum >= 2)
      continue;

    if (ChildInst->getIntrinsicID() == Intrinsic::shang_add ||
        ChildInst->getIntrinsicID() == Intrinsic::shang_addc) {
      VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
      Depth++;
    }
  }

  if (Depth != 0 && AdderVector.size() > 2) {
    MOAs.push_back(MOA);
    VisitedAdders.insert(MOA);

    // Collect all the operands of MOA.
    std::set<IntrinsicInst *> AdderSet;
    typedef std::vector<IntrinsicInst *>::iterator iterator;
    for (iterator I = AdderVector.begin(), E = AdderVector.end(); I != E; ++I)
      AdderSet.insert(*I);

    std::vector<Value *> Operands;
    for (iterator I = AdderVector.begin(), E = AdderVector.end(); I != E; ++I) {
      IntrinsicInst *Adder = *I;

      for (unsigned i = 0; i < Adder->getNumOperands() - 1; ++i) {
        Value *Operand = Adder->getOperand(i);

        // Ignore the adder instruction itself.
        if (IntrinsicInst *OperandInst = dyn_cast<IntrinsicInst>(Operand)) {
          if (AdderSet.count(OperandInst))
            continue;
        }

        Operands.push_back(Operand);
      }
    }

    // Index the connection between MOA and its operands.
    MOA2Ops.insert(std::make_pair(MOA, Operands));
  }
}

bool SIRMOAOpt::isConstantInt(MatrixRowType Row) {
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

unsigned SIRMOAOpt::getOperandBitWidth(MatrixRowType Row) {
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

std::vector<unsigned> SIRMOAOpt::getSignBitNumListInMatrix(MatrixType Matrix) {
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
    if (SignBit.first == "1'b0" || SignBit.first == "1'b1") {
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

MatrixType SIRMOAOpt::sumAllSignBitsInMatrix(MatrixType Matrix, unsigned RowNum,
                                             unsigned ColNum) {
  // Get the number of sign bit in each row in Matrix.
  std::vector<unsigned> SignBitNumList = getSignBitNumListInMatrix(Matrix);

  // Sum all sign bit using the equation:
  // ssssssss = 11111111 + 0000000~s,
  // then sum all the one bit.
  MatrixType SignBitMatrix = Matrix;
  for (unsigned i = 0; i < Matrix.size(); ++i) {
    // If there is sign bit pattern in current row.
    if (SignBitNumList[i] > 1) {
      unsigned SignBitStartPoint = Matrix[i].size() - SignBitNumList[i];

      // Set the ssssssss to 0000000~s in origin Matrix.
      DotType OriginDot = Matrix[i][SignBitStartPoint];
      Matrix[i][SignBitStartPoint] = std::make_pair("~" + OriginDot.first, OriginDot.second);
      for (unsigned j = SignBitStartPoint + 1; j < Matrix[i].size(); ++j)
        Matrix[i][j] = std::make_pair("1'b0", std::make_pair(0.0f, 0));

      // Set the non-sign bit to 00000000 in sign bit Matrix.
      for (unsigned j = 0; j < SignBitStartPoint; ++j)
        SignBitMatrix[i][j] = std::make_pair("1'b0", std::make_pair(0.0f, 0));
      // Set the ssssssss to 11111111 in sign bit Matrix.
      for (unsigned j = SignBitStartPoint; j < SignBitMatrix[i].size(); ++j)
        SignBitMatrix[i][j] = std::make_pair("1'b1", std::make_pair(0.0f, 0));
    }
    // If there is no sign bit pattern in current row.
    else {
      // Set all bits to 00000000 in sign bit Matrix.
      for (unsigned j = 0; j < SignBitMatrix[i].size(); ++j)
        SignBitMatrix[i][j] = std::make_pair("1'b0", std::make_pair(0.0f, 0));
    }
  }

  MatrixType SignBitTMatrix = transportMatrix(SignBitMatrix, RowNum, ColNum);
  SignBitTMatrix = sumAllOneBitsInTMatrix(SignBitTMatrix);

  SignBitMatrix = transportMatrix(SignBitTMatrix, ColNum, ++RowNum);  
  MatrixRowType Row = SignBitMatrix[0];
  Matrix.push_back(SignBitMatrix[0]);

  return Matrix;
}

bool ArrivalTimeCompare(const std::pair<int, float> AT1,
                        const std::pair<int, float> AT2) {
  if (AT1.second < AT2.second)
    return true;
  else
    return false;
}

MatrixType SIRMOAOpt::sumRowsByAdder(MatrixType Matrix, unsigned ColNum,
                                     raw_fd_ostream &Output) {
  /// First we should check each row by its arrival time to identify which rows can be
  /// summed by adder without increasing critical path delay of compressor tree.

  // Collect the arrival time of each row and sort it in ascending order.
  std::vector<std::pair<int, float> > ArrivalTimes;
  for (unsigned i = 0; i < Matrix.size(); ++i) {
    MatrixRowType Row = Matrix[i];

    // Ignore the constant int row.
    if (isConstantInt(Row))
      continue;

    float ArrivalTime = 0.0f;

    for (unsigned j = 0; j < Row.size(); ++j) {
      DotType Dot = Row[j];

      if (Dot.second.first != 0.0f) {
        if (ArrivalTime == 0.0f)
          ArrivalTime = Dot.second.first;
        else
          assert(ArrivalTime == Dot.second.first && "Unexpected arrival time of dot!");
      }      
    }

    assert(ArrivalTime != 0.0f || isConstantInt(Row) && "Unexpected arrival time of row!");
    ArrivalTimes.push_back(std::make_pair(i, ArrivalTime));
  }
  std::sort(ArrivalTimes.begin(), ArrivalTimes.end(), ArrivalTimeCompare);

  // Identify which rows can be summed by adder.
  // TODO: only identify the first three rows now.
  std::vector<std::vector<unsigned> > RowsSummedByAdder;  
  float InputArrivalTime = std::max(ArrivalTimes[0].second,
                                    std::max(ArrivalTimes[1].second, ArrivalTimes[2].second));
  float ResultArrivalTime = InputArrivalTime + 1.436f / VFUs::Period;

  if (ResultArrivalTime < ArrivalTimes.back().second) {
    std::vector<unsigned> AdderOps;
    AdderOps.push_back(ArrivalTimes[0].first);
    AdderOps.push_back(ArrivalTimes[1].first);
    AdderOps.push_back(ArrivalTimes[2].first);

    RowsSummedByAdder.push_back(AdderOps);

    errs() << "Sum first arrived three rows by adder!\n";
  }

  /// Then according to the result, generate the adder to sum these rows and remember to
  /// insert the add result back into matrix as new rows.

  // Generate the adders to sum the chosen rows.
  for (unsigned i = 0; i < RowsSummedByAdder.size(); ++i) {
    std::vector<unsigned> AdderOpIdxs = RowsSummedByAdder[i];

    // The operands of current adder.
    for (unsigned j = 0; j < AdderOpIdxs.size(); ++j) {
      unsigned AdderOpIdx = AdderOpIdxs[j];

      Output << "wire[" << utostr_32(ColNum - 1)
             << ":0] Adder_" + utostr_32(i) + "_Op_" + utostr_32(j) << " = {";

      for (unsigned k = 0; k < ColNum; ++k) {
        Output << Matrix[AdderOpIdx][ColNum - 1 - k].first;

        if (k != ColNum - 1)
          Output << ", ";
        else
          Output << "};\n";
      }
    }

    // The implement of current adder.
    Output << "wire[" << utostr_32(ColNum - 1) << ":0] Adder_0 = ";
    for (unsigned j = 0; j < AdderOpIdxs.size(); ++j) {
      Output << "Adder_" + utostr_32(i) + "_Op_" + utostr_32(j);

      if (j != AdderOpIdxs.size() - 1)
        Output << " + ";
      else
        Output << ";\n\n";
    }
  }

  // Insert the adder result back into matrix.
  for (unsigned i = 0; i < RowsSummedByAdder.size(); ++i) {
    std::string AdderName = "Adder_" + utostr_32(i);

    // Get the mask for each operand row of current adder.
    std::vector<BitMask> AdderOpMasks;
    std::vector<unsigned> AdderOpIdxs = RowsSummedByAdder[i];
    for (unsigned j = 0; j < AdderOpIdxs.size(); ++j) {
      unsigned AdderOpIdx = AdderOpIdxs[j];
      MatrixRowType AdderOp = Matrix[AdderOpIdx];

      // Initialize a empty mask.
      BitMask Mask = BitMask(ColNum);

      // Set each bit of mask according to the dot in row.
      for (unsigned k = 0; k < ColNum; ++k) {
        if (AdderOp[k].first == "1'b0")
          Mask.setKnownZeroAt(k);
        else if (AdderOp[k].first == "1'b1")
          Mask.setKnownOneAt(k);
      }

      AdderOpMasks.push_back(Mask);
    }

    // Calculate the mask of adder result.
    BitMask ResultMask
      = BitMaskAnalysis::computeAdd(AdderOpMasks[0], AdderOpMasks[1], ColNum);
    for (unsigned i = 2; i < AdderOpMasks.size(); ++i) {
      ResultMask = BitMaskAnalysis::computeAdd(AdderOpMasks[i], ResultMask, ColNum);
    }

    MatrixRowType AdderResultRow
      = createDotMatrixRow(AdderName, ColNum, ColNum, 0.0f, 0, ResultMask);

    // Insert the adder result back into matrix.
    Matrix.push_back(AdderResultRow);
  }

  // Clear the rows summed by adder in matrix.
  for (unsigned i = 0; i < RowsSummedByAdder.size(); ++i) {
    std::vector<unsigned> AdderOpIdxs = RowsSummedByAdder[i];

    for (unsigned j = 0; j < AdderOpIdxs.size(); ++j) {
      unsigned AdderOpIdx = AdderOpIdxs[j];  

      for (unsigned k = 0; k < ColNum; ++k) {
        Matrix[AdderOpIdx][k] = std::make_pair("1'b0", std::make_pair(0.0f, 0));
      }
    }
  }

  return Matrix;
}

MatrixType SIRMOAOpt::transportMatrix(MatrixType Matrix, unsigned RowNum,
                                      unsigned ColumnNum) {
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

std::vector<unsigned> SIRMOAOpt::getOneBitNumListInTMatrix(MatrixType TMatrix) {
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

std::vector<unsigned> SIRMOAOpt::getBitNumListInTMatrix(MatrixType TMatrix) {
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

std::vector<unsigned>
SIRMOAOpt::getActiveBitNumListInTMatrix(MatrixType TMatrix,
                                        unsigned Stage) {
  std::vector<unsigned> ActiveBitNumList;

  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType Row = TMatrix[i];

    // If there are no dot in current row.
    if (Row.size() == 0) {
      ActiveBitNumList.push_back(0);
      continue;
    }

    unsigned BitNum = 0;
    for (unsigned j = 0; j < Row.size(); ++j) {
      if (Row[j].first != "1'b0" && Row[j].second.second <= Stage)
        ++BitNum;
    }

    ActiveBitNumList.push_back(BitNum);
  }

  return ActiveBitNumList;
}

MatrixType SIRMOAOpt::simplifyTMatrix(MatrixType TMatrix) {
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
  unsigned DotAStage = DotA.second.second;
  unsigned DotBStage = DotB.second.second;
  float DotADelay = DotA.second.first;
  float DotBDelay = DotB.second.first;

  if (sortMatrixByArrivalTime) {
    if (DotAStage < DotBStage)
      return true;
    else if (DotAStage > DotBStage)
      return false;
    else {
      if (DotADelay < DotBDelay)
        return true;
      else
        return false;
    }
  }
  else {
    if (DotAStage < DotBStage)
      return true;
    else
      return false;
  }
}

MatrixType SIRMOAOpt::sortTMatrix(MatrixType TMatrix) {
  // Sort the bits according to #1: stage, #2: delay.
  MatrixType SortedTMatrix;

  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType SimplifiedTRow = TMatrix[i];

    std::sort(SimplifiedTRow.begin(), SimplifiedTRow.end(), DotCompare);

    SortedTMatrix.push_back(SimplifiedTRow);
  }

  return SortedTMatrix;
}

MatrixType SIRMOAOpt::sumAllOneBitsInTMatrix(MatrixType TMatrix) {
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

    TRowAfterSum.push_back(std::make_pair(DotName, std::make_pair(0.0f, 0)));

    // Insert other origin bits other than one bits which will be eliminated.
    MatrixRowType TRow = TMatrix[i];
    for (unsigned j = 0; j < TRow.size(); ++j) {
      if (TRow[j].first != "1'b1")
        TRowAfterSum.push_back(TRow[j]);
      else
        TRowAfterSum.push_back(std::make_pair("1'b0", std::make_pair(0.0f, 0)));
    }

    TMatrixAfterSum.push_back(TRowAfterSum);
  }

  return TMatrixAfterSum;
}

MatrixType SIRMOAOpt::eliminateOneBitInTMatrix(MatrixType TMatrix) {
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

    TMatrix[i][0] = std::make_pair("1'b0", std::make_pair(0.0f, 0));
    TMatrix[i][1] = std::make_pair("1'b0", std::make_pair(0.0f, 0));

    TMatrix[i].push_back(std::make_pair(SumName, Row[1].second));
    if (i + 1 < TMatrix.size())
      TMatrix[i + 1].push_back(std::make_pair(CarryName, Row[1].second));
  }

  return TMatrix;
}

SIRMOAOpt::CompressComponent
SIRMOAOpt::createAddChainComponent(std::string Name, unsigned OpNum,
                                   unsigned BitWidth, unsigned Area,
                                   float CriticalDelay) {
  // Inputs
  std::vector<unsigned> AddChain_InputsVector;
  for (unsigned i = 0; i < BitWidth; ++i)
    AddChain_InputsVector.push_back(OpNum);

  // Output
  unsigned OutputBitWidth = BitWidth + std::ceil(log(OpNum) / log(2));

  CompressComponent AddChain(CompressComponent::AddChain, Name,
                             AddChain_InputsVector, OutputBitWidth,
                             Area, CriticalDelay);

  return AddChain;
}

void SIRMOAOpt::initGPCs() {
  CompressComponent::Type GPCType = CompressComponent::GPC;
  CompressComponent::Type GPCWithExtraOneType = CompressComponent::GPCWithExtraOne;

  /// GPC_3_2_LUT
  // Inputs & Outputs
  unsigned GPC_3_2_LUT_Inputs[1] = { 3 };
  std::vector<unsigned> GPC_3_2_LUT_InputsVector(GPC_3_2_LUT_Inputs,
                                                 GPC_3_2_LUT_Inputs + 1);

  CompressComponent *GPC_3_2_LUT
    = new CompressComponent(GPCType, "GPC_3_2_LUT",
                            GPC_3_2_LUT_InputsVector, 2, 1, 0.052f);
  Library.push_back(GPC_3_2_LUT);

  /// GPC_4_3_LUT
  // Inputs & Outputs
  unsigned GPC_4_3_LUT_Inputs[1] = { 4 };
  std::vector<unsigned> GPC_4_3_LUT_InputsVector(GPC_4_3_LUT_Inputs,
                                                 GPC_4_3_LUT_Inputs + 1);

  CompressComponent *GPC_4_3_LUT
    = new CompressComponent(GPCType, "GPC_4_3_LUT",
                            GPC_4_3_LUT_InputsVector, 3, 2, 0.051f);
  Library.push_back(GPC_4_3_LUT);

  /// GPC_5_3_LUT
  // Inputs & Outputs
  unsigned GPC_5_3_LUT_Inputs[1] = { 5 };
  std::vector<unsigned> GPC_5_3_LUT_InputsVector(GPC_5_3_LUT_Inputs,
                                                 GPC_5_3_LUT_Inputs + 1);

  CompressComponent *GPC_5_3_LUT
    = new CompressComponent(GPCType, "GPC_5_3_LUT",
                            GPC_5_3_LUT_InputsVector, 3, 2, 0.049f);
  Library.push_back(GPC_5_3_LUT);

  if (useGPCWithCarryChain) {
    /// GPC_6_3
    // Inputs & Outputs
    unsigned GPC_6_3_Inputs[1] = { 6 };
    std::vector<unsigned> GPC_6_3_InputsVector(GPC_6_3_Inputs,
                                               GPC_6_3_Inputs + 1);

    CompressComponent *GPC_6_3
      = new CompressComponent(GPCType, "GPC_6_3",
                              GPC_6_3_InputsVector, 3, 2, 0.293f);
    Library.push_back(GPC_6_3);
  }
  else {
    /// GPC_6_3_LUT
    // Inputs & Outputs
    unsigned GPC_6_3_LUT_Inputs[1] = { 6 };
    std::vector<unsigned> GPC_6_3_LUT_InputsVector(GPC_6_3_LUT_Inputs,
                                                   GPC_6_3_LUT_Inputs + 1);

    CompressComponent *GPC_6_3_LUT
      = new CompressComponent(GPCType, "GPC_6_3_LUT",
                              GPC_6_3_LUT_InputsVector, 3, 3, 0.049f);
    Library.push_back(GPC_6_3_LUT);
  }
  

//   /// GPC_6_3 with extra 1'b1 in rank of 0
//   // Inputs & Outputs
//   unsigned GPC_6_3_SP_Inputs[2] = { 7 };
//   std::vector<unsigned> GPC_6_3_SP_InputsVector(GPC_6_3_SP_Inputs,
//                                                     GPC_6_3_SP_Inputs + 1);
//   GPC_with_extra_One *GPC_6_3_ExtraOne_Rank0
//     = new GPC_with_extra_One("GPC_6_3_ExtraOne_Rank0",
//                              GPC_6_3_SP_InputsVector, 3, 2, 0.293f, 0);
//   Library.push_back(GPC_6_3_ExtraOne_Rank0);

  /// GPC_13_3_LUT
  // Inputs & Outputs
  unsigned GPC_13_3_LUT_Inputs[2] = { 3, 1 };
  std::vector<unsigned> GPC_13_3_LUT_InputsVector(GPC_13_3_LUT_Inputs,
                                                  GPC_13_3_LUT_Inputs + 2);

  CompressComponent *GPC_13_3_LUT
    = new CompressComponent(GPCType, "GPC_13_3_LUT",
                            GPC_13_3_LUT_InputsVector, 3, 2, 0.051f);
  Library.push_back(GPC_13_3_LUT);

  /// GPC_23_3_LUT
  // Inputs & Outputs
  unsigned GPC_23_3_LUT_Inputs[2] = { 3, 2 };
  std::vector<unsigned> GPC_23_3_LUT_InputsVector(GPC_23_3_LUT_Inputs,
                                                  GPC_23_3_LUT_Inputs + 2);

  CompressComponent *GPC_23_3_LUT
    = new CompressComponent(GPCType, "GPC_23_3_LUT",
                            GPC_23_3_LUT_InputsVector, 3, 2, 0.051f);
  Library.push_back(GPC_23_3_LUT);

  /// GPC_14_3_LUT
  // Inputs & Outputs
  unsigned GPC_14_3_LUT_Inputs[2] = { 4, 1 };
  std::vector<unsigned> GPC_14_3_LUT_InputsVector(GPC_14_3_LUT_Inputs,
                                                  GPC_14_3_LUT_Inputs + 2);

  CompressComponent *GPC_14_3_LUT
    = new CompressComponent(GPCType, "GPC_14_3_LUT",
                            GPC_14_3_LUT_InputsVector, 3, 2, 0.049f);
  Library.push_back(GPC_14_3_LUT);

//   /// GPC_14_3_LUT with extra 1'b1 in rank of 0
//   // Inputs & Outputs
//   unsigned GPC_14_3_LUT_SP_Inputs[2] = { 5, 1 };
//   std::vector<unsigned> GPC_14_3_LUT_SP_InputsVector(GPC_14_3_LUT_SP_Inputs,
//                                                      GPC_14_3_LUT_SP_Inputs + 2);
//   GPC_with_extra_One *GPC_14_3_LUT_ExtraOne_Rank0
//     = new GPC_with_extra_One("GPC_14_3_LUT_ExtraOne_Rank0",
//                              GPC_14_3_LUT_SP_InputsVector, 3, 2, 0.049f, 0);
//   Library.push_back(GPC_14_3_LUT_ExtraOne_Rank0);

  if (useGPCWithCarryChain) {
    /// GPC_15_3
    // Inputs & Outputs
    unsigned GPC_15_3_Inputs[2] = { 5, 1 };
    std::vector<unsigned> GPC_15_3_InputsVector(GPC_15_3_Inputs,
                                                GPC_15_3_Inputs + 2);

    CompressComponent *GPC_15_3
      = new CompressComponent(GPCType, "GPC_15_3",
                              GPC_15_3_InputsVector, 3, 2, 0.274f);
    Library.push_back(GPC_15_3);
  }
  else
  {
    /// GPC_15_3_LUT
    // Inputs & Outputs
    unsigned GPC_15_3_LUT_Inputs[2] = { 5, 1 };
    std::vector<unsigned> GPC_15_3_LUT_InputsVector(GPC_15_3_LUT_Inputs,
                                                    GPC_15_3_LUT_Inputs + 2);

    CompressComponent *GPC_15_3_LUT
      = new CompressComponent(GPCType, "GPC_15_3_LUT",
                              GPC_15_3_LUT_InputsVector, 3, 3, 0.049f);
    Library.push_back(GPC_15_3_LUT);
  }
  
  if (useGPCWithCarryChain) {
    /// GPC_506_5
    // Inputs & Outputs
    unsigned GPC_506_5_Inputs[3] = { 6, 0, 5 };
    std::vector<unsigned> GPC_506_5_InputsVector(GPC_506_5_Inputs,
                                                 GPC_506_5_Inputs + 3);

    CompressComponent *GPC_506_5
      = new CompressComponent(GPCType, "GPC_506_5",
                              GPC_506_5_InputsVector, 5, 4, 0.31f);
    Library.push_back(GPC_506_5);

    //   /// GPC_506_5 with extra 1'b1 in rank of 0
    //   // Inputs & Outputs
    //   unsigned GPC_506_5_LUT_SP_Inputs[3] = { 7, 0, 5 };
    //   std::vector<unsigned> GPC_506_5_LUT_SP_InputsVector(GPC_506_5_LUT_SP_Inputs,
    //                                                       GPC_506_5_LUT_SP_Inputs + 3);
    //   GPC_with_extra_One *GPC_506_5_ExtraOne_Rank0
    //     = new GPC_with_extra_One("GPC_506_5_ExtraOne_Rank0",
    //                              GPC_506_5_LUT_SP_InputsVector, 5, 4, 0.31f, 0);
    //   Library.push_back(GPC_506_5_ExtraOne_Rank0);

    // GPC_606_5
    // Inputs & Outputs
    unsigned GPC_606_5_Inputs[3] = { 6, 0, 6 };
    std::vector<unsigned> GPC_606_5_InputsVector(GPC_606_5_Inputs,
                                                 GPC_606_5_Inputs + 3);

    CompressComponent *GPC_606_5
      = new CompressComponent(GPCType, "GPC_606_5",
                              GPC_606_5_InputsVector, 5, 4, 0.31f);
    Library.push_back(GPC_606_5);

    //   /// GPC_606_5 with extra 1'b1 in rank of 0
    //   // Inputs & Outputs
    //   unsigned GPC_606_5_LUT_SP_Inputs[3] = { 7, 0, 6 };
    //   std::vector<unsigned> GPC_606_5_LUT_SP_InputsVector(GPC_606_5_LUT_SP_Inputs,
    //                                                       GPC_606_5_LUT_SP_Inputs + 3);
    //   GPC_with_extra_One *GPC_606_5_ExtraOne_Rank0
    //     = new GPC_with_extra_One("GPC_606_5_ExtraOne_Rank0",
    //                              GPC_606_5_LUT_SP_InputsVector, 5, 4, 0.31f, 0);
    //   Library.push_back(GPC_606_5_ExtraOne_Rank0);

    // GPC_1325_5
    // Inputs & Outputs
    unsigned GPC_1325_5_Inputs[4] = { 5, 2, 3, 1 };
    std::vector<unsigned> GPC_1325_5_InputsVector(GPC_1325_5_Inputs,
                                                  GPC_1325_5_Inputs + 4);

    CompressComponent *GPC_1325_5
      = new CompressComponent(GPCType, "GPC_1325_5",
                              GPC_1325_5_InputsVector, 5, 4, 0.302f);
    Library.push_back(GPC_1325_5);

    //   /// GPC_1325_5 with extra 1'b1 in rank of 1
    //   // Inputs & Outputs
    //   unsigned GPC_1325_5_LUT_SP_Inputs[4] = { 5, 3, 3, 1 };
    //   std::vector<unsigned> GPC_1325_5_LUT_SP_InputsVector(GPC_1325_5_LUT_SP_Inputs,
    //                                                        GPC_1325_5_LUT_SP_Inputs + 4);
    //   GPC_with_extra_One *GPC_1325_5_ExtraOne_Rank1
    //     = new GPC_with_extra_One("GPC_1325_5_ExtraOne_Rank1",
    //                              GPC_1325_5_LUT_SP_InputsVector, 5, 4, 0.31f, 1);
    //   Library.push_back(GPC_1325_5_ExtraOne_Rank1);

    // GPC_1406_5
    // Inputs & Outputs
    unsigned GPC_1406_5_Inputs[4] = { 6, 0, 4, 1 };
    std::vector<unsigned> GPC_1406_5_InputsVector(GPC_1406_5_Inputs,
                                                  GPC_1406_5_Inputs + 4);

    CompressComponent *GPC_1406_5
      = new CompressComponent(GPCType, "GPC_1406_5",
                              GPC_1406_5_InputsVector, 5, 4, 0.31f);
    Library.push_back(GPC_1406_5);

    //   /// GPC_1406_5 with extra 1'b1 in rank of 0
    //   // Inputs & Outputs
    //   unsigned GPC_1406_5_LUT_SP_Inputs[4] = { 7, 0, 4, 1 };
    //   std::vector<unsigned> GPC_1406_5_LUT_SP_InputsVector(GPC_1406_5_LUT_SP_Inputs,
    //                                                        GPC_1406_5_LUT_SP_Inputs + 4);
    //   GPC_with_extra_One *GPC_1406_5_ExtraOne_Rank0
    //     = new GPC_with_extra_One("GPC_1406_5_ExtraOne_Rank0",
    //                              GPC_1406_5_LUT_SP_InputsVector, 5, 4, 0.31f, 0);
    //   Library.push_back(GPC_1406_5_ExtraOne_Rank0);

    // GPC_1415_5
    // Inputs & Outputs
    unsigned GPC_1415_5_Inputs[4] = { 5, 1, 4, 1 };
    std::vector<unsigned> GPC_1415_5_InputsVector(GPC_1415_5_Inputs,
                                                  GPC_1415_5_Inputs + 4);

    CompressComponent *GPC_1415_5
      = new CompressComponent(GPCType, "GPC_1415_5",
                              GPC_1415_5_InputsVector, 5, 4, 0.31f);
    Library.push_back(GPC_1415_5);
  }
}

void SIRMOAOpt::initAddChains() {
//   / AddChain with bitwidth of 16
//     // AddChain_2_16
//     CompressComponent
//       AddChain_2_16 = createAddChainComponent("AddChain_2_16", 2, 16, 16, 0.521);
//     Library.push_back(AddChain_2_16);
//   
//     // AddChain_3_16
//     CompressComponent
//       AddChain_3_16 = createAddChainComponent("AddChain_3_16", 3, 16, 16, 0.59);
//     Library.push_back(AddChain_3_16);
//   
//     // AddChain_4_16
//     CompressComponent
//       AddChain_4_16 = createAddChainComponent("AddChain_4_16", 4, 16, 44, 0.706);
//     Library.push_back(AddChain_4_16);
//   
//     // AddChain_5_16
//     CompressComponent
//       AddChain_5_16 = createAddChainComponent("AddChain_5_16", 5, 16, 32, 1.584);
//     Library.push_back(AddChain_5_16);
//   
//     // AddChain_6_16
//     CompressComponent
//       AddChain_6_16 = createAddChainComponent("AddChain_6_16", 6, 16, 60, 1.715);
//     Library.push_back(AddChain_6_16);
//   
//     // AddChain_7_16
//     CompressComponent
//       AddChain_7_16 = createAddChainComponent("AddChain_7_16", 7, 16, 48, 1.822);
//     Library.push_back(AddChain_7_16);
//   
//     // AddChain_8_16
//     CompressComponent
//       AddChain_8_16 = createAddChainComponent("AddChain_8_16", 8, 16, 76, 2.101);
//     Library.push_back(AddChain_8_16);
//   
//     // AddChain_9_16
//     CompressComponent
//       AddChain_9_16 = createAddChainComponent("AddChain_9_16", 9, 16, 64, 2.183);
//     Library.push_back(AddChain_9_16);
//   
//     // AddChain_10_16
//     CompressComponent
//       AddChain_10_16 = createAddChainComponent("AddChain_10_16", 10, 16, 92, 2.009);
//     Library.push_back(AddChain_10_16);
//   
//     /// AddChain with bitwidth of 32
//     // AddChain_2_32
//     CompressComponent
//       AddChain_2_32 = createAddChainComponent("AddChain_2_32", 2, 32, 32, 0.816);
//     Library.push_back(AddChain_2_32);
//   
//     // AddChain_3_32
//     CompressComponent
//       AddChain_3_32 = createAddChainComponent("AddChain_3_32", 3, 32, 32, 0.876);
//     Library.push_back(AddChain_3_32);
//   
//     // AddChain_4_32
//     CompressComponent
//       AddChain_4_32 = createAddChainComponent("AddChain_4_32", 4, 32, 92, 1.163);
//     Library.push_back(AddChain_4_32);
//   
//     // AddChain_5_32
//     CompressComponent
//       AddChain_5_32 = createAddChainComponent("AddChain_5_32", 5, 32, 64, 2.199);
//     Library.push_back(AddChain_5_32);
//   
//     // AddChain_6_32
//     CompressComponent
//       AddChain_6_32 = createAddChainComponent("AddChain_6_32", 6, 32, 124, 2.343);
//     Library.push_back(AddChain_6_32);
//   
//     // AddChain_7_32
//     CompressComponent
//       AddChain_7_32 = createAddChainComponent("AddChain_7_32", 7, 32, 96, 2.343);
//     Library.push_back(AddChain_7_32);
//   
//     // AddChain_8_32
//     CompressComponent
//       AddChain_8_32 = createAddChainComponent("AddChain_8_32", 8, 32, 156, 2.047);
//     Library.push_back(AddChain_8_32);
//   
//     // AddChain_9_32
//     CompressComponent
//       AddChain_9_32 = createAddChainComponent("AddChain_9_32", 9, 32, 128, 2.265);
//     Library.push_back(AddChain_9_32);
//   
//     // AddChain_10_32
//     CompressComponent
//       AddChain_10_32 = createAddChainComponent("AddChain_10_32", 10, 32, 188, 2.504);
//     Library.push_back(AddChain_10_32);
//   
//     /// AddChain with bitwidth of 64
//     // AddChain_2_64
//     CompressComponent
//       AddChain_2_64 = createAddChainComponent("AddChain_2_64", 2, 64, 64, 1.24);
//     Library.push_back(AddChain_2_64);
//   
//     // AddChain_3_64
//     CompressComponent
//       AddChain_3_64 = createAddChainComponent("AddChain_3_64", 3, 64, 64, 1.255);
//     Library.push_back(AddChain_3_64);
//   
//     // AddChain_4_64
//     CompressComponent
//       AddChain_4_64 = createAddChainComponent("AddChain_4_64", 4, 64, 188, 1.574);
//     Library.push_back(AddChain_4_64);
//   
//     // AddChain_5_64
//     CompressComponent
//       AddChain_5_64 = createAddChainComponent("AddChain_5_64", 5, 64, 128, 2.326);
//     Library.push_back(AddChain_5_64);
//   
//     // AddChain_6_64
//     CompressComponent
//       AddChain_6_64 = createAddChainComponent("AddChain_6_64", 6, 64, 254, 2.784);
//     Library.push_back(AddChain_6_64);
//   
//     // AddChain_7_64
//     CompressComponent
//       AddChain_7_64 = createAddChainComponent("AddChain_7_64", 7, 64, 192, 2.143);
//     Library.push_back(AddChain_7_64);
//   
//     // AddChain_8_64
//     CompressComponent
//       AddChain_8_64 = createAddChainComponent("AddChain_8_64", 8, 64, 316, 2.9);
//     Library.push_back(AddChain_8_64);
//   
//     // AddChain_9_64
//     CompressComponent
//       AddChain_9_64 = createAddChainComponent("AddChain_9_64", 9, 64, 256, 2.607);
//     Library.push_back(AddChain_9_64);
//   
//     // AddChain_10_64
//     CompressComponent
//       AddChain_10_64 = createAddChainComponent("AddChain_10_64", 10, 64, 380, 3.033);
//     Library.push_back(AddChain_10_64);
}

void SIRMOAOpt::initLibrary() {
  // Initialize the GPCs.
  initGPCs();

  // Initialize the AddChains.
  //initAddChains();
}

MatrixType
SIRMOAOpt::compressTMatrixUsingComponent(MatrixType TMatrix,
                                         unsigned ComponentIdx,
                                         unsigned RowNo, unsigned Stage,
                                         raw_fd_ostream &Output) {
  // Get information of TMatrix.
  std::vector<unsigned> ActiveBitNumList
    = getActiveBitNumListInTMatrix(TMatrix, Stage);

  // Get the Component to be used.
  CompressComponent *Component = Library[ComponentIdx];

  // Identify if the component is GPC with extra one type.
  bool IsSpecialGPC = isa<GPC_with_extra_One>(Component);
  unsigned RankOfExtraOne = 0;
  if (IsSpecialGPC) {
    GPC_with_extra_One *SpecialGPC = dyn_cast<GPC_with_extra_One>(Component);
    RankOfExtraOne = SpecialGPC->getRankOfExtraOne();

    // Code for debug
    errs() << "Use the special GPC: " << SpecialGPC->getName() << "\n";
  }    

  // Collect input dots.
  float InputArrivalTime = 0.0f;
  std::vector<std::vector<DotType> > InputDots;
  std::vector<unsigned> InputDotNums = Component->getInputDotNums();
  for (unsigned i = 0; i < InputDotNums.size(); ++i) {
    unsigned InputDotNum = InputDotNums[i];

    // The dots to be compressed in current row in TMatrix.
    std::vector<DotType> InputDotRow;
    if (RowNo + i < TMatrix.size()) {
      for (unsigned j = 0; j < InputDotNum; ++j) {
        unsigned DotIdx = j;

        // Reserve the 1'b1 dot so it may be summed by GPC with extra
        // one in process of next row.
        if (i != RankOfExtraOne && TMatrix[RowNo + i][0].first == "1'b1" &&
            InputDotNum < ActiveBitNumList[RowNo + i]) {
          ++DotIdx;
        }

        if (DotIdx < ActiveBitNumList[RowNo + i]) {
          DotType Dot = TMatrix[RowNo + i][DotIdx];

          // Make sure the input is valid if it is a GPC with extra one.
          if (IsSpecialGPC && i == RankOfExtraOne && DotIdx == 0)
            assert(Dot.first == "1'b1" && "Unexpected input dot!");

          InputDotRow.push_back(Dot);

          // Clear input dots in TMatrix.
          TMatrix[RowNo + i][DotIdx] = std::make_pair("1'b0", std::make_pair(0.0f, 0));

          // Collect the arrival time of each input dot.
          InputArrivalTime = std::max(InputArrivalTime, Dot.second.first);

          // Make sure we compress the dots in right stage.
          assert(Dot.second.second <= Stage && "Unexpected dot stage!");
        }
        else {
          InputDotRow.push_back(std::make_pair("1'b0", std::make_pair(0.0f, Stage)));
        }
      }
    }
    else {
      for (unsigned j = 0; j < InputDotNum; ++j)
        InputDotRow.push_back(std::make_pair("1'b0", std::make_pair(0.0f, Stage)));
    }

    InputDots.push_back(InputDotRow);
  }

  // Get name and delay for output dots.
  std::string OutputName
    = "gpc_result_" + utostr_32(Component_NUM++) + "_" + utostr_32(Stage);
  float OutputArrivalTime = InputArrivalTime + Component->getCriticalDelay() + VFUs::WireDelay;

  // Insert the output dots into TMatrix.
  unsigned OutputDotNum = Component->getOutputDotNum();
  for (unsigned i = 0; i < OutputDotNum; ++i) {
    // Do not insert if exceed the range of TMatrix.
    if (RowNo + i >= TMatrix.size())
      break;

    std::string OutputDotName = OutputName + "[" + utostr_32(i) + "]";
    TMatrix[RowNo + i].push_back(std::make_pair(OutputDotName,
                                                std::make_pair(0.0f,
                                                               Stage + 1)));
  }

  // Generate component instance.
  printComponentInstance(ComponentIdx, InputDots,
                         OutputName, Output);

  printTMatrixForDebug(TMatrix);

  return TMatrix;
}

bool sortComponent(std::pair<unsigned, std::pair<float, std::pair<float, unsigned> > > OpA,
                   std::pair<unsigned, std::pair<float, std::pair<float, unsigned> > > OpB) {
  if (OpA.second.first < OpB.second.first)
    return true;
  else if (OpA.second.first > OpB.second.first)
    return false;
  else {
    if (OpA.second.second.first < OpB.second.second.first)
      return true;
    else if (OpA.second.second.first > OpB.second.second.first)
      return false;
    else {
      if (OpA.second.second.second < OpB.second.second.second)
        return true;
      else
        return false;
    }
  }
}

unsigned
SIRMOAOpt::getHighestPriorityComponent(MatrixType TMatrix,
                                       unsigned RowNo, unsigned Stage) {
  unsigned HighestPriorityGPCIdx;

  // Get information of TMatrix.
  std::vector<unsigned> ActiveBitNumList
    = getActiveBitNumListInTMatrix(TMatrix, Stage);

  // Try all library and evaluate its priority.
  typedef std::pair<float, std::pair<float, unsigned> > GPCPriority;
  std::vector<std::pair<unsigned, GPCPriority> > PriorityList;
  for (unsigned i = 0; i < Library.size(); ++i) {
    CompressComponent *Component = Library[i];

    // Get the information of current GPC.
    std::vector<unsigned> InputDotNums = Component->getInputDotNums();
    unsigned OutputDotNum = Component->getOutputDotNum();
    float CriticalDelay = Component->getCriticalDelay();
    unsigned Area = Component->getArea();

    /// Ignore the invalid component.
    // Check if all inputs of component can be fulfilled.
    unsigned RealInputDotNum = 0;
    bool ComponentValid = true;
    if (RowNo + InputDotNums.size() > TMatrix.size())
      ComponentValid = false;
    else {
      for (unsigned j = 0; j < InputDotNums.size(); ++j) {
        if (InputDotNums[j] > ActiveBitNumList[RowNo + j])
          ComponentValid = false;

        RealInputDotNum += InputDotNums[j];
      }
    }
    if (!ComponentValid)
      continue;
    // Check if there exists the needed 1'b1 if the component
    // is the GPCWithExtraOne.
    if (GPC_with_extra_One *SpecialGPC
      = dyn_cast<GPC_with_extra_One>(Component)) {
      unsigned ExtraOneRank = SpecialGPC->getRankOfExtraOne();

      MatrixRowType TargetRow = TMatrix[RowNo + ExtraOneRank];
      if (TargetRow[0].first != "1'b1")
        ComponentValid = false;
    }
    if (!ComponentValid)
      continue;

    unsigned RealOutputDotNum = std::min(OutputDotNum, TMatrix.size() - RowNo + 1);

    // Evaluate the performance.
    unsigned CompressedDotNum = RealInputDotNum > RealOutputDotNum ? 
                                  RealInputDotNum - RealOutputDotNum : 0;

    float RealDelay = CriticalDelay + VFUs::WireDelay;
    //float Performance = ((float) (CompressedDotNum * CompressedDotNum)) / (RealDelay * Area);
    //float Performance = ((float)CompressedDotNum) / RealDelay;
    float Performance = ((float)CompressedDotNum) / Area;

    GPCPriority Priority = std::make_pair(Performance,
                                          std::make_pair(0.0f - CriticalDelay, InputDotNums[0]));
    PriorityList.push_back(std::make_pair(i, Priority));
  }

  // Sort the PriorityList and get the highest one.
  std::sort(PriorityList.begin(), PriorityList.end(), sortComponent);

  // Debug
//   errs() << "Component performance list is as follows:\n";
//   for (unsigned i = 0; i < PriorityList.size(); ++i) {
//     unsigned ComponentIdx = PriorityList[i].first;
// 
//     CompressComponent *Component = Library[ComponentIdx];
// 
//     errs() << Component->getName() << "--" << PriorityList[i].second.first << "\n";
//   }


  return PriorityList.back().first;
}

MatrixType SIRMOAOpt::compressTMatrixInStage(MatrixType TMatrix,
                                             unsigned Stage,
                                             raw_fd_ostream &Output) {
  // Get the informations of the TMatrix.
  std::vector<unsigned> BitNumList = getBitNumListInTMatrix(TMatrix);
  std::vector<unsigned> ActiveBitNumList
    = getActiveBitNumListInTMatrix(TMatrix, Stage);

  // Compress row by row.
  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    // Compress current row if it has more than target final bit numbers.
    while (BitNumList[i] > 3 && ActiveBitNumList[i] >= 3) {
      unsigned ComponentIdx = getHighestPriorityComponent(TMatrix, i, Stage);

      TMatrix = compressTMatrixUsingComponent(TMatrix, ComponentIdx, i,
                                              Stage, Output);

      // Do some clean up and optimize work.
      TMatrix = eliminateOneBitInTMatrix(TMatrix);
      TMatrix = simplifyTMatrix(TMatrix);
      TMatrix = sortTMatrix(TMatrix);

      // Update the informations of the TMatrix.
      BitNumList = getBitNumListInTMatrix(TMatrix);
      ActiveBitNumList = getActiveBitNumListInTMatrix(TMatrix, Stage);
    }
  }

  return TMatrix;
}

float SIRMOAOpt::compressMatrix(MatrixType TMatrix, std::string MatrixName,
                                unsigned OperandNum, unsigned OperandWidth,
                                raw_fd_ostream &Output) {
  // Code for debug.
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

  // Code for debug.
  printTMatrixForDebug(TMatrix);

  /// Start to compress the TMatrix
  unsigned Stage = 0;
  bool Continue = true;
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
    CPADataA_ArrivalTime = std::max(CPADataA_ArrivalTime, TMatrix[i][0].second.first);

    if (TMatrix[i].size() == 1) {
      CPADataB.push_back(std::make_pair("1'b0", std::make_pair(0.0f, 0)));
      CPADataC.push_back(std::make_pair("1'b0", std::make_pair(0.0f, 0)));
    }
    else if (TMatrix[i].size() == 2) {
      CPADataB.push_back(TMatrix[i][1]);
      CPADataB_ArrivalTime = std::max(CPADataB_ArrivalTime, TMatrix[i][1].second.first);

      CPADataC.push_back(std::make_pair("1'b0", std::make_pair(0.0f, 0)));
    }
    else if (TMatrix[i].size() == 3) {
      CPADataB.push_back(TMatrix[i][1]);
      CPADataB_ArrivalTime = std::max(CPADataB_ArrivalTime, TMatrix[i][1].second.first);

      CPADataC.push_back(TMatrix[i][2]);
      CPADataC_ArrivalTime = std::max(CPADataC_ArrivalTime, TMatrix[i][2].second.first);
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
  float CPADelay = LuaI::Get<VFUAddSub>()->lookupLatency(std::min(CPADataA.size(), 64u));;
  float ResultArrivalTime = std::max(std::max(CPADataA_ArrivalTime, CPADataB_ArrivalTime),
                                     CPADataC_ArrivalTime) + CPADelay;

  return ResultArrivalTime;
}

void SIRMOAOpt::printTMatrixForDebug(MatrixType TMatrix) {
  for (unsigned i = 0; i < TMatrix.size(); ++i) {
    MatrixRowType Row = TMatrix[i];

    for (unsigned j = 0; j < Row.size(); ++j) {
      DotType Dot = Row[j];

      DebugOutput << Dot.first/* << "--" << Dot.second.first*/;

      if (j != Row.size() - 1)
        DebugOutput << "  ";
    }

    DebugOutput << "\n";
  }

  DebugOutput << "\n\n";
}

void SIRMOAOpt::printGPCModule(raw_fd_ostream &Output) {
  // Generate the 3-2 compressor.
  Output << "module GPC_3_2_LUT(\n";
  Output << "\tinput wire[2:0] col0,\n";
  Output << "\toutput wire[1:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2];\n\n";
  Output << "endmodule\n\n";

  // Generate the 4-3 compressor.
  Output << "module GPC_4_3_LUT(\n";
  Output << "\tinput wire[3:0] col0,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3];\n\n";
  Output << "endmodule\n\n";

  // Generate the 5-3 compressor.
  Output << "module GPC_5_3_LUT(\n";
  Output << "\tinput wire[4:0] col0,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4];\n\n";
  Output << "endmodule\n\n";

  // Generate the 6-3 compressor.
  Output << "module GPC_6_3(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5];\n\n";
  Output << "endmodule\n\n";

  Output << "module GPC_6_3_LUT(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5];\n\n";
  Output << "endmodule\n\n";

  // Generate the 6-3_ExtraOne_Rank0 compressor.
  Output << "module GPC_6_3_ExtraOne_Rank0(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5] + 1'b1;\n\n";
  Output << "endmodule\n\n";

  // Generate the 13-3 compressor.
  Output << "module GPC_13_3_LUT(\n";
  Output << "\tinput wire[2:0] col0,\n";
  Output << "\tinput wire col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + 2 * col1;\n\n";
  Output << "endmodule\n\n";

  // Generate the 14-3 compressor.
  Output << "module GPC_14_3_LUT(\n";
  Output << "\tinput wire[3:0] col0,\n";
  Output << "\tinput wire col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + 2 * col1;\n\n";
  Output << "endmodule\n\n";

  // Generate the 14-3_ExtraOne_Rank0 compressor.
  Output << "module GPC_14_3_LUT_ExtraOne_Rank0(\n";
  Output << "\tinput wire[3:0] col0,\n";
  Output << "\tinput wire col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + 1'b1 + 2 * col1;\n\n";
  Output << "endmodule\n\n";

  // Generate the 15-3 compressor.
  Output << "module GPC_15_3(\n";
  Output << "\tinput wire[4:0] col0,\n";
  Output << "\tinput wire col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + 2 * col1;\n\n";
  Output << "endmodule\n\n";

  Output << "module GPC_15_3_LUT(\n";
  Output << "\tinput wire[4:0] col0,\n";
  Output << "\tinput wire col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + 2 * col1;\n\n";
  Output << "endmodule\n\n";

  // Generate the 15-3_ExtraOne_Rank0 compressor.
  Output << "module GPC_15_3_LUT_ExtraOne_Rank0(\n";
  Output << "\tinput wire[4:0] col0,\n";
  Output << "\tinput wire col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + 1'b1 + 2 * col1;\n\n";
  Output << "endmodule\n\n";

  // Generate the 23-3 compressor.
  Output << "module GPC_23_3_LUT(\n";
  Output << "\tinput wire[2:0] col0,\n";
  Output << "\tinput wire[1:0] col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + 2 * (col1[0] + col1[1]);\n\n";
  Output << "endmodule\n\n";

  // Generate the 506-5 compressor.
  Output << "module GPC_506_5(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\tinput wire[4:0] col2,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5] ";
  Output << "+ 4 * (col2[0] + col2[1] + col2[2] + col2[3] + col2[4]);\n\n";
  Output << "endmodule\n\n";

  // Generate the 506-5_ExtraOne_Rank0 compressor.
  Output << "module GPC_506_5_ExtraOne_Rank0(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\tinput wire[4:0] col2,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5] + 1'b1";
  Output << "+ 4 * (col2[0] + col2[1] + col2[2] + col2[3] + col2[4]);\n\n";
  Output << "endmodule\n\n";

  // Generate the 606-5 compressor.
  Output << "module GPC_606_5(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\tinput wire[5:0] col2,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5] ";
  Output << "+ 4 * (col2[0] + col2[1] + col2[2] + col2[3] + col2[4] + col2[5]);\n\n";
  Output << "endmodule\n\n";

  // Generate the 606-5_ExtraOne_Rank0 compressor.
  Output << "module GPC_606_5_ExtraOne_Rank0(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\tinput wire[5:0] col2,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5] + 1'b1";
  Output << "+ 4 * (col2[0] + col2[1] + col2[2] + col2[3] + col2[4] + col2[5]);\n\n";
  Output << "endmodule\n\n";

  // Generate the 1325-5 compressor.
  Output << "module GPC_1325_5(\n";
  Output << "\tinput wire[4:0] col0,\n";
  Output << "\tinput wire[1:0] col1,\n";
  Output << "\tinput wire[2:0] col2,\n";
  Output << "\tinput wire col3,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] ";
  Output << "+ 2 * (col1[0] + col1[1]) + 4 * (col2[0] + col2[1] + col2[2]) + 8 * col3;\n\n";
  Output << "endmodule\n\n";

  // Generate the 1325-5_ExtraOne_Rank1 compressor.
  Output << "module GPC_1325_5_ExtraOne_Rank1(\n";
  Output << "\tinput wire[4:0] col0,\n";
  Output << "\tinput wire[1:0] col1,\n";
  Output << "\tinput wire[2:0] col2,\n";
  Output << "\tinput wire col3,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] ";
  Output << "+ 2 * (col1[0] + col1[1] + 1'b1) + 4 * (col2[0] + col2[1] + col2[2]) + 8 * col3;\n\n";
  Output << "endmodule\n\n";

  // Generate the 1406-5 compressor.
  Output << "module GPC_1406_5(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\tinput wire[3:0] col2,\n";
  Output << "\tinput wire col3,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5] ";
  Output << "+ 4 * (col2[0] + col2[1] + col2[2] + col2[3]) + 8 * col3;\n\n";
  Output << "endmodule\n\n";

  // Generate the 1406-5_ExtraOne_Rank0 compressor.
  Output << "module GPC_1406_5_ExtraOne_Rank0(\n";
  Output << "\tinput wire[5:0] col0,\n";
  Output << "\tinput wire[3:0] col2,\n";
  Output << "\tinput wire col3,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] + col0[5] + 1'b1";
  Output << "+ 4 * (col2[0] + col2[1] + col2[2] + col2[3]) + 8 * col3;\n\n";
  Output << "endmodule\n\n";

  // Generate the 1415-5 compressor.
  Output << "module GPC_1415_5(\n";
  Output << "\tinput wire[4:0] col0,\n";
  Output << "\tinput wire col1,\n";
  Output << "\tinput wire[3:0] col2,\n";
  Output << "\tinput wire col3,\n";
  Output << "\toutput wire[4:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + col0[2] + col0[3] + col0[4] ";
  Output << "+ 2 * col1 + 4 * (col2[0] + col2[1] + col2[2] + col2[3]) + 8 * col3;\n\n";
  Output << "endmodule\n\n";
}

void SIRMOAOpt::printAddChainModule(unsigned OpNum, unsigned BitWidth,
                                    raw_fd_ostream &Output) {
  // Calculate the output bitwidth.
  unsigned OutputBitWidth = BitWidth + std::ceil(log(OpNum) / log(2));

  Output << "module AddChain_" << utostr_32(OpNum)
    << "_" << utostr_32(BitWidth) << "(\n";
  for (unsigned i = 0; i < BitWidth; ++i) {
    Output << "\tinput wire[" << utostr_32(OpNum - 1)
      << ":0] col" << utostr_32(i) << ",\n";
  }
  Output << "\toutput wire[" << utostr_32(OutputBitWidth - 1)
    << ":0] sum\n";
  Output << ");\n\n";
  for (unsigned i = 0; i < OpNum; ++i) {
    Output << "\twire[" << utostr_32(BitWidth - 1)
      << ":0] op" << utostr_32(i) << " = {";

    for (unsigned j = 0; j < BitWidth; ++j) {
      Output << "col" << utostr_32(BitWidth - 1 - j)
        << "[" << utostr_32(i) << "]";

      if (j != BitWidth - 1)
        Output << ", ";
    }

    Output << "};\n";
  }
  Output << "\n";

  Output << "\tassign sum = ";
  for (unsigned i = 0; i < OpNum; ++i) {
    Output << "op" << utostr_32(i);

    if (i != OpNum - 1)
      Output << " + ";
  }
  Output << ";\n\n";

  Output << "endmodule\n\n";
}

void SIRMOAOpt::printCompressComponent(raw_fd_ostream &Output) {
  /// Print the module of compress components.
  // GPCs
  printGPCModule(Output);
//   // AddChains
//   for (unsigned i = 2; i < 10; ++i) {
//     printAddChainModule(i, 16, Output);
//     printAddChainModule(i, 32, Output);
//     printAddChainModule(i, 64, Output);
//   }
}

void
SIRMOAOpt::printComponentInstance(unsigned ComponentIdx,
                                  std::vector<std::vector<DotType> > InputDots,
                                  std::string OutputName,
                                  raw_fd_ostream &Output) {
  // Get the Component to be used and its information.
  CompressComponent *Component = Library[ComponentIdx];
  std::vector<unsigned> InputDotNums = Component->getInputDotNums();
  unsigned OutputDotNum = Component->getOutputDotNum();
  std::string ComponentName = Component->getName();

  // Identify the special GPC component.
  bool IsSpecialGPC = isa<GPC_with_extra_One>(Component);
  unsigned RankOfExtraOne = 0;
  if (IsSpecialGPC) {
    GPC_with_extra_One *SpecialGPC = dyn_cast<GPC_with_extra_One>(Component);
    RankOfExtraOne = SpecialGPC->getRankOfExtraOne();
  }

  // Print the declaration of the result.  
  Output << "wire [" << utostr_32(OutputDotNum - 1) << ":0] " << OutputName << ";\n";

  // Print the instantiation of the compressor module.
  Output << ComponentName << " " + ComponentName + "_" << utostr_32(Component_NUM) << "(";

  // Print the inputs and outputs instance.
  for (unsigned i = 0; i < InputDotNums.size(); ++i) {
    // Ignore the empty column.
    if (InputDotNums[i] == 0)
      continue;

    Output << ".col" << utostr_32(i) << "({";

    std::vector<DotType> InputDotRow = InputDots[i];
    assert(InputDotRow.size() == InputDotNums[i] || InputDotRow.size() == 0
      && "Unexpected input dot number!");
    for (unsigned j = 0; j < InputDotRow.size(); ++j) {
      // If this is a special GPC, then do not print the extra 1'b1 input.
      if (IsSpecialGPC && i == RankOfExtraOne && j == 0)
        continue;

      Output << InputDotRow[j].first;

      if (j != InputDotRow.size() - 1)
        Output << ", ";
    }

    Output << "}), ";
  }

  Output << ".sum(" << OutputName << ")";

  Output << ");\n";
}
