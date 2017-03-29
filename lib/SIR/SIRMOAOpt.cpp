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

// Dot name, arrival time and GPC level
typedef std::pair<std::string, std::pair<float, unsigned> > DotType;


static unsigned GPCNum = 0;
static bool sortMatrixByArrivalTime = false;
static bool enableBitMaskOpt = true;
static bool useGPCWithCarryChain = false;
static bool useSepcialGPC = false;
static bool sumFirstRowsByAdder = false;
static unsigned TargetHeight = 3;

namespace {
// The dot in DotMatrix which represents the multi-operand adder
struct MatrixDot {
  // The name of dot
  std::string Name;
  // The arrival time of the value represented by the dot
  float ArrivalTime;
  // The GPC level of the dot
  unsigned Level;
  // If the dot is coming from the booth radix-4 coding of multiplication.
  bool IsMulDot;
  // The bits which involved in the booth radix-4 coding of current dot.
  std::vector<std::string> BoothCodingElements;

  // Default constructor
  MatrixDot(std::string Name, float ArrivalTime, unsigned Level,
            bool IsMulDot, std::vector<std::string> BoothCodingElements)
    : Name(Name), ArrivalTime(ArrivalTime), Level(Level), IsMulDot(IsMulDot),
      BoothCodingElements(BoothCodingElements) {
    if (!IsMulDot)
      assert(BoothCodingElements.empty() && "Unexpected booth coding elements!");
    else
      assert(BoothCodingElements.size() == 5
             && "Unexpected size of booth coding elements!");
  }
  // Replicate the MatrixDot
  MatrixDot(MatrixDot *Dot) {
    this->Name = Dot->getName();
    this->ArrivalTime = Dot->getArrivalTime();
    this->Level = Dot->getLevel();
    this->IsMulDot = Dot->isMulDot();
    this->BoothCodingElements = Dot->getCodingElements();
  }

  std::string getName() const { return Name; }
  float getArrivalTime() const { return ArrivalTime; }
  unsigned getLevel() const { return Level; }

  bool isZero() const { return Name == "1'b0" || Name == "~1'b0"; }
  bool isOne() const { return Name == "1'b1"; }
  bool isMulDot() const { return IsMulDot; }
  std::vector<std::string> getCodingElements() const {
    return BoothCodingElements;
  }
  bool isSameAs(MatrixDot *Dot) const {
    return Name == Dot->getName() &&
           ArrivalTime == Dot->getArrivalTime() &&
           Level == Dot->getLevel();
  }

  void setName(std::string NewName) { this->Name = NewName; }
  void setArrivalTime(float NewArrivalTime) { this->ArrivalTime = NewArrivalTime; }
  void setLevel(unsigned NewLevel) { this->Level = NewLevel; }
  void setToZero() {
    this->Name = "1'b0";
    this->ArrivalTime = 0.0f;
    this->Level = 0;
  }
  void setToOne() {
    this->Name = "1'b1";
    this->ArrivalTime = 0.0f;
    this->Level = 0;
  }
};

// The dot row in DotMatrix
struct MatrixRow {
  // The name of row
  std::string Name;
  // The dots in row
  std::vector<MatrixDot *> Dots;

  MatrixRow() : Name(""), Dots(std::vector<MatrixDot *>()) {}
  MatrixRow(std::string Name) : Name(Name), Dots(std::vector<MatrixDot *>()) {}
  // Replicate the MatrixRow
  MatrixRow(MatrixRow *Row) {
    this->Name = Row->getName();

    for (unsigned i = 0; i < Row->getWidth(); ++i) {
      MatrixDot *ReplicateDot = new MatrixDot(Row->getDot(i));
      this->addDot(ReplicateDot);
    }
  }

  std::string getName() const { return Name; }
  unsigned getWidth() const { return Dots.size(); }

  void addDot(MatrixDot *Dot) {
    Dots.push_back(Dot);
  }
  void addZeroDot() {
    MatrixDot *Zero = new MatrixDot("1'b0", 0.0f, 0, false, std::vector<std::string>());

    Dots.push_back(Zero);
  }
  void addOneDot() {
    MatrixDot *One = new MatrixDot("1'b1", 0.0f, 0, false, std::vector<std::string>());

    Dots.push_back(One);
  }

  MatrixDot *getDot(unsigned DotIdx) {
    assert(DotIdx < getWidth() && "Out of range!");

    return Dots[DotIdx];
  }

  /// Analysis the information about the row.

  bool isConstantInt() {
    bool IsConstantInt = true;

    for (unsigned i = 0; i < Dots.size(); ++i) {
      MatrixDot *Dot = getDot(i);

      if (!Dot->isZero() && !Dot->isOne()) {
        IsConstantInt = false;
        break;
      }
    }

    return IsConstantInt;
  }

  unsigned countingLeadingZeros() {
    unsigned Width = getWidth();
    unsigned LeadingZeros = 0;

    for (unsigned i = 0; i < Width; ++i) {
      MatrixDot *Dot = Dots[Width - 1 - i];

      if (Dot->isZero())
        ++LeadingZeros;
      else
        break;
    }

    return LeadingZeros;
  }
  unsigned countingLeadingOnes() {
    unsigned Width = getWidth();
    unsigned LeadingOnes = 0;

    for (unsigned i = 0; i < Width; ++i) {
      MatrixDot *Dot = Dots[Width - 1 - i];

      if (Dot->isOne())
        ++LeadingOnes;
      else
        break;
    }

    return LeadingOnes;
  }
  unsigned countingTrailingZeros() {
    unsigned Width = getWidth();
    unsigned TrailingZeros = 0;

    for (unsigned i = 0; i < Width; ++i) {
      MatrixDot *Dot = Dots[i];

      if (Dot->isZero())
        ++TrailingZeros;
      else
        break;
    }

    return TrailingZeros;
  }
  unsigned countingTrailingOnes() {
    unsigned Width = getWidth();
    unsigned TrailingOnes = 0;

    for (unsigned i = 0; i < Width; ++i) {
      MatrixDot *Dot = Dots[i];

      if (Dot->isOne())
        ++TrailingOnes;
      else
        break;
    }

    return TrailingOnes;
  }

  unsigned countingLeadingDots() {
    // If this row is a constant integer.
    if (isConstantInt())
      return 1;

    // Extract the most significant dot(MSD).
    MatrixDot *MSD = getDot(getWidth() - 1);

    // If the MSD is zero or one, then set the result as 1.
    if (MSD->isZero() || MSD->isOne())
      return 1;

    // Then traverse the row from MSD to least significant dot(LSD)
    // and these bits that same as the LeadingDot will be counted in.
    unsigned LeadingDots = 0;
    for (unsigned i = 0; i < getWidth(); ++i) {
      if (getDot(getWidth() - 1 - i)->isSameAs(MSD))
        ++LeadingDots;
      else
        break;
    }

    return LeadingDots;
  }

  unsigned getValidWidth() {
    unsigned Width = getWidth();
    unsigned LeadingZeros = countingLeadingZeros();

    return Width - LeadingZeros;
  }

  float getMaxArrivalTime() {
    float MaxArrivalTime = 0.0f;

    for (unsigned i = 0; i < getWidth(); ++i) {
      MatrixDot *Dot = getDot(i);
      MaxArrivalTime = std::max(MaxArrivalTime, Dot->getArrivalTime());
    }

    return MaxArrivalTime;
  }

  bool hasSameArrivalTime() {
    float MaxArrivalTime = getMaxArrivalTime();

    for (unsigned i = 0; i < getWidth(); ++i) {
      MatrixDot *Dot = getDot(i);
      float ArrivalTime = Dot->getArrivalTime();

      if (ArrivalTime != MaxArrivalTime && ArrivalTime != 0.0f)
        return false;
    }

    return true;
  }

  // Sort the dots in row according to the algorithm settings.
  static bool dotCompare(const MatrixDot *DotA, const MatrixDot *DotB) {
    unsigned DotAStage = DotA->getLevel();
    unsigned DotBStage = DotB->getLevel();
    float DotADelay = DotA->getArrivalTime();
    float DotBDelay = DotB->getArrivalTime();

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
};

// The DotMatrix
struct DotMatrix {
  // The name of DotMatrix
  std::string Name;
  // The rows of DotMatrix
  std::vector<MatrixRow *> Rows;

  DotMatrix()
    : Name(""), Rows(std::vector<MatrixRow *>()) {}
  DotMatrix(std::string Name)
    : Name(Name), Rows(std::vector<MatrixRow *>()) {}
  // Replicate constructor
  DotMatrix(DotMatrix *DM) {
    this->Name = DM->getName();
    
    for (unsigned i = 0; i < DM->getRowNum(); ++i) {
      MatrixRow *ReplicateRow = new MatrixRow(DM->getRow(i));
      this->addRow(ReplicateRow);
    }
  }

  std::string getName() const { return Name; }
  unsigned getRowNum() const { return Rows.size(); }
  bool isInerratic() const {
    unsigned RowNum = getRowNum();
    
    // If this is an empty DotMatrix, it is inerratic of course.
    if (RowNum == 0)
      return true;

    unsigned ExpectedColNum = Rows[0]->getWidth();
    for (unsigned i = 1; i < RowNum; ++i) {
      if (Rows[i]->getWidth() != ExpectedColNum)
        return false;
    }

    return true;
  }
  // To be noted that, only inerratic DotMatrix can call this method.
  unsigned getColNum() const {
    assert(isInerratic() && "Only inerratic DotMatrix has a fixed column number!");
    assert(Rows.size() && "Unexpected empty DotMatrix!");

    return Rows[0]->getWidth();
  }

  void addRow(MatrixRow *Row) {
    Rows.push_back(Row);
  }

  MatrixRow *getRow(unsigned RowIdx) {
    assert(RowIdx < getRowNum() && "Out of range!");

    return Rows[RowIdx];
  }

  std::vector<unsigned> getNonZeroDotNumList() {
    std::vector<unsigned> NZDotNumList;

    for (unsigned i = 0; i < getRowNum(); ++i) {
      MatrixRow *Row = getRow(i);

      // If there are no dot in current row.
      if (Row->getWidth() == 0) {
        NZDotNumList.push_back(0);
        continue;
      }

      unsigned NZDotNum = 0;
      for (unsigned j = 0; j < Row->getWidth(); ++j) {
        if (!Row->getDot(j)->isZero())
          ++NZDotNum;
      }

      NZDotNumList.push_back(NZDotNum);
    }

    return NZDotNumList;
  }
  std::vector<unsigned> getActiveNZDotNumList(unsigned Level) {
    std::vector<unsigned> ActiveNZDotNumList;

    for (unsigned i = 0; i < getRowNum(); ++i) {
      MatrixRow *Row = getRow(i);

      // If there are no dot in current row.
      if (Row->getWidth() == 0) {
        ActiveNZDotNumList.push_back(0);
        continue;
      }

      unsigned ActiveNZDotNum = 0;
      for (unsigned j = 0; j < Row->getWidth(); ++j) {
        MatrixDot *Dot = Row->getDot(j);
        if (!Dot->isZero() && Dot->getLevel() <= Level)
          ++ActiveNZDotNum;
      }

      ActiveNZDotNumList.push_back(ActiveNZDotNum);
    }

    return ActiveNZDotNumList;
  }
};

// Idx, arrival time, dot start point and valid width of row in MulDM.
typedef std::pair<unsigned,
  std::pair<float, std::pair<unsigned, unsigned> > > MulDMInfoTy;

// Eliminated dot number in current row, compress ratio and delay(negative form) of GPC.
typedef std::pair<std::pair<unsigned, float>, float> PerfType;

// The compress component
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
  float getCriticalDelay() { return CriticalDelay; }
  unsigned getArea() { return Area; }
  Type getType() const { return T; }
};

// Special GPCs built to sum extra 1'b1 without extra area cost.
class SpecialGPC : public CompressComponent {
private:
  unsigned ExtraOneRank;

public:
  // Default constructor
  SpecialGPC(std::string Name, std::vector<unsigned> InputDotNums,
             unsigned OutputDotNum, unsigned Area, float CriticalDelay,
             unsigned ExtraOneRank)
    : CompressComponent(GPCWithExtraOne, Name, InputDotNums,
      OutputDotNum, Area, CriticalDelay), ExtraOneRank(ExtraOneRank) {}

  unsigned getRankOfExtraOne() { return ExtraOneRank; }

  // Methods for support type inquiry through isa, cast and dyn_cast.
  static inline bool classof(const SpecialGPC *Component) { return true; }
  static inline bool classof(const CompressComponent *Component) {
    return Component->getType() == GPCWithExtraOne;
  }
};

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

  void generateHybridTreeForMOA(IntrinsicInst *MOA, raw_fd_ostream &Output,
                                raw_fd_ostream &CTOutput);
  void generateHybridTrees();

  void collectMOAOps(IntrinsicInst *MOA);
  void collectMOAs();

  MatrixDot *createNormalMatrixDot(std::string Name, float ArrivalTime, unsigned Level);

  MatrixDot *createMulMatrixDot(std::string Name, float ArrivalTime, unsigned Level,
                                std::vector<std::string> BoothCodingElements);
  MatrixRow *createMaskedDMRow(std::string Name, unsigned Width, float ArrivalTime,
                               unsigned Level, BitMask Mask);
  std::pair<DotMatrix *, DotMatrix *>
  createDotMatrix(std::vector<Value *> NormalOps,
                  std::vector<std::pair<Value *, Value *> > MulOps,
                  unsigned Width, std::string Name, raw_fd_ostream &Output,
                  raw_fd_ostream &CTOutput);
  DotMatrix *createMulDotMatrix(unsigned OpAWidth, unsigned OpBWidth,
                                unsigned Idx, float ArrivalTime, unsigned Width,
                                raw_fd_ostream &Output);

  DotMatrix *preComputing(DotMatrix *DM);
  DotMatrix *preHandling(DotMatrix *DM, DotMatrix *MulDM);

  void printPCBCC(std::vector<MatrixDot *> OpA, std::vector<MatrixDot *> OpB,
                  unsigned Idx, raw_fd_ostream &Output);

  DotMatrix *sumRowsByAdder(DotMatrix *DM, raw_fd_ostream &Output);
  DotMatrix *preComputingByCarryChain(DotMatrix *DM, DotMatrix *MulDM,
                                      raw_fd_ostream &Output);

  float getCritialPathDelay(DFGNode *Node);
  float getOperandArrivalTime(Value *Operand);

  DotMatrix *transportDotMatrix(DotMatrix *DM);

  DotMatrix *abandonZeroDots(DotMatrix *TDM);
  DotMatrix *sumOneDots(DotMatrix *DM);

  // Initial the GPCs.
  CompressComponent createAddChainComponent(std::string Name, unsigned OpNum,
                                            unsigned BitWidth, unsigned Area,
                                            float CriticalDelay);
  void initGPCs();
  void initAddChains();
  void initLibrary();

  std::vector<unsigned> calculateIHs(DotMatrix *TDM);

  DotMatrix *compressTDMUsingGPC(DotMatrix *TDM, unsigned GPCIdx, unsigned RowNo,
                                 unsigned Level, unsigned &TotalLevels,
                                 raw_fd_ostream &Output);

  unsigned getHighestPriorityGPC(DotMatrix *TDM, unsigned RowNo,
                                 unsigned Level, unsigned IH);

  DotMatrix *compressTDMInLevel(DotMatrix *TDM, unsigned IH, unsigned Level,
                                unsigned &TotalLevels, unsigned &TotalArea,
                                raw_fd_ostream &Output);
  void compressDotMatrix(DotMatrix *DM, raw_fd_ostream &Output);

  void hybridTreeCodegen(DotMatrix *DM, raw_fd_ostream &Output);

  // The function to output verilog and debug files.
  void printDotMatrixForDebug(DotMatrix *DM);

  void printGPCModule(raw_fd_ostream &Output);
  void printAddChainModule(unsigned OpNum, unsigned BitWidth, raw_fd_ostream &Output);
  void printCompressComponent(raw_fd_ostream &Output);
  void printComponentInstance(unsigned ComponentIdx,
                              std::vector<std::vector<MatrixDot *> > InputDots,
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

static bool sortInArrivalTime(std::pair<unsigned, float> RowA,
                              std::pair<unsigned, float> RowB) {
  return RowA.second < RowB.second;
}


static bool sortInPeformance(std::pair<unsigned, PerfType> GPCA,
                             std::pair<unsigned, PerfType> GPCB) {
  if (GPCA.second.first < GPCB.second.first)
    return true;
  else if (GPCA.second.first > GPCB.second.first)
    return false;
  else {
    if (GPCA.second.second < GPCB.second.second)
      return true;
    else
      return false;
  }
}

static bool sortInInfos(MulDMInfoTy InfoA, MulDMInfoTy InfoB) {
  if (InfoA.second.second.second > InfoB.second.second.second)
    return true;
  else {
    if (InfoA.second.first < InfoB.second.first)
      return true;
    else
      return false;
  }
}

std::string float2String(float FV) {
  assert(FV >= 0.0f && "Unexpected negative float value!");

  // Get the integer part.
  unsigned IntPart = std::floor(FV);
  std::string IntPartStr = utostr_32(IntPart);

  // Get the decimal part. To be noted that, we only reserve
  // two bit here.
  unsigned BitOne = std::floor(FV * 10 - IntPart * 10);
  std::string BitOneStr = utostr_32(BitOne);
  unsigned BitTwo = std::floor(FV * 100 - IntPart * 100 - BitOne * 10);
  std::string BitTwoStr = utostr_32(BitTwo);

  std::string Result = IntPartStr + "." + BitOneStr + BitTwoStr;
  return Result;
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

void SIRMOAOpt::hybridTreeCodegen(DotMatrix *DM, raw_fd_ostream &Output) {
  // Basic information of DotMatrix
  std::string MatrixName = DM->getName();
  unsigned RowNum = DM->getRowNum();
  unsigned ColNum = DM->getColNum();

  // Eliminate the KnownDots in DotMatrix by pre-computation.
  DM = preComputing(DM);

  printDotMatrixForDebug(DM);

  // Consider use ternary adder to sum some rows which have earlier arrival time.
  if (sumFirstRowsByAdder)
    DM = sumRowsByAdder(DM, Output);

  // Compress the dot matrix.
  compressDotMatrix(DM, Output);
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
  case llvm::DFGNode::Not:
    return 0.0f;

  case llvm::DFGNode::GlobalVal:
    return VFUs::WireDelay;

  case llvm::DFGNode::Add: {
    unsigned BitWidth = Node->getBitWidth();
    BitWidth = BitWidth < 64 ? BitWidth : 64;

    return LuaI::Get<VFUAddSub>()->lookupLatency(BitWidth);
  }
  case llvm::DFGNode::Mul: {
    unsigned BitWidth = Node->getBitWidth();
    BitWidth = BitWidth < 64 ? BitWidth : 64;

    return LuaI::Get<VFUMult>()->lookupLatency(BitWidth);
  }
  case llvm::DFGNode::Div: {
    unsigned BitWidth = Node->getBitWidth();
    BitWidth = BitWidth < 64 ? BitWidth : 64;

    return LuaI::Get<VFUDiv>()->lookupLatency(BitWidth);
  }

  case llvm::DFGNode::LShr:
  case llvm::DFGNode::AShr:
  case llvm::DFGNode::Shl: {
    unsigned BitWidth = Node->getBitWidth();
    BitWidth = BitWidth < 64 ? BitWidth : 64;

    return LuaI::Get<VFUShift>()->lookupLatency(BitWidth);
  }

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
  case llvm::DFGNode::LT: {
    unsigned BitWidth = Node->getBitWidth();
    BitWidth = BitWidth < 64 ? BitWidth : 64;

    return LuaI::Get<VFUGT_LT>()->lookupLatency(BitWidth);
  }
  case llvm::DFGNode::Eq:
  case llvm::DFGNode::NE: {
    unsigned BitWidth = Node->getBitWidth();
    BitWidth = BitWidth < 64 ? BitWidth : 64;

    return LuaI::Get<VFUEQ_NE>()->lookupLatency(BitWidth);
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
  // If the operand is a constant integer, the arrival time
  // is 0.0f of course.
  if (isa<ConstantInt>(Operand))
    return 0.0f;

  SM->indexKeepVal(Operand);

  // Get the corresponding DFG node.
  DFGNode *Node = SM->getDFGNodeOfVal(Operand);

  /// Arrival time will be 0.0f if it is a register.
  if (Node->isSequentialNode())
    return VFUs::WireDelay;

  /// Otherwise, traverse the DFG to get the arrival time.
  // Arrival times from different source
  std::map<DFGNode *, float> ArrivalTimes;

  typedef DFGNode::iterator node_iterator;
  std::vector<std::pair<DFGNode *, node_iterator> > VisitStack;
  VisitStack.push_back(std::make_pair(Node, Node->parent_begin()));

  // Initialize a arrival time.
  float ArrivalTime = getCritialPathDelay(Node);

  SM->indexKeepVal(Node->getValue());
  if (Node->getType() == DFGNode::LogicOperationChain) {

    std::vector<DFGNode *> Operations = DFG->getOperations(Node);

    for (unsigned i = 0; i < Operations.size(); ++i) {
      DFGNode *Operation = Operations[i];

      SM->indexKeepVal(Operation->getValue());
    }
  }

  while (!VisitStack.empty()) {
    DFGNode *CurNode = VisitStack.back().first;
    node_iterator &It = VisitStack.back().second;

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
    if (ParentNode->getType() == DFGNode::LogicOperationChain) {
      std::vector<DFGNode *> Operations = DFG->getOperations(ParentNode);

      for (unsigned i = 0; i < Operations.size(); ++i) {
        DFGNode *Operation = Operations[i];

        SM->indexKeepVal(Operation->getValue());
      }
    }

    // If we reach the sequential node, then record the arrival time.
    if (ParentNode->isSequentialNode()) {
      // Remember to sum up the latency of the sequential node.
      float FinalArrivalTime = ArrivalTime + getCritialPathDelay(ParentNode);

      if (ArrivalTimes.count(ParentNode))
        ArrivalTimes[ParentNode] = ArrivalTimes[ParentNode] > FinalArrivalTime ?
                                     ArrivalTimes[ParentNode] : FinalArrivalTime;
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
    CritialPathArrivalTime
      = CritialPathArrivalTime > AI->second ? CritialPathArrivalTime : AI->second;

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

MatrixRow *SIRMOAOpt::createMaskedDMRow(std::string Name, unsigned Width,
                                        float ArrivalTime, unsigned Level,
                                        BitMask Mask) {
  MatrixRow *Row = new MatrixRow();

  // Used to denote the sign bit of the operand if it exists.
  std::string SameBit;
  for (unsigned i = 0; i < Width; ++i) {
    // When the dot position is within the range of operand bit width,
    // we get the name of dot considering the bit mask.
    if (i < Mask.getMaskWidth()) {
      // If it is a known bit, then use the known value.
      if (Mask.isOneKnownAt(i)) {
        MatrixDot *OneDot = createNormalMatrixDot("1'b1", 0.0f, 0);
        Row->addDot(OneDot);
        continue;
      }
      else if (Mask.isZeroKnownAt(i)) {
        MatrixDot *ZeroDot = createNormalMatrixDot("1'b0", 0.0f, 0);
        Row->addDot(ZeroDot);
        continue;
      }
      else if (Mask.isSameKnownAt(i)) {
        if (SameBit.size() != 0) {
          MatrixDot *SameDot = createNormalMatrixDot(SameBit, ArrivalTime, Level);
          Row->addDot(SameDot);
        }
        else {
          SameBit = Mangle(Name) + "[" + utostr_32(i) + "]";
          MatrixDot *SameDot = createNormalMatrixDot(SameBit, ArrivalTime, Level);
          Row->addDot(SameDot);
        }

        continue;
      }
      else {
        // Or use the form like operand[0], operand[1]...
        std::string DotName = Mangle(Name) + "[" + utostr_32(i) + "]";
        MatrixDot *Dot = createNormalMatrixDot(DotName, ArrivalTime, Level);
        Row->addDot(Dot);
      }
    }
    // When the dot position is beyond the range of operand bit width,
    // we need to pad zero into the matrix.
    else {
      MatrixDot *ZeroDot = createNormalMatrixDot("1'b0", 0.0f, 0);
      Row->addDot(ZeroDot);
    }
  }

  return Row;
}

DotMatrix *SIRMOAOpt::createMulDotMatrix(unsigned OpAWidth, unsigned OpBWidth,
                                         unsigned Idx, float ArrivalTime, unsigned Width,
                                         raw_fd_ostream &Output) {
  DotMatrix *MulMatrix = new DotMatrix();
  std::string OpAName = "MulOp_" + utostr_32(Idx) + "_a";
  std::string OpBName = "MulOp_" + utostr_32(Idx) + "_b";

  // Decide which operand is multiplicand and which is multiplier.
  unsigned MultiplicandBW = (OpAWidth > OpBWidth) ? OpBWidth : OpAWidth;
  unsigned MultiplierBW = (OpAWidth > OpBWidth) ? OpAWidth : OpBWidth;
  std::string MultiplicandName = (OpAWidth > OpBWidth) ? OpBName : OpAName;
  std::string MultiplierName = (OpAWidth > OpBWidth) ? OpAName : OpBName;

  // Calculate the height of the mul matrix.
  unsigned RowNum = std::floor(MultiplierBW / 2.0f) + 1;

  // Generate the implementation of sign extension bits.
  for (unsigned i = 0; i < RowNum; ++i) {
    if (2 * i + 1 < MultiplicandBW) {
      Output << "wire s" + utostr_32(i) << " = ";
      Output << MultiplierName + "[" << utostr_32(2 * i + 1) << "];\n";
    }    
  }
  Output << "\n";

  // Generate the partial products.
  for (unsigned i = 0; i < RowNum; ++i) {
    MatrixRow *Row = new MatrixRow();

    // The shift bit number for each partial product
    unsigned ShiftBitNum = i * 2;

    for (unsigned j = 0; j < Width; ++j) {
      // The shift bits should be zero.
      if (j < ShiftBitNum)
        Row->addZeroDot();

      // The +2, +1, 0, -1, -2 multiplicand.
      else if (j < ShiftBitNum + MultiplicandBW + 1) {
        std::string MulDotName = "pp_" + utostr_32(i) + "_" + utostr_32(j);
        unsigned MulDotLevel = 0;

        std::vector<std::string> BoothCodingElements;
        // Booth coding bit #1
        int FirstBitIdx = 2 * i - 1;
        if (FirstBitIdx < 0) {
          BoothCodingElements.push_back("1'b0");
        }
        else {
          std::string FirstEleName
            = MultiplierName + "[" + utostr_32(FirstBitIdx) + "]";
          BoothCodingElements.push_back(FirstEleName);
        }
        // Booth coding bit #2
        int SecondBitIdx = 2 * i;
        if (SecondBitIdx < MultiplierBW) {
          std::string SecondEleName
            = MultiplierName + "[" + utostr_32(SecondBitIdx) + "]";
          BoothCodingElements.push_back(SecondEleName);
        }
        else {
          BoothCodingElements.push_back("1'b0");
        }
        // Booth coding bit #3
        int ThirdBitIdx = 2 * i + 1;
        if (ThirdBitIdx < MultiplierBW) {
          std::string ThirdEleName
            = MultiplierName + "[" + utostr_32(ThirdBitIdx) + "]";
          BoothCodingElements.push_back(ThirdEleName);
        }
        else {
          BoothCodingElements.push_back("1'b0");
        }
        // Booth coding bit #4 & #5
        int FFBitIdx = j - ShiftBitNum;
        if (FFBitIdx < MultiplicandBW) {
          std::string ForthEleName
            = MultiplicandName + "[" + utostr_32(FFBitIdx) + "]";
          BoothCodingElements.push_back(ForthEleName);
        }
        else {
          BoothCodingElements.push_back("1'b0");
        }        
        if (FFBitIdx - 1 < 0) {
          BoothCodingElements.push_back("1'b0");
        }
        else {
          std::string FifthEleName
            = MultiplicandName + "[" + utostr_32(FFBitIdx - 1) + "]";
          BoothCodingElements.push_back(FifthEleName);
        }

        // Create the mul partial product dot.
        MatrixDot *MulDot
          = createMulMatrixDot(MulDotName, ArrivalTime, MulDotLevel, BoothCodingElements);

        Row->addDot(MulDot);
      }

      // The sign extension bits
      else {
        std::string SignBitName = "s" + utostr_32(i);
        MatrixDot *Dot = createNormalMatrixDot(SignBitName, 0.0f, 0);

        Row->addDot(Dot);
      }
    }

    MulMatrix->addRow(Row);
  }

  // Remember to add a row to patch the possible 1'b1 in two's complement.
  MatrixRow *AdditionRow = new MatrixRow();
  unsigned UpperBound = 2 * RowNum - 1;
  for (unsigned i = 0; i < Width; ++i) {
    if (i < UpperBound) {
      if (i % 2 == 0) {
        unsigned AdditionIdx = i / 2;
        std::string AdditionBitName = "s" + utostr_32(AdditionIdx);
        MatrixDot *Dot = createNormalMatrixDot(AdditionBitName, ArrivalTime, 0);

        AdditionRow->addDot(Dot);
        continue;
      }
    }

    MatrixDot *ZeroDot = createNormalMatrixDot("1'b0", 0.0f, 0);
    AdditionRow->addDot(ZeroDot);
  }
  MulMatrix->addRow(AdditionRow);

  return MulMatrix;
}

// This function is used to create matrix which can be synthesized into compressor tree
// for experiment purpose. The synthesized compressor tree has nothing to do with the
// HLS target C-program.
// MatrixType createExperimentDotMatrix() {
//   const unsigned RowNum = 8;
//   const unsigned ColNum = 16;
// 
//   int Masks[RowNum][ColNum] =
//   { { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 1, 0 },
//     { 2, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0 },
//     { 0, 0, 2, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0 },
//     { 0, 0, 0, 0, 2, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
//     { 0, 0, 0, 0, 0, 0, 2, 0, 2, 2, 2, 2, 2, 2, 2, 2 },
//     { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
//     { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
//     { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 }
// //     { 2, 0, 2, 0, 2, 0, 2, 0, 0, 2, 0, 0, 0, 0, 0, 0 }
// //     { 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0 },
// //     { 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 0, 0 },
// //     { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2 }
//    };
// 
//   MatrixType Matrix;
//   for (unsigned i = 0; i < RowNum; ++i) {
//     MatrixRowType Row;
// 
//     for (unsigned j = 0; j < ColNum; ++j) {
//       std::string Name;
//       if (Masks[i][j] == 0)
//         Name = "1'b0";
//       else if (Masks[i][j] == 1)
//         Name = "1'b1";
//       else
//         Name = "operand_" + utostr_32(i) + "[" + utostr_32(j) + "]";
// 
//       MatrixDot *Dot = new MatrixDot(Name, 0.0f, 0, false, std::vector<std::string>());
//       Row.push_back(Dot);
//     }
// 
//     Matrix.push_back(Row);
//   }
// 
//   return Matrix;
// }

MatrixDot *SIRMOAOpt::createNormalMatrixDot(std::string Name, float ArrivalTime,
                                           unsigned Level) {
  MatrixDot *Dot = new MatrixDot(Name, ArrivalTime, Level, false, std::vector<std::string>());

  return Dot;
}

MatrixDot *SIRMOAOpt::createMulMatrixDot(std::string Name, float ArrivalTime,
                                         unsigned Level,
                                         std::vector<std::string> BoothCodingElements) {
  MatrixDot *MulDot
    = new MatrixDot(Name, ArrivalTime, Level, true, BoothCodingElements);

  return MulDot;
}

std::pair<DotMatrix *, DotMatrix *>
SIRMOAOpt::createDotMatrix(std::vector<Value *> NormalOps,
                           std::vector<std::pair<Value *, Value *> > MulOps,
                           unsigned Width, std::string Name,
                           raw_fd_ostream &Output, raw_fd_ostream &CTOutput) {
  unsigned NormalOpNum = NormalOps.size();
  unsigned MulOpNum = MulOps.size();

  // Print the declaration of the module.  
  Output << "module " << "compressor_" << Name << "(\n";
  for (unsigned i = 0; i < NormalOpNum; ++i) {
    Value *NormalOp = NormalOps[i];
    std::string NormalOpName = "Op_" + utostr_32(i);
    unsigned NormalOpWidth = TD->getTypeSizeInBits(NormalOp->getType());

    Output << "\tinput wire[" << utostr_32(NormalOpWidth - 1) << ":0] "
           << NormalOpName << ", \n";
  }
  for (unsigned i = 0; i < MulOpNum; ++i) {
    std::pair<Value *, Value *> MulOpPair = MulOps[i];
    std::string MulOpName = "MulOp_" + utostr_32(i);
    unsigned MulOpAWidth = TD->getTypeSizeInBits(MulOpPair.first->getType());
    unsigned MulOpBWidth = TD->getTypeSizeInBits(MulOpPair.second->getType());

    Output << "\tinput wire[" << utostr_32(MulOpAWidth - 1) << ":0] "
           << MulOpName + "_a" << ", \n";
    Output << "\tinput wire[" << utostr_32(MulOpBWidth - 1) << ":0] "
           << MulOpName + "_b" << ", \n";
  }
  Output << "\toutput wire[";
  Output << utostr_32(Width - 1) << ":0] result\n);\n\n";

  DotMatrix *DM = new DotMatrix(Name);
  DotMatrix *TopMulDM = new DotMatrix(Name + "_MulDM");

  // Create the MulDM for MulOps.
  for (unsigned i = 0; i < MulOpNum; ++i) {
    std::pair<Value *, Value *> MulOpPair = MulOps[i];
    unsigned MulOpAWidth = TD->getTypeSizeInBits(MulOpPair.first->getType());
    unsigned MulOpBWidth = TD->getTypeSizeInBits(MulOpPair.second->getType());

    float MulOpA_AT = getOperandArrivalTime(MulOpPair.first) * VFUs::Period;
    float MulOpB_AT = getOperandArrivalTime(MulOpPair.second) * VFUs::Period;
    float ArrivalTime = std::max(MulOpA_AT, MulOpB_AT);

    DotMatrix *MulDM
      = createMulDotMatrix(MulOpAWidth, MulOpBWidth, i, ArrivalTime, Width, Output);
    MulDM = preHandling(DM, MulDM);

    // Merge each the MulDM into the TopMulDM.
    for (unsigned i = 0; i < MulDM->getRowNum(); ++i)
      TopMulDM->addRow(MulDM->getRow(i));
  }

  // Sort the normal operands in ascending order of arrival time.
  std::vector<std::pair<unsigned, float> > SortedOps;
  for (unsigned i = 0; i < NormalOps.size(); ++i) {
    Value *NormalOp = NormalOps[i];
    float ArrivalTime = getOperandArrivalTime(NormalOp) * VFUs::Period;

    SortedOps.push_back(std::make_pair(i, ArrivalTime));
  }
  std::sort(SortedOps.begin(), SortedOps.end(), sortInArrivalTime);

  for (unsigned i = 0; i < SortedOps.size(); ++i) {
    unsigned NormalOpIdx = SortedOps[i].first;
    Value *NormalOp = NormalOps[NormalOpIdx];
    float ArrivalTime = SortedOps[i].second;
    unsigned OpWidth = TD->getTypeSizeInBits(NormalOp->getType());

    /// Create MatrixRow for normal operands.
    std::string RowName = "operand_" + utostr_32(NormalOpIdx);
    MatrixRow *Row = new MatrixRow(RowName);
    // If the operand is a constant integer, then the dots will be
    // its binary representation.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(NormalOp)) {
      APInt CIV = CI->getValue();

      for (unsigned j = 0; j < Width; ++j) {
        if (j < OpWidth) {
          APInt LShiftedCIV = CIV.shl(OpWidth - 1 - j);
          APInt RLShiftedCIV = LShiftedCIV.lshr(OpWidth - 1);

          unsigned BitVal = RLShiftedCIV.getZExtValue();
          if (BitVal)
            Row->addOneDot();
          else
            Row->addZeroDot();
        }
        else
          Row->addZeroDot();
      }

      DM->addRow(Row);
    }
    // Or the dots will be the form like operand[0], operand[1]...
    else {
      // Get the bit mask of the dots.
      DFGNode *OpNode = SM->getDFGNodeOfVal(NormalOp);
      assert(OpNode && "DFG node not created?");
      BitMask Mask = SM->getBitMask(OpNode);

      errs() << "Operand_" + utostr_32(i) << ": ArrivalTime[ "
             << float2String(ArrivalTime) << "], ";
      errs() << "BitMask[";
      Mask.print(errs());
      errs() << "];\n";

      // Used to denote the sign bit of the operand if it exists.
      std::string SameBit;
      for (unsigned j = 0; j < Width; ++j) {
        // When the dot position is within the range of operand bit width,
        // we get the name of dot considering the bit mask.
        if (j < OpWidth) {
          // If we enable the optimization based on bitmask analysis. Then
          // the content of dot should be decided by mask.
          if (enableBitMaskOpt) {
            // If it is a known bit, then use the known value.
            if (Mask.isOneKnownAt(j)) {
              Row->addOneDot();
              continue;
            }
            else if (Mask.isZeroKnownAt(j)) {
              Row->addZeroDot();
              continue;
            }
            else if (Mask.isSameKnownAt(j)) {
              if (SameBit.size() != 0) {
                MatrixDot *Dot = createNormalMatrixDot(SameBit, ArrivalTime, 0);
                Row->addDot(Dot);
              }                
              else {
                SameBit = Mangle(RowName) + "[" + utostr_32(j) + "]";
                MatrixDot *Dot = createNormalMatrixDot(SameBit, ArrivalTime, 0);
                Row->addDot(Dot);
              }
              continue;
            }
          }

          // Or use the form like operand[0], operand[1]...
          std::string DotName = Mangle(RowName) + "[" + utostr_32(j) + "]";
          MatrixDot *Dot = createNormalMatrixDot(DotName, ArrivalTime, 0);

          Row->addDot(Dot);
        }
        // When the dot position is beyond the range of operand bit width,
        // we need to pad zero into the matrix.
        else
          Row->addZeroDot();
      }

      DM->addRow(Row);
    }
  }

  // Output all the bitmask information of operands for debug.
  for (unsigned i = 0; i < NormalOps.size(); ++i) {
    Value *Operand = NormalOps[i];
    unsigned Width = TD->getTypeSizeInBits(Operand->getType());
    std::string OperandName = "operand_" + utostr_32(i);

    if (ConstantInt *CI = dyn_cast<ConstantInt>(Operand)) {
      CTOutput << ";\n";
      continue;
    }
    else {
      DFGNode *OpNode = SM->getDFGNodeOfVal(Operand);
      assert(OpNode && "DFG node not created?");
      BitMask Mask = SM->getBitMask(OpNode);

      unsigned LeadingSigns = Mask.countLeadingSigns();
      if (LeadingSigns == Width) {
        CTOutput << "{" << LeadingSigns << "{" << OperandName << "[" + utostr_32(Width - 1) + "]" << "}}";
      }
      else if (LeadingSigns != 0 && LeadingSigns != Width) {
        CTOutput << "{" << LeadingSigns << "{" << OperandName << "[" + utostr_32(Width - 1) + "]" << "}";
        CTOutput << ", " << OperandName << "[" + utostr_32(Width - 1 - LeadingSigns) + ":0]}";
      }
      else
        CTOutput << OperandName;

      CTOutput << " & ";

      CTOutput << utostr_32(Width) << "\'b";
      for (unsigned j = 0; j < Width; ++j) {
        if (Mask.isZeroKnownAt(Width - 1 - j)) {
          CTOutput << "0";
        }
        else
          CTOutput << "1";
      }

      CTOutput << " | ";

      CTOutput << utostr_32(Width) << "\'b";
      for (unsigned j = 0; j < Width; ++j) {
        if (Mask.isOneKnownAt(Width - 1 - j)) {
          CTOutput << "1";
        }
        else
          CTOutput << "0";
      }
    }

    CTOutput << ";\n";
  }
  CTOutput << "\n\n";

  return std::make_pair(DM, TopMulDM);
}

void SIRMOAOpt::generateHybridTreeForMOA(IntrinsicInst *MOA,
                                         raw_fd_ostream &Output,
                                         raw_fd_ostream &CTOutput) {
  unsigned Width = TD->getTypeSizeInBits(MOA->getType());
  std::vector<Value *> Operands = MOA2Ops[MOA];

  // Eliminate the identical operands in add chain.
  Operands = eliminateIdenticalOperands(Operands, MOA, Width);

  std::vector<Value *> NormalOps;
  std::vector<std::pair<Value *, Value *> > MulOps;
  // Extract out the multiplication operands.
  for (unsigned i = 0; i < Operands.size(); ++i) {
    Value *Operand = Operands[i];

    if (IntrinsicInst *OpII = dyn_cast<IntrinsicInst>(Operand)) {
      if (OpII->getIntrinsicID() == Intrinsic::shang_mul) {
        Value *OperandA = OpII->getOperand(0);
        Value *OperandB = OpII->getOperand(1);

        MulOps.push_back(std::make_pair(OperandA, OperandB));
        continue;
      }
    }

    NormalOps.push_back(Operand);
  }

  // Index the connection of operands and pseudo hybrid tree instruction.
  // So we can generate the instance of the hybrid tree module.
  IntrinsicInst *PseudoHybridTreeInst = MOA2PseudoHybridTreeInst[MOA];
  SM->IndexOps2AdderChain(PseudoHybridTreeInst, NormalOps);

  std::string MatrixName = Mangle(PseudoHybridTreeInst->getName());
  unsigned MatrixWidth = TD->getTypeSizeInBits(MOA->getType());

  errs() << "Synthesize compressor tree for " << MatrixName << "......\n";

  std::pair<DotMatrix *, DotMatrix *> DMPair
    = createDotMatrix(NormalOps, MulOps, MatrixWidth, MatrixName, Output, CTOutput);

  DotMatrix *DM = preComputingByCarryChain(DMPair.first, DMPair.second, Output);

  DebugOutput << "---------- Matrix for " << MatrixName << " ------------\n";
  printDotMatrixForDebug(DM);

  hybridTreeCodegen(DM, Output);
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

  // Generate bitmask top module for compressor tree module.
  std::string CompressorBitMaskPath = LuaI::GetString("CompressorBitMask");
  std::string CTError;
  raw_fd_ostream CTOutput(CompressorBitMaskPath.c_str(), CTError);

  // Generate hybrid tree for each multi-operand adder.
  for (iterator I = MOAs.begin(), E = MOAs.end(); I != E; ++I) {
    generateHybridTreeForMOA(*I, Output, CTOutput);
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

        if (IntrinsicInst *OperandInst = dyn_cast<IntrinsicInst>(Operand)) {
          // Ignore the adder instruction itself.
          if (AdderSet.count(OperandInst))
            continue;

          // Identify the MAC.
          if (OperandInst->getIntrinsicID() == Intrinsic::shang_mul)
            errs() << "MAC found!\n";
        }

        Operands.push_back(Operand);
      }
    }

    // Index the connection between MOA and its operands.
    MOA2Ops.insert(std::make_pair(MOA, Operands));
  }
}

DotMatrix *SIRMOAOpt::preHandling(DotMatrix *DM, DotMatrix *MulDM) {
  // Sum all the LeadingDots in each row.
  MulDM = preComputing(MulDM);

  /// Divide the MulDM into two matrix, 1) first matrix to contain
  /// the normal dots like 1'b1 and s?; 2) second matrix to contain
  /// all the MulDots.

  // Get the first part matrix.
  DotMatrix *FistPartMatrix = new DotMatrix(MulDM);
  for (unsigned i = 0; i < FistPartMatrix->getRowNum(); ++i) {
    MatrixRow *Row = FistPartMatrix->getRow(i);

    for (unsigned j = 0; j < Row->getWidth(); ++j) {
      MatrixDot *Dot = Row->getDot(j);

      if (Dot->isMulDot())
        Dot->setToZero();
    }
  }

  // Get the second part matrix.
  DotMatrix *SecondPartMatrix = new DotMatrix(MulDM);
  for (unsigned i = 0; i < SecondPartMatrix->getRowNum(); ++i) {
    MatrixRow *Row = SecondPartMatrix->getRow(i);

    for (unsigned j = 0; j < Row->getWidth(); ++j) {
      MatrixDot *Dot = Row->getDot(j);

      if (!Dot->isMulDot())
        Dot->setToZero();
    }
  }

  /// Since all the dots in second part matrix have same arrival time, so the dots
  /// with same rank in different row are actually the same. To utilized the carry
  /// chain as much as possible, we need to transform the MulDM from the origin shape:
  /// ----00000000ssssssssss
  /// ----000000ssssssssss00
  /// ----0000ssssssssss0000
  /// ----00ssssssssss000000
  /// ----ssssssssss00000000
  /// into the new shape:
  /// ----ssssssssssssssssss
  /// ----00ssssssssssssss00
  /// ----0000ssssssssss0000
  /// ----000000ssssss000000
  /// ----00000000ss00000000

  unsigned RowNum = SecondPartMatrix->getRowNum();
  unsigned ColNum = SecondPartMatrix->getColNum();

  // First we transport the DotMatrix and abandon the ZeroDots.
  SecondPartMatrix = transportDotMatrix(SecondPartMatrix);
  SecondPartMatrix = abandonZeroDots(SecondPartMatrix);

  // Then we padding each row in MulTDM with ZeroDots so that
  // the TDM can be transport back into DM.
  for (unsigned i = 0; i < SecondPartMatrix->getRowNum(); ++i) {
    MatrixRow *Row = SecondPartMatrix->getRow(i);
    unsigned RowValidWidth = Row->getValidWidth();

    unsigned PaddingZeroNum = RowNum - RowValidWidth;
    for (unsigned j = 0; j < PaddingZeroNum; ++j)
      Row->addZeroDot();
  }

  // Transport the MulTDM back to MulDM.
  SecondPartMatrix = transportDotMatrix(SecondPartMatrix);

  // Eliminate the empty rows.
  DotMatrix *SimplifedSPM = new DotMatrix();
  for (unsigned i = 0; i < SecondPartMatrix->getRowNum(); ++i) {
    MatrixRow *Row = SecondPartMatrix->getRow(i);

    bool isZeroRow = true;
    for (unsigned j = 0; j < Row->getWidth(); ++j) {
      MatrixDot *Dot = Row->getDot(j);

      if (!Dot->isZero()) {
        isZeroRow = false;
        break;
      }
    }

    if (!isZeroRow)
      SimplifedSPM->addRow(Row);
  }
  
  // The non-zero rows of first part matrix will be merged into MOA matrix.
  for (unsigned i = 0; i < FistPartMatrix->getRowNum(); ++i) {
    MatrixRow *Row = FistPartMatrix->getRow(i);

    bool isZeroRow = true;
    for (unsigned j = 0; j < Row->getWidth(); ++j) {
      MatrixDot *Dot = Row->getDot(j);

      if (!Dot->isZero()) {
        isZeroRow = false;
        break;
      }
    }

    if (!isZeroRow)
      DM->addRow(Row);
  }   

  return SimplifedSPM;
}

DotMatrix *SIRMOAOpt::preComputing(DotMatrix *DM) {
  /// First, transform all LeadingDots in DotMatrix using equation:
  /// ssssssss = 11111111 + 0000000~s,
  /// then we create a new DotMatrix to contain all the 11111111,
  /// after we sum all these OneDots, the sum result is inserted
  /// back to the origin DotMatrix.

  // Get the number of LeadingDots in each row of DotMatrix.
  std::vector<unsigned> LDNumList;
  for (unsigned i = 0; i < DM->getRowNum(); ++i) {
    MatrixRow *MR = DM->getRow(i);

    LDNumList.push_back(MR->countingLeadingDots());
  }

  // Transform the LeadingDots and create the LDDM to contain all the
  // 111111111. And don't forget the invert the s-dot in origin DotMatrix
  // in transformation.
  DotMatrix *LDDM = new DotMatrix(DM);
  for (unsigned i = 0; i < DM->getRowNum(); ++i) {
    // If there is sign bit pattern in current row.
    if (LDNumList[i] > 1) {
      unsigned LeadingDotStartPoint = DM->getRow(i)->getWidth() - LDNumList[i];

      // Set the ssssssss to 0000000~s in origin Matrix.
      MatrixDot *OriginDot = DM->getRow(i)->getDot(LeadingDotStartPoint);
      OriginDot->setName("~" + OriginDot->getName());
      for (unsigned j = LeadingDotStartPoint + 1; j < DM->getRow(i)->getWidth(); ++j)
        DM->getRow(i)->getDot(j)->setToZero();

      // Set the non-sign bit to 00000000 in sign bit Matrix.
      for (unsigned j = 0; j < LeadingDotStartPoint; ++j)
        LDDM->getRow(i)->getDot(j)->setToZero();

      // Set the ssssssss to 11111111 in sign bit Matrix.
      for (unsigned j = LeadingDotStartPoint; j < LDDM->getRow(i)->getWidth(); ++j)
        LDDM->getRow(i)->getDot(j)->setToOne();
    }
    // If there is no sign bit pattern in current row.
    else {
      // Set all bits to 00000000 in sign bit Matrix.
      for (unsigned j = 0; j < LDDM->getRow(i)->getWidth(); ++j)
        LDDM->getRow(i)->getDot(j)->setToZero();
    }
  }

  // Sum all the OneDots in LDDM and the sum result will be in the first row.
  LDDM = sumOneDots(LDDM);
  MatrixRow *SumResultRow = LDDM->getRow(0);

  // Insert the sum result back to the origin DotMatrix
  DM->addRow(SumResultRow);

  // Then sum all the OneDots in origin DotMatrix.
  DM = sumOneDots(DM);

  return DM;
}

DotMatrix *SIRMOAOpt::sumOneDots(DotMatrix *DM) {
  // Transport the DotMatrix first.
  DotMatrix *TDM = transportDotMatrix(DM);

  // Get the number of LeadingDots in each row of TDM.
  std::vector<unsigned> ODNumList;
  for (unsigned i = 0; i < TDM->getRowNum(); ++i) {
    MatrixRow *TRow = TDM->getRow(i);

    unsigned OneDotNum = 0;
    for (unsigned j = 0; j < TRow->getWidth(); ++j) {
      if (TRow->getDot(j)->isOne())
        ++OneDotNum;
    }

    ODNumList.push_back(OneDotNum);
  }

  // The number of OneDots in each row of TDM after sum.
  std::vector<unsigned> ODNumListAfterSum;
  for (unsigned i = 0; i < ODNumList.size(); ++i) {
    unsigned ODNum = ODNumList[i];

    ODNumListAfterSum.push_back(ODNum % 2);
    unsigned CarryOneNum = ODNum / 2;

    if (i != ODNumList.size() - 1)
      ODNumList[i + 1] += CarryOneNum;
  }

  // Create a row to represent the sum result.
  MatrixRow *SumResult = new MatrixRow();
  for (unsigned i = 0; i < ODNumListAfterSum.size(); ++i) {
    MatrixDot *SumResultDot;
    if (ODNumListAfterSum[i] == 0)
      SumResultDot = createNormalMatrixDot("1'b0", 0.0f, 0);
    else
      SumResultDot = createNormalMatrixDot("1'b1", 0.0f, 0);
    SumResult->addDot(SumResultDot);
  }

  DotMatrix *ResultDM = new DotMatrix(DM->getName());
  ResultDM->addRow(SumResult);
  for (unsigned i = 0; i < DM->getRowNum(); ++i) {
    MatrixRow *Row = DM->getRow(i);
    MatrixRow *ResultRow = new MatrixRow(Row->getName());

    for (unsigned j = 0; j < Row->getWidth(); ++j) {
      MatrixDot *Dot = Row->getDot(j);

      if (!Dot->isOne())
        ResultRow->addDot(Dot);
      else
        ResultRow->addZeroDot();
    }

    ResultDM->addRow(ResultRow);
  }

  return ResultDM;
}

DotMatrix *SIRMOAOpt::sumRowsByAdder(DotMatrix *DM, raw_fd_ostream &Output) {
  // We expect the DotMatrix are inerratic here.
  assert(DM->isInerratic() && "Unexpected erratic DotMatrix!");

  /// First we should check each row by its arrival time to identify which
  /// rows can be summed by adder without increasing critical path delay of
  /// compressor tree.

  // Collect information of each row, including name, index,
  // arrival time & valid width.
  std::vector<std::pair<unsigned, float> > SortedRows;
  for (unsigned i = 0; i < DM->getRowNum(); ++i) {
    MatrixRow *Row = DM->getRow(i);

    // Ignore the constant int row.
    if (Row->isConstantInt())
      continue;

    // We expect the dots in the row have same arrival time here.
    assert(Row->hasSameArrivalTime() && "Unexpected different arrival time!");

    // Collect the arrival time and valid width information.
    float ArrivalTime = Row->getMaxArrivalTime();
    assert(ArrivalTime != 0.0f && "Unexpected arrival time!");

    SortedRows.push_back(std::make_pair(i, ArrivalTime));
  }
  std::sort(SortedRows.begin(), SortedRows.end(), sortInArrivalTime);

  // Identify which rows can be summed by adder.
  unsigned ColNum = DM->getColNum();
  float TimeLimit = SortedRows.back().second/* + VFUs::WireDelay * VFUs::Period*/;
  std::vector<std::pair<std::vector<unsigned>, float> > TernaryAdders;
  unsigned AdderIdx = 0;
  bool Continue = true;
  while (Continue && SortedRows.size() > 3) {
    Continue = false;

    // Get the max arrival time and valid width of first triple row after sorting.
    float MaxArrivalTime = 0.0f;
    unsigned MaxValidWidth = 0;
    for (unsigned i = 0; i < 3; ++i) {
      unsigned RowIdx = SortedRows[i].first;
      MatrixRow *Row = DM->getRow(RowIdx);

      float ArrivalTime = Row->getMaxArrivalTime();
      unsigned ValidWidth = Row->getValidWidth();

      MaxArrivalTime = std::max(MaxArrivalTime, ArrivalTime);
      MaxValidWidth = std::max(MaxValidWidth, ValidWidth);
    }

    // Predict the arrival time of result if a adder is used.
    float TernaryAddDelay
      = LuaI::Get<VFUTernaryAdd>()->lookupLatency(MaxValidWidth) * VFUs::Period;
    float ResultAT = MaxArrivalTime + TernaryAddDelay;

    if (ResultAT < TimeLimit) {
      // The name of adder.
      std::string AdderName = "Adder_" + utostr_32(AdderIdx);

      // The implementation of operands.
      for (unsigned i = 0; i < 3; ++i) {
        Output << "wire[" << utostr_32(ColNum - 1) << ":0] " + AdderName
               << "_Op_" + utostr_32(i) << " = {";

        unsigned RowIdx = SortedRows[i].first;
        for (unsigned j = 0; j < ColNum; ++j) {
          Output << DM->getRow(RowIdx)->getDot(ColNum - 1 - j)->getName();

          if (j != ColNum - 1)
            Output << ", ";
          else
            Output << "};\n";
        }
      }

      // The implementation of current adder.
      Output << "wire[" << utostr_32(ColNum - 1) << ":0] " + AdderName + " = ";
      for (unsigned i = 0; i < 3; ++i) {
        Output << "Adder_" + utostr_32(AdderIdx) + "_Op_" + utostr_32(i);

        if (i != 2)
          Output << " + ";
        else
          Output << ";\n\n";
      }

      // Debug code
      errs() << utostr_32(MaxValidWidth) << "-bit ternary adder sum: ";
      for (unsigned i = 0; i < 3; ++i) {
        unsigned RowIdx = SortedRows[i].first;
        MatrixRow *Row = DM->getRow(RowIdx);

        errs() << Row->getName();

        if (i != 2)
          errs() << ", ";
        else
          errs() << ";\n";
      }
     
      // Get the mask for each operand row of current adder.
      std::vector<BitMask> AdderOpMasks;
      for (unsigned i = 0; i < 3; ++i) {
        unsigned RowIdx = SortedRows[i].first;
        MatrixRow *Row = DM->getRow(RowIdx);

        // Initialize a empty mask.
        BitMask Mask = BitMask(ColNum);

        // Set each bit of mask according to the dot in row.
        for (unsigned j = 0; j < ColNum; ++j) {
          if (Row->getDot(j)->getName() == "1'b0")
            Mask.setKnownZeroAt(j);
          else if (Row->getDot(j)->getName() == "1'b1")
            Mask.setKnownOneAt(j);
        }

        AdderOpMasks.push_back(Mask);
      }

      // Calculate the mask of adder result.
      BitMask ResultMask
        = BitMaskAnalysis::computeAdd(AdderOpMasks[0], AdderOpMasks[1], ColNum);
      for (unsigned i = 2; i < AdderOpMasks.size(); ++i) {
        ResultMask = BitMaskAnalysis::computeAdd(AdderOpMasks[i], ResultMask, ColNum);
      }

      unsigned ResultValidWidth
        = ResultMask.getMaskWidth() - ResultMask.countLeadingZeros();

      MatrixRow *AdderResultRow
        = createMaskedDMRow(AdderName, ColNum, ResultAT, 0, ResultMask);

      // Insert the adder result back into matrix.
      DM->addRow(AdderResultRow);

      // Also insert the information of created row into RowInfos.
      SortedRows.push_back(std::make_pair(DM->getRowNum() - 1, ResultAT));

      // Clear the rows summed by adder in Matrix.
      for (unsigned i = 0; i < 3; ++i) {
        unsigned RowIdx = SortedRows[i].first;

        for (unsigned j = 0; j < ColNum; ++j)
          DM->getRow(RowIdx)->getDot(j)->setToZero();
      }

      // Also clear the row information in RowInfos.
      std::vector<std::pair<unsigned, float> > NewSortedRows;
      for (unsigned i = 3; i < SortedRows.size(); ++i) {
        NewSortedRows.push_back(SortedRows[i]);
      }
      SortedRows = NewSortedRows;

      // Sort the new rows in matrix and continue the identifying.
      std::sort(NewSortedRows.begin(), NewSortedRows.end(), sortInArrivalTime);
      Continue = true;
      AdderIdx++;
    }
  }

  // We also expect the result DotMatrix are inerratic here.
  assert(DM->isInerratic() && "Unexpected erratic DotMatrix!");

  return DM;
}

DotMatrix *SIRMOAOpt::preComputingByCarryChain(DotMatrix *DM, DotMatrix *MulDM,
                                               raw_fd_ostream &Output) {
  std::vector<MulDMInfoTy> MulDMInfos;
  for (unsigned i = 0; i < MulDM->getRowNum(); ++i) {
    MatrixRow *Row = MulDM->getRow(i);
    assert(Row->hasSameArrivalTime() && "Unexpected different arrival time!");

    float ArrivalTime = Row->getMaxArrivalTime();
    unsigned StartPoint = Row->countingTrailingZeros();
    unsigned ValidWidth = Row->getWidth() - Row->countingLeadingZeros() - StartPoint;

    MulDMInfoTy Info
      = std::make_pair(i, std::make_pair(ArrivalTime,
                                         std::make_pair(StartPoint, ValidWidth)));

    MulDMInfos.push_back(Info);
  }

  // Sort the rows in MulDM.
  std::sort(MulDMInfos.begin(), MulDMInfos.end(), sortInInfos);

  /// Pre-compute each row in MulDM by carry chain.

  // First transform the DM into TDM.
  DotMatrix *TDM = transportDotMatrix(DM);

  // Handle the MulDM row by row.
  for (unsigned i = 0; i < MulDMInfos.size(); ++i) {
    unsigned MulDMRowIdx = MulDMInfos[i].first;
    float TimeLimit = MulDMInfos[i].second.first;
    unsigned StartPoint = MulDMInfos[i].second.second.first;
    unsigned ValidWidth = MulDMInfos[i].second.second.second;

    MatrixRow *MulDMRow = MulDM->getRow(MulDMRowIdx);
    std::vector<MatrixDot *> PCBCCOpA;
    std::vector<MatrixDot *> PCBCCOpB;
    for (unsigned j = StartPoint; j < StartPoint + ValidWidth; ++j) {
      MatrixDot *MulDMDot = MulDMRow->getDot(j);

      // Search for available dot in TDM.
      MatrixRow *TDMRow = TDM->getRow(j);
      bool Continue = true;
      for (unsigned k = 0; k < TDMRow->getWidth(); ++k) {
        MatrixDot *TDMDot = TDMRow->getDot(k);

        // Ignore the ZeroDot.
        if (TDMDot->isZero())
          continue;

        float TDMDotAT = TDMDot->getArrivalTime();
        
        if (TDMDotAT <= TimeLimit) {
          PCBCCOpA.push_back(MulDMDot);
          PCBCCOpB.push_back(TDMDot);
          break;
        }
        else {
          Continue = false;
          break;
        }
      }

      if (!Continue)
        break;
    }

    assert(PCBCCOpA.size() == PCBCCOpB.size() && "Unexpected different size!");
    unsigned Length = PCBCCOpA.size();

    // Ignore if the length of carry chain is too small.
    if (Length <= 2)
      continue;

    // Print the implementation of PreComputingByCarryChain.
    printPCBCC(PCBCCOpA, PCBCCOpB, i, Output);

    /// Generate the result dots.

    // The arrival time of result dots.
    float MaxOpArrivalTime = 0.0f;
    for (unsigned j = 0; j < PCBCCOpA.size(); ++j) {
      MatrixDot *PCBCCOpADot = PCBCCOpA[j];
      MaxOpArrivalTime = std::max(MaxOpArrivalTime, PCBCCOpADot->getArrivalTime());
    }
    for (unsigned j = 0; j < PCBCCOpB.size(); ++j) {
      MatrixDot *PCBCCOpBDot = PCBCCOpB[j];
      MaxOpArrivalTime = std::max(MaxOpArrivalTime, PCBCCOpBDot->getArrivalTime());
    }
    // TODO: set the correct carry chain delay.
    MaxOpArrivalTime = MaxOpArrivalTime + 2.0f;

    std::vector<MatrixDot *> ResultDots;
    for (unsigned j = 0; j < Length; ++j) {
      unsigned C4Idx = j / 4;
      unsigned C4BitIdx = j % 4;

      std::string DotName = "c4_" + utostr_32(i) + "_" + utostr_32(C4Idx) +
                            "_sum" + "[" + utostr_32(C4BitIdx) + "]";

      MatrixDot *ResultDot = createNormalMatrixDot(DotName, 2.0f, 0);
      ResultDots.push_back(ResultDot);
    }
    if (Length % 4 == 0) {
      unsigned C4Idx = (Length - 1) / 4;
      std::string DotName
        = "c4_" + utostr_32(i) + "_" + utostr_32(C4Idx) + "_carry[3]";

      MatrixDot *ResultDot = createNormalMatrixDot(DotName, 2.0f, 0);
      ResultDots.push_back(ResultDot);
    }
    else {
      unsigned C4Idx = Length / 4;
      unsigned C4BitIdx = Length % 4;

      std::string DotName = "c4_" + utostr_32(i) + "_" + utostr_32(C4Idx) +
                            "_sum" + "[" + utostr_32(C4BitIdx) + "]";

      MatrixDot *ResultDot = createNormalMatrixDot(DotName, 2.0f, 0);
      ResultDots.push_back(ResultDot);
    }

    // Clear the dots used in the implementation of PreComputingByCarryChain.
    for (unsigned j = 0; j < Length; ++j) {
      MatrixDot *PCBCCOpADot = PCBCCOpA[j];
      MatrixDot *PCBCCOpBDot = PCBCCOpB[j];

      PCBCCOpADot->setToZero();
      PCBCCOpBDot->setToZero();
    }

    // Insert the result into TDM.
    for (unsigned j = StartPoint; j < StartPoint + Length; ++j) {
      MatrixDot *ResultDot = ResultDots[j - StartPoint];
      MatrixRow *TDMRow = TDM->getRow(j);

      TDMRow->addDot(ResultDot);
    }
  }

  /// For these dots in MulDM which can't be pre-computed by carry-chain,
  /// generate the origin booth coding elements and insert them into the DM.
  for (unsigned i = 0; i < MulDM->getRowNum(); ++i) {
    MatrixRow *MulDMRow = MulDM->getRow(i);

    for (unsigned j = 0; j < MulDMRow->getWidth(); ++j) {
      MatrixDot *MulDMDot = MulDMRow->getDot(j);

      if (MulDMDot->isZero())
        continue;

      Output << "wire " + MulDMDot->getName() << ";\n";
      Output << "PCBCC_LUT PCBCC_LUT_" + utostr_32(i) + "_" + utostr_32(j) << "(\n";

      std::vector<std::string> CodingElements = MulDMDot->getCodingElements();
      Output << "\t.triple_bit_group({" << CodingElements[2] << ", "
        << CodingElements[1] << ", " << CodingElements[0] << "}),\n";
      Output << "\t.current_bit(" << CodingElements[3] << "),\n";
      Output << "\t.previous_bit(" << CodingElements[4] << "),\n";
      Output << "\t.operand_bit(1'b0),\n";
      Output << "\t.result(" << MulDMDot->getName() << ")\n";
      Output << ");\n\n";

      MatrixRow *TDMRow = TDM->getRow(j);
      TDMRow->addDot(MulDMDot);
    }
  }

  // Transport the TDM back to DM.
  unsigned MaxColNum = 0;
  for (unsigned i = 0; i < TDM->getRowNum(); ++i) {
    unsigned ColNum = TDM->getRow(i)->getWidth();
    MaxColNum = std::max(MaxColNum, ColNum);
  }
  for (unsigned i = 0; i < TDM->getRowNum(); ++i) {
    MatrixRow *Row = TDM->getRow(i);
    unsigned ColNum = Row->getWidth();
    unsigned PaddingNum = MaxColNum - ColNum;
    
    for (unsigned j = 0; j < PaddingNum; ++j)
      Row->addZeroDot();
  }

  DM = transportDotMatrix(TDM);

  return DM;
}

void SIRMOAOpt::printPCBCC(std::vector<MatrixDot *> OpA,
                           std::vector<MatrixDot *> OpB,
                           unsigned Idx, raw_fd_ostream &Output) {
  assert(OpA.size() == OpB.size() && "Unexpected different size!");
  unsigned Length = OpA.size();

  // Print the LUT part.
  for (unsigned i = 0; i < Length; ++i) {
    std::string LUTResultName = "CC_result_" + utostr_32(Idx) + "_" + utostr_32(i);

    Output << "wire " + LUTResultName << ";\n";
    Output << "PCBCC_LUT PCBCC_LUT_" + utostr_32(Idx) + "_" + utostr_32(i) << "(\n";
    
    MatrixDot *MulDMDot = OpA[i];
    MatrixDot *DMDot = OpB[i];

    std::vector<std::string> CodingElements = MulDMDot->getCodingElements();
    Output << "\t.triple_bit_group({" << CodingElements[2] << ", "
           << CodingElements[1] << ", " << CodingElements[0] << "}),\n";
    Output << "\t.current_bit(" << CodingElements[3] << "),\n";
    Output << "\t.previous_bit(" << CodingElements[4] << "),\n";
    Output << "\t.operand_bit(" << DMDot->getName() << "),\n";
    Output << "\t.result(" << LUTResultName << ")\n";
    Output << ");\n\n";
  }

  // Print the CarryChain part.
  unsigned C4Num = std::ceil(Length / 4.0f);
  for (unsigned i = 0; i < C4Num; ++i) {
    std::string C4SumName = "c4_" + utostr_32(Idx) + "_" + utostr_32(i) + "_sum";
    std::string C4CarryName = "c4_" + utostr_32(Idx) + "_" + utostr_32(i) + "_carry";

    Output << "wire[3:0] " << C4SumName << ";\n";
    Output << "wire[3:0] " << C4CarryName << ";\n";
    Output << "CARRY4 CARRY4_" + utostr_32(Idx) + "_" + utostr_32(i) << "(\n";

    Output << "\t.DI({";
    for (unsigned j = i * 4; j < (i + 1) * 4; ++j) {
      if ((i + 1) * 4 - 1 - j + i * 4 < Length) {
        MatrixDot *Dot = OpB[(i + 1) * 4 - 1 - j + i * 4];
        Output << Dot->getName();
      }
      else
        Output << "1'b0";      

      if (j != (i + 1) * 4 - 1)
        Output << ", ";
    }
    Output << "}),\n";

    Output << "\t.S({";
    for (unsigned j = i * 4; j < (i + 1) * 4; ++j) {
      if ((i + 1) * 4 - 1 - j + i * 4 < Length)
        Output << "CC_result_" + utostr_32(Idx) + "_"
               << utostr_32((i + 1) * 4 - 1 - j + i * 4);
      else
        Output << "1'b0";

      if (j != (i + 1) * 4 - 1)
        Output << ", ";
    }
    Output << "}),\n";

    Output << "\t.O(" << C4SumName << "),\n";
    Output << "\t.CO(" << C4CarryName << "),\n";
    Output << "\t.CYINIT(1'b0),\n";

    if (i != 0)
      Output << "\t.CI(" << "c4_" + utostr_32(Idx)
             << "_" + utostr_32(i - 1) + "_carry[3])\n";
    else
      Output << "\t.CI(1'b0)\n";

    Output << ");\n\n";
  }
}

DotMatrix *SIRMOAOpt::transportDotMatrix(DotMatrix *DM) {
  unsigned RowNum = DM->getRowNum();
  unsigned ColNum = DM->getColNum();

  DotMatrix *TDM = new DotMatrix(DM->getName());

  for (unsigned j = 0; j < ColNum; ++j) {
    MatrixRow *TRow = new MatrixRow();

    for (unsigned i = 0; i < RowNum; ++i) {
      MatrixRow *Row = DM->getRow(i);

      TRow->addDot(Row->getDot(j));
    }

    TDM->addRow(TRow);
  }
  
  return TDM;
}

DotMatrix *SIRMOAOpt::abandonZeroDots(DotMatrix *TDM) {
  DotMatrix *ResultTDM = new DotMatrix();

  for (unsigned i = 0; i < TDM->getRowNum(); ++i) {
    MatrixRow *ResultTRow = new MatrixRow();

    MatrixRow *OriginTRow = TDM->getRow(i);
    for (unsigned j = 0; j < OriginTRow->getWidth(); ++j) {
      MatrixDot *Dot = OriginTRow->getDot(j);

      if (!Dot->isZero())
        ResultTRow->addDot(Dot);
    }

    ResultTDM->addRow(ResultTRow);
  }

  return ResultTDM;
}

CompressComponent
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

  /// GPC_2_2_LUT
  // Inputs & Outputs
  unsigned GPC_2_2_LUT_Inputs[1] = { 2 };
  std::vector<unsigned> GPC_2_2_LUT_InputsVector(GPC_2_2_LUT_Inputs,
                                                 GPC_2_2_LUT_Inputs + 1);

  CompressComponent *GPC_2_2_LUT
    = new CompressComponent(GPCType, "GPC_2_2_LUT",
                            GPC_2_2_LUT_InputsVector, 2, 1, 0.052f);
  Library.push_back(GPC_2_2_LUT);

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

  /// GPC_22_3_LUT
  // Inputs & Outputs
  unsigned GPC_22_3_LUT_Inputs[2] = { 2, 2 };
  std::vector<unsigned> GPC_22_3_LUT_InputsVector(GPC_22_3_LUT_Inputs,
                                                  GPC_22_3_LUT_Inputs + 2);

  CompressComponent *GPC_22_3_LUT
    = new CompressComponent(GPCType, "GPC_22_3_LUT",
                            GPC_22_3_LUT_InputsVector, 3, 2, 0.051f);
  Library.push_back(GPC_22_3_LUT);

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
//     /// GPC_15_3_LUT
//     // Inputs & Outputs
//     unsigned GPC_15_3_LUT_Inputs[2] = { 5, 1 };
//     std::vector<unsigned> GPC_15_3_LUT_InputsVector(GPC_15_3_LUT_Inputs,
//                                                     GPC_15_3_LUT_Inputs + 2);
// 
//     CompressComponent *GPC_15_3_LUT
//       = new CompressComponent(GPCType, "GPC_15_3_LUT",
//                               GPC_15_3_LUT_InputsVector, 3, 3, 0.049f);
//     Library.push_back(GPC_15_3_LUT);
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

  errs() << "Available GPCs: ";
  for (unsigned i = 0; i < Library.size(); ++i) {
    CompressComponent *Component = Library[i];
    errs() << Component->getName();

    if (i != Library.size() - 1)
      errs() << "; ";
  }
  errs() << "\n";
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

std::vector<unsigned> SIRMOAOpt::calculateIHs(DotMatrix *TDM) {
  // Get the non-zero dot numbers in each row of TDM.
  std::vector<unsigned> NZDotNumList = TDM->getNonZeroDotNumList();
  unsigned MaxNZDotNum = 0;
  for (unsigned i = 0; i < NZDotNumList.size(); ++i) {
    MaxNZDotNum = std::max(MaxNZDotNum, NZDotNumList[i]);
  }

//   // Get the highest priority GPC.
//   typedef std::pair<float, std::pair<float, unsigned> > GPCPriority;
//   std::vector<std::pair<unsigned, GPCPriority> > PriorityList;
//   for (unsigned i = 0; i < Library.size(); ++i) {
//     CompressComponent *Component = Library[i];
// 
//     // Get the information of current GPC.
//     std::vector<unsigned> InputDotNums = Component->getInputDotNums();
//     unsigned OutputDotNum = Component->getOutputDotNum();
//     float CriticalDelay = Component->getCriticalDelay();
//     unsigned Area = Component->getArea();
// 
//     unsigned InputDotNum = 0;
//     for (unsigned j = 0; j < InputDotNums.size(); ++j)
//       InputDotNum += InputDotNums[j];
// 
//     // Evaluate the performance.
//     unsigned CompressedDotNum
//       = InputDotNum > OutputDotNum ? InputDotNum - OutputDotNum : 0;
// 
//     float RealDelay = CriticalDelay + VFUs::WireDelay;
//     //float Performance = ((float) (CompressedDotNum * CompressedDotNum)) / (RealDelay * Area);
//     //float Performance = ((float)CompressedDotNum) / RealDelay;
//     float Performance = ((float)CompressedDotNum) / Area;
// 
//     GPCPriority Priority = std::make_pair(Performance,
//                                           std::make_pair(0.0f - CriticalDelay,
//                                                          InputDotNums[0]));
//     PriorityList.push_back(std::make_pair(i, Priority));
//   }
// 
//   // Sort the PriorityList and get the highest one.
//   std::sort(PriorityList.begin(), PriorityList.end(), sortComponent);
//   unsigned HighestPriorityGPCIdx = PriorityList.back().first;
// 
//   // Calculate the compression ratio.
//   CompressComponent *Component = Library[HighestPriorityGPCIdx];
//   std::vector<unsigned> InputDotNums = Component->getInputDotNums();
//   unsigned OutputDotNum = Component->getOutputDotNum();
//   unsigned InputDotNum = 0;
//   for (unsigned j = 0; j < InputDotNums.size(); ++j)
//     InputDotNum += InputDotNums[j];
  //float CompressionRatio = (float)InputDotNum / OutputDotNum;
  // Temporary set the ratio as 2.
  float CompressionRatio = 2.0f;

  // Calculate the IHs.
  std::vector<unsigned> IHs;

  unsigned IH = TargetHeight;
  while (IH < MaxNZDotNum) {
    IHs.push_back(IH);

    IH = std::floor(IH * CompressionRatio);
  }

  return IHs;
}

DotMatrix *SIRMOAOpt::compressTDMUsingGPC(DotMatrix *TDM, unsigned GPCIdx,
                                          unsigned RowNo, unsigned Level,
                                          unsigned &TotalLevels,
                                          raw_fd_ostream &Output) {
  // Get information of TDM.
  std::vector<unsigned> ActiveNZDotNumList = TDM->getActiveNZDotNumList(Level);

  // Get the GPC to be used.
  CompressComponent *GPC = Library[GPCIdx];

  // Identify if the component is GPC with extra one type.
  bool IsSpecialGPC = isa<SpecialGPC>(GPC);
  unsigned RankOfExtraOne = 0;
  if (IsSpecialGPC) {
    assert(useSepcialGPC && "Unexpected special GPC here!");

    SpecialGPC *SpGPC = dyn_cast<SpecialGPC>(GPC);
    RankOfExtraOne = SpGPC->getRankOfExtraOne();

    // Code for debug
    errs() << "Use the special GPC: " << SpGPC->getName() << "\n";
  }    

  // Collect input dots. It should be noted that, we do not restrict that the
  // inputs of GPC must be fulfilled by dots in TDM here. This is for the
  // convenience of coding. For example, if we want to relieve the restriction
  // someday, we just need to change the code in function "getHighestPriority-
  // Component".
  float MaxInputArrivalTime = 0.0f;
  std::vector<std::vector<MatrixDot *> > InputDots;
  std::vector<unsigned> InputDotNums = GPC->getInputDotNums();
  for (unsigned i = 0; i < InputDotNums.size(); ++i) {
    unsigned InputDotNum = InputDotNums[i];

    std::vector<MatrixDot *> InputDotRow;
    if (RowNo + i < TDM->getRowNum()) {
      for (unsigned j = 0; j < InputDotNum; ++j) {
        unsigned DotIdx = j;

        if (useSepcialGPC) {
          // Reserve the 1'b1 dot so it may be summed by GPC with extra
          // one in process of next row.
          if (i != RankOfExtraOne &&
              TDM->getRow(RowNo + i)->getDot(0)->isOne() &&
              InputDotNum < ActiveNZDotNumList[RowNo + i]) {
            ++DotIdx;
          }
        }        

        if (DotIdx < ActiveNZDotNumList[RowNo + i]) {
          MatrixDot *Dot = TDM->getRow(RowNo + i)->getDot(DotIdx);

          if (useSepcialGPC) {
            // Make sure the input is valid if it is a GPC with extra one.
            if (IsSpecialGPC && i == RankOfExtraOne && DotIdx == 0)
              assert(Dot->getName() == "1'b1" && "Unexpected input dot!");
          }          

          InputDotRow.push_back(Dot);

          // Collect the arrival time of each input dot.
          float InputArrivalTime = Dot->getArrivalTime();
          MaxInputArrivalTime = std::max(MaxInputArrivalTime, InputArrivalTime);

          // Make sure we compress the dots in right level.
          assert(Dot->getLevel() <= Level && "Unexpected level!");
        }
        else {
          MatrixDot *ZeroDot = createNormalMatrixDot("1'b0", 0.0f, 0);
          InputDotRow.push_back(ZeroDot);
        }
      }
    }
    else {
      for (unsigned j = 0; j < InputDotNum; ++j) {
        MatrixDot *ZeroDot = createNormalMatrixDot("1'b0", 0.0f, 0);
        InputDotRow.push_back(ZeroDot);
      }
    }

    InputDots.push_back(InputDotRow);
  }

  // Calculate the level of current GPC.
  unsigned MaxInputDotLevel = 0;
  for (unsigned i = 0; i < InputDots.size(); ++i) {
    for (unsigned j = 0; j < InputDots[i].size(); ++j) {
      unsigned InputDotLevel = InputDots[i][j]->getLevel();
      MaxInputDotLevel = std::max(MaxInputDotLevel, InputDotLevel);
    }
  }
  unsigned GPCLevel = MaxInputDotLevel + 1;

  // Update the final GPC level.
  TotalLevels = std::max(TotalLevels, GPCLevel);

  // Get name and delay for output dots.
  std::string OutputName
    = "gpc_result_" + utostr_32(GPCNum++) + "_" + utostr_32(GPCLevel);
  float OutputArrivalTime
    = MaxInputArrivalTime + GPC->getCriticalDelay() + VFUs::WireDelay;

  // Insert the output dots into TMatrix.
  unsigned OutputDotNum = GPC->getOutputDotNum();
  for (unsigned i = 0; i < OutputDotNum; ++i) {
    // Do not insert if exceed the range of TMatrix.
    if (RowNo + i >= TDM->getRowNum())
      break;

    std::string OutputDotName = OutputName + "[" + utostr_32(i) + "]";

    MatrixDot *Dot
      = createNormalMatrixDot(OutputDotName, OutputArrivalTime, GPCLevel);
    TDM->getRow(RowNo + i)->addDot(Dot);
  }

  // Generate component instance.
  printComponentInstance(GPCIdx, InputDots, OutputName, Output);

  // Clear input dots in TMatrix.
  for (unsigned i = 0; i < InputDots.size(); ++i) {
    std::vector<MatrixDot *> IDsInCurrentRow = InputDots[i];
    for (unsigned j = 0; j < IDsInCurrentRow.size(); ++j) {
      MatrixDot *InputDot = IDsInCurrentRow[j];
      InputDot->setToZero();
    }
  }

  printDotMatrixForDebug(TDM);

  return TDM;
}

unsigned SIRMOAOpt::getHighestPriorityGPC(DotMatrix *TDM, unsigned RowNo,
                                          unsigned Level, unsigned IH) {
  // Get information of TMatrix.
  std::vector<unsigned> NZDotNumList = TDM->getNonZeroDotNumList();
  std::vector<unsigned> ActiveNZDotNumList = TDM->getActiveNZDotNumList(Level);

  // Get the number of dots excess the target intermediate height.
  std::vector<unsigned> ExcessDotNumList;
  for (unsigned i = 0; i < NZDotNumList.size(); ++i) {    
    unsigned ExcessDotNum = NZDotNumList[i] > IH ? NZDotNumList[i] - IH: 0;
    ExcessDotNumList.push_back(ExcessDotNum);
  }

  // Try all library and evaluate its priority.
  std::vector<std::pair<unsigned, PerfType> > PerformanceList;
  for (unsigned i = 0; i < Library.size(); ++i) {
    CompressComponent *GPC = Library[i];

    // Get the information of current GPC.
    std::vector<unsigned> InputDotNums = GPC->getInputDotNums();
    unsigned InputDotNum = 0;
    for (unsigned j = 0; j < InputDotNums.size(); ++j) {
      InputDotNum += InputDotNums[j];
    }

    unsigned OutputDotNum
      = std::min(GPC->getOutputDotNum(), TDM->getRowNum() - RowNo + 1);

    unsigned CompressedDotNum
      = InputDotNum > OutputDotNum ? InputDotNum - OutputDotNum : 0;

    float CriticalDelay = GPC->getCriticalDelay() + VFUs::WireDelay;
    unsigned Area = GPC->getArea();

    /// Ignore the invalid GPC which satisfy following conditions:
    bool GPCInValid = false;

    // 1) eliminate dots more than what we need.
    if (InputDotNums[0] - 1 > ExcessDotNumList[RowNo])
      GPCInValid = true;

    // 2) Inputs can not be fulfilled.
    if (RowNo + InputDotNums.size() > TDM->getRowNum())
      GPCInValid = true;
    else {
      for (unsigned j = 0; j < InputDotNums.size(); ++j) {
        if (InputDotNums[j] > ActiveNZDotNumList[RowNo + j]) {
          GPCInValid = true;
          break;
        }
      }
    }      

    // 3) No available 1'b1 if the GPC is special GPC.
    if (SpecialGPC *SpGPC = dyn_cast<SpecialGPC>(GPC)) {
      unsigned ExtraOneRank = SpGPC->getRankOfExtraOne();

      MatrixRow *Row = TDM->getRow(RowNo + ExtraOneRank);
      if (!Row->getDot(0)->isOne())
        GPCInValid = true;
    }

    if (GPCInValid)
      continue;

    // Evaluate the performance.
    
    //float Performance = ((float) (CompressedDotNum * CompressedDotNum)) / (RealDelay * Area);
    //float Performance = ((float)CompressedDotNum) / RealDelay;
    float CompressRatio = ((float)InputDotNum) / OutputDotNum;
    float NegativeDelay = 0.0f - CriticalDelay;
    PerfType Perf
      = std::make_pair(std::make_pair(InputDotNums[0], CompressRatio), NegativeDelay);
    PerformanceList.push_back(std::make_pair(i, Perf));
  }

  assert(!PerformanceList.empty() && "No feasible GPC!");

  // Sort the PriorityList and get the highest one.
  std::sort(PerformanceList.begin(), PerformanceList.end(), sortInPeformance);

//   // Debug
//   errs() << "Component performance list is as follows:\n";
//   for (unsigned i = 0; i < PriorityList.size(); ++i) {
//     unsigned ComponentIdx = PriorityList[i].first;
// 
//     CompressComponent *Component = Library[ComponentIdx];
// 
//     errs() << Component->getName() << "--" << PriorityList[i].second.first << "\n";
//   }


  return PerformanceList.back().first;
}

DotMatrix *SIRMOAOpt::compressTDMInLevel(DotMatrix *TDM, unsigned IH, unsigned Level,
                                         unsigned &TotalLevels, unsigned &TotalArea,
                                         raw_fd_ostream &Output) {
  // Get the informations of the TMatrix.
  std::vector<unsigned> NZDotNumList = TDM->getNonZeroDotNumList();
  std::vector<unsigned> ActiveNZDotNumList = TDM->getActiveNZDotNumList(Level);

  // Compress row by row.
  for (unsigned i = 0; i < TDM->getRowNum(); ++i) {
    // Compress current row if it has dots more than target IH.
    while (NZDotNumList[i] > IH) {
      unsigned GPCIdx
        = getHighestPriorityGPC(TDM, i, Level, IH);

      TDM = compressTDMUsingGPC(TDM, GPCIdx, i, Level, TotalLevels, Output);

      // Update the area.
      CompressComponent *GPC = Library[GPCIdx];
      TotalArea += GPC->getArea();

      // Do some clean up work.
      TDM = abandonZeroDots(TDM);

      // Update the informations of the TMatrix.
      NZDotNumList = TDM->getNonZeroDotNumList();
      ActiveNZDotNumList = TDM->getActiveNZDotNumList(Level);
    }
  }

  return TDM;
}

void SIRMOAOpt::compressDotMatrix(DotMatrix *DM, raw_fd_ostream &Output) {
  // Code for debug.
  printDotMatrixForDebug(DM);

  /// Prepare for the compress progress by transport the DotMatrix and
  /// abandon the ZeroDots. Then the dots in TDM are sorted according
  /// to the algorithm settings.

  DotMatrix *TDM = transportDotMatrix(DM);
  TDM = abandonZeroDots(TDM);

  // Code for debug.
  printDotMatrixForDebug(DM);

  /// Calculate the ideal intermediate height(IH).

  std::vector<unsigned> IHs = calculateIHs(TDM);

  /// Start to compress the TDM
  unsigned TotalLevels = 0;
  unsigned TotalArea = 0;
  for (unsigned i = 0; i < IHs.size(); ++i) {
    unsigned IH = IHs[IHs.size() - i - 1];

    bool Continue = true;
    while (Continue) {
      // We expect each level of IH can be achieved in one compress
      // level (one level of GPCs), that is, the level index of IH
      // equals to the active stage in compression.
      TDM = compressTDMInLevel(TDM, IH, i, TotalLevels, TotalArea, Output);
    
      // Determine if we need to continue compressing.
      std::vector<unsigned> NZDotNumList = TDM->getNonZeroDotNumList();
      Continue = false;
      for (unsigned i = 0; i < NZDotNumList.size(); ++i) {
        if (NZDotNumList[i] > IH)
          Continue = true;
      }
    }
  }

  errs() << "Synthesized compressor tree with GPC level of ["
         << utostr_32(TotalLevels) << "] and Area of ["
         << utostr_32(TotalArea) << "]\n";

  /// Finish the compress by sum the left-behind bits using ternary CPA.
  MatrixRow *CPADataA = new MatrixRow();
  MatrixRow *CPADataB = new MatrixRow();
  MatrixRow *CPADataC = new MatrixRow();
  
  for (unsigned i = 0; i < TDM->getRowNum(); ++i) {
    MatrixRow *Row = TDM->getRow(i);

    if (Row->getWidth() == 0) {
      CPADataA->addZeroDot();
      CPADataB->addZeroDot();
      CPADataC->addZeroDot();
    }
    else if (Row->getWidth() == 1) {
      CPADataA->addDot(Row->getDot(0));

      CPADataB->addZeroDot();
      CPADataC->addZeroDot();
    }
    else if (Row->getWidth() == 2) {
      CPADataA->addDot(Row->getDot(0));
      CPADataB->addDot(Row->getDot(1));

      CPADataC->addZeroDot();
    }
    else {
      assert(Row->getWidth() == 3 && "Unexpected width!");

      CPADataA->addDot(Row->getDot(0));
      CPADataB->addDot(Row->getDot(1));
      CPADataC->addDot(Row->getDot(2));
    }
  }

  unsigned CPAWidth = DM->getColNum();
  assert(CPADataA->getWidth() == CPAWidth &&
         CPADataB->getWidth() == CPAWidth &&
         CPADataC->getWidth() == CPAWidth &&
         "Should be same size!");

  Output << "\n";

  // Print the declaration and definition of DataA & DataB & DataC of CPA.
  Output << "wire[" << utostr_32(CPAWidth - 1) << ":0] CPA_DataA = {";
  for (unsigned i = 0; i < CPAWidth; ++i) {
    Output << CPADataA->getDot(CPAWidth - 1 - i)->getName();

    if (i != CPAWidth - 1)
      Output << ", ";
  }
  Output << "};\n";

  Output << "wire[" << utostr_32(CPAWidth - 1) << ":0] CPA_DataB = {";
  for (unsigned i = 0; i < CPAWidth; ++i) {
    Output << CPADataB->getDot(CPAWidth - 1 - i)->getName();

    if (i != CPAWidth - 1)
      Output << ", ";
  }
  Output << "};\n";

  Output << "wire[" << utostr_32(CPAWidth - 1) << ":0] CPA_DataC = {";
  for (unsigned i = 0; i < CPAWidth; ++i) {
    Output << CPADataC->getDot(CPAWidth - 1 - i)->getName();

    if (i != CPAWidth - 1)
      Output << ", ";
  }
  Output << "};\n";

  // Print the implementation of the CPA.
  Output << "wire[" << utostr_32(CPAWidth - 1)
         << ":0] CPA_Result = CPA_DataA + CPA_DataB + CPA_DataC;\n";

  // Print the implementation of the result.
  Output << "assign result = CPA_Result;\n";

  // Print the end of module.
  Output << "\nendmodule\n\n";
}

void SIRMOAOpt::printDotMatrixForDebug(DotMatrix *DM) {
  for (unsigned i = 0; i < DM->getRowNum(); ++i) {
    MatrixRow *MR = DM->getRow(i);

    for (unsigned j = 0; j < MR->getWidth(); ++j) {
      MatrixDot *Dot = MR->getDot(j);

      DebugOutput << Dot->getName();

      if (j != MR->getWidth() - 1)
        DebugOutput << "  ";
    }

    DebugOutput << "\n";
  }

  DebugOutput << "\n\n";
}

void SIRMOAOpt::printGPCModule(raw_fd_ostream &Output) {
  // Generate the 2-2 compressor.
  Output << "module GPC_2_2_LUT(\n";
  Output << "\tinput wire[1:0] col0,\n";
  Output << "\toutput wire[1:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1];\n\n";
  Output << "endmodule\n\n";

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

  // Generate the 22-3 compressor.
  Output << "module GPC_22_3_LUT(\n";
  Output << "\tinput wire[1:0] col0,\n";
  Output << "\tinput wire[1:0] col1,\n";
  Output << "\toutput wire[2:0] sum\n";
  Output << ");\n\n";
  Output << "\tassign sum = col0[0] + col0[1] + 2 * (col1[0] + col1[1]);\n\n";
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
                                  std::vector<std::vector<MatrixDot *> > InputDots,
                                  std::string OutputName,
                                  raw_fd_ostream &Output) {
  // Get the Component to be used and its information.
  CompressComponent *Component = Library[ComponentIdx];
  std::vector<unsigned> InputDotNums = Component->getInputDotNums();
  unsigned OutputDotNum = Component->getOutputDotNum();
  std::string ComponentName = Component->getName();

  // Identify the special GPC component.
  bool IsSpecialGPC = isa<SpecialGPC>(Component);
  unsigned RankOfExtraOne = 0;
  if (IsSpecialGPC) {
    SpecialGPC *SpGPC = dyn_cast<SpecialGPC>(Component);
    RankOfExtraOne = SpGPC->getRankOfExtraOne();
  }

  // Print the declaration of the result.  
  Output << "wire [" << utostr_32(OutputDotNum - 1) << ":0] " << OutputName << ";\n";

  // Print the instantiation of the compressor module.
  Output << ComponentName << " " + ComponentName + "_" << utostr_32(GPCNum) << "(";

  // Print the inputs and outputs instance.
  for (unsigned i = 0; i < InputDotNums.size(); ++i) {
    // Ignore the empty column.
    if (InputDotNums[i] == 0)
      continue;

    Output << ".col" << utostr_32(i) << "({";

    std::vector<MatrixDot *> InputDotRow = InputDots[i];
    assert(InputDotRow.size() == InputDotNums[i] || InputDotRow.size() == 0
           && "Unexpected input dot number!");
    for (unsigned j = 0; j < InputDotRow.size(); ++j) {
      // If this is a special GPC, then do not print the extra 1'b1 input.
      if (IsSpecialGPC && i == RankOfExtraOne && j == 0)
        continue;

      Output << InputDotRow[j]->getName();

      if (j != InputDotRow.size() - 1)
        Output << ", ";
    }

    Output << "}), ";
  }

  Output << ".sum(" << OutputName << ")";

  Output << ");\n";
}
