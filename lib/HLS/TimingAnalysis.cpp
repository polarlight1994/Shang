//---- TimingAnalysis.cpp - Abstract Interface for Timing Analysis -*- C++ -*-//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file datapath define the delay estimator based on linear approximation.
//
//===----------------------------------------------------------------------===//
#include "vast/TimingAnalysis.h"
#include "vast/Passes.h"
#include "vast/LuaI.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"
#include "vast/VASTMemoryBank.h"

#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-timing-estimator"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;

TimingAnalysis::PhysicalDelay TimingAnalysis::getArrivalTime(VASTValue *To,
                                                             VASTValue *From) {
  return TA->getArrivalTime(To, From);
}

TimingAnalysis::PhysicalDelay TimingAnalysis::getArrivalTime(VASTSelector *To,
                                                             VASTSeqValue *From) {
  return TA->getArrivalTime(To, From);
}

TimingAnalysis::PhysicalDelay TimingAnalysis::getArrivalTime(VASTSelector *To,
                                                             VASTExpr *Thu,
                                                             VASTSeqValue *From) {
  return TA->getArrivalTime(To, Thu, From);
}

bool TimingAnalysis::isBasicBlockUnreachable(BasicBlock *BB) const {
  return TA->isBasicBlockUnreachable(BB);
}

void TimingAnalysis::InitializeTimingAnalysis(Pass *P) {
  TA = &P->getAnalysis<TimingAnalysis>();
}

void TimingAnalysis::extractDelay(const VASTLatch &L, VASTValue *V,
                                  ArrivalMap &Arrivals) {
  // Simply add the zero delay record if the fanin itself is a register.
  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V)) {
    if (!SV->isSlot() && !SV->isFUOutput()) {
      // Please note that this insertion may fail (V already existed), but it
      // does not hurt because here we only want to ensure the record exist.
      Arrivals.insert(std::make_pair(SV, PhysicalDelay()));
      return;
    }
  }

  typedef std::set<VASTSeqValue*> LeafSet;
  LeafSet Leaves;
  V->extractCombConeLeaves(Leaves);
  if (Leaves.empty())
    return;

  VASTSelector *Sel = L.getSelector();
  SmallVector<VASTSeqValue*, 4> MissedLeaves;

  typedef LeafSet::iterator iterator;
  for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
    VASTSeqValue *Leaf = *I;

    PhysicalDelay Delay = getArrivalTime(Sel, Leaf);

    // If there is more than one paths between Leaf and selector, the delay
    // is not directly available.
    if (Delay == None) {
      MissedLeaves.push_back(Leaf);
      continue;
    }

    // Otherwise Update the delay.
    PhysicalDelay &OldDelay = Arrivals[Leaf];
    OldDelay = std::max(OldDelay, Delay);
  }

  if (MissedLeaves.empty())
    return;

  VASTSlot *ReadSlot = L.getSlot();
  typedef VASTSelector::ann_iterator ann_iterator;
  for (ann_iterator I = Sel->ann_begin(), E = Sel->ann_end(); I != E; ++I) {
    const VASTSelector::Annotation &Ann = *I->second;
    ArrayRef<VASTSlot*> Slots(Ann.getSlots());
    VASTExpr *Thu = I->first;
    assert(Thu->isAnnotation() && "Unexpected Expr Type!");

    if (std::find(Slots.begin(), Slots.end(), ReadSlot) == Slots.end())
      continue;

    typedef SmallVector<VASTSeqValue*, 4>::iterator leaf_iterator;
    for (leaf_iterator I = MissedLeaves.begin(), E = MissedLeaves.end();
         I != E; ++I) {
      VASTSeqValue *Leaf = *I;

      PhysicalDelay Delay = getArrivalTime(Sel, Thu, Leaf);

      if (Delay == None)
        continue;

      PhysicalDelay &OldDelay = Arrivals[Leaf];
      OldDelay = std::max(OldDelay, Delay);
    }
  }

  DEBUG(for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;

    if (LLVM_LIKELY(Arrivals.count(SV)))
      continue;

    Sel->getFanin().printAsOperand(dbgs());
    dbgs() << '\n';
    Sel->getGuard().printAsOperand(dbgs());
    dbgs() << '\n';
    Sel->dumpFanins();

    dbgs() << "Missed arrival time from: " << SV->getName() << '\n';
    cast<VASTExpr>(V)->dumpExprTree();
    dbgs() << '\n';
  });
}

void TimingAnalysis::printArrivalPath(raw_ostream &OS, VASTSelector *To,
                                      VASTValue *From) {
}

void TimingAnalysis::printArrivalPath(raw_ostream &OS, VASTSelector *To,
                                      VASTExpr *Thu, VASTValue *From) {
}

void TimingAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredID(ControlLogicSynthesisID);
  AU.addRequiredID(SelectorSynthesisForAnnotationID);
  AU.addRequiredID(DatapathNamerID);
  AU.addRequired<TimingAnalysis>();
  AU.setPreservesAll();
}

char TimingAnalysis::ID = 0;

namespace llvm {
void initializeTimingNetlistPass(PassRegistry &Registry);
}

INITIALIZE_ANALYSIS_GROUP(TimingAnalysis,
                          "High-level Synthesis Timing Analysis",
                          TimingNetlist)

//===----------------------------------------------------------------------===//
namespace {
// Bit level arrival time.
struct ArrivalTime : public ilist_node<ArrivalTime> {
  VASTValue *Src;
  float Arrival;
  // Arrival to bit range [ToLB, ToUB)
  uint8_t ToUB, ToLB;
  ArrivalTime(VASTValue *Src, float Arrival, uint8_t ToUB, uint8_t ToLB);
  ArrivalTime() : Src(NULL), Arrival(0.0f), ToUB(0), ToLB(0) {}
  void verify() const;

  unsigned width() const { return ToUB - ToLB; }
};

class DelayModel : public ilist_node<DelayModel> {
  VASTExpr *Node;
  // The fanin to the current delay model, order matters.
  ArrayRef<DelayModel*> Fanins;

  ilist<ArrivalTime> Arrivals;
  std::map<VASTValue*, ArrivalTime*> ArrivalStart;

  ilist<ArrivalTime>::iterator
  findInsertPosition(ArrivalTime *Start, VASTValue *V, uint8_t ToLB);

  void addArrival(VASTValue *V, float Arrival, uint8_t ToUB, uint8_t ToLB);

  void updateArrivalCarryChain(unsigned i, float Base, float PerBit);
  void updateArrivalCritial(unsigned i, float Delay, uint8_t ToUB, uint8_t ToLB);
  void updateArrivalParallel(unsigned i, float Delay);

  void updateArrivalParallel(float delay);
  void updateArrivalCritial(float delay, uint8_t ToUB, uint8_t ToLB);

  void updateBitCatArrival();
  void updateBitRepeatArrival();
  void updateBitExtractArrival();
  void updateBitMaskArrival();
  void updateReductionArrival();
  void updateROMLookUpArrival();

  template<typename VFUTy>
  void updateCarryChainArrival(VFUTy *FU)  {
    unsigned BitWidth = Node->getBitWidth();

    // Dirty HACK: We only have the data up to 64 bit FUs.
    float Delay = FU->lookupLatency(std::min(BitWidth, 64u));
    float PreBit = Delay / BitWidth;

    unsigned NumOperands = Node->size();
    float Base = PreBit * (NumOperands - 1);

    for (unsigned i = 0, e = Node->size(); i < e; ++i)
      updateArrivalCarryChain(i, Base, PreBit);
  }

  void updateCmpArrivial();
  void updateShiftAmt();
  void updateShlArrival();
  void updateShrArrival();
public:
  DelayModel() : Node(NULL) {}
  DelayModel(VASTExpr *Node, ArrayRef<DelayModel*> Fanins);
  ~DelayModel();

  typedef ArrayRef<DelayModel>::iterator iterator;

  void updateArrival();

  void verify() const;

  // Iterative the arrival times.
  typedef ilist<ArrivalTime>::iterator arrival_iterator;
  arrival_iterator arrival_begin() { return Arrivals.begin(); }
  arrival_iterator arrival_end() { return Arrivals.end(); }
  typedef ilist<ArrivalTime>::const_iterator const_arrival_iterator;
  const_arrival_iterator arrival_begin() const { return Arrivals.begin(); }
  const_arrival_iterator arrival_end() const { return Arrivals.end(); }

  bool hasArrivalFrom(VASTValue *V) const {
    return ArrivalStart.count(V);
  }

  // Get the iterator point to the first arrival time of a given source node.
  const_arrival_iterator arrival_begin(VASTValue *V) const {
    std::map<VASTValue*, ArrivalTime*>::const_iterator I = ArrivalStart.find(V);
    if (I == ArrivalStart.end())
      return Arrivals.end();

    return I->second;
  }

  bool inRange(const_arrival_iterator I, VASTValue *V) const {
    if (I == Arrivals.end())
      return false;

    return I->Src == V;
  }
};

/// Timinging Netlist - Annotate the timing information to the RTL netlist.
class TimingNetlist : public VASTModulePass, public TimingAnalysis {
public:
// The bit-level delay model.
  ilist<DelayModel> Models;
  std::map<VASTExpr*, DelayModel*> ModelMap;

  DelayModel *createModel(VASTExpr *Expr);

  void buildTimingNetlist(VASTValue *V);
public: 
  static char ID;

  TimingNetlist();
  DelayModel *lookUpDelayModel(VASTExpr *Expr) const {
    std::map<VASTExpr*, DelayModel*>::const_iterator I = ModelMap.find(Expr);
    assert(I != ModelMap.end() && "Model of Expr cannot be found!");
    return I->second;
  }

  PhysicalDelay getArrivalTimeImpl(VASTValue *To, VASTValue *From);
  PhysicalDelay getArrivalTimeImpl(VASTSelector *To, VASTValue *From);

  PhysicalDelay getArrivalTime(VASTValue *To, VASTValue *From);
  PhysicalDelay getArrivalTime(VASTSelector *To, VASTSeqValue *From);
  PhysicalDelay getArrivalTime(VASTSelector *To, VASTExpr *Thu,
                               VASTSeqValue *From);

  void printArrivalPathImpl(raw_ostream &OS, VASTValue *To, VASTValue *From);

  void printArrivalPath(raw_ostream &OS, VASTSelector *To, VASTValue *From) {
    printArrivalPathImpl(OS, To->getFanin().get(), From);
    printArrivalPathImpl(OS, To->getGuard().get(), From);
  }

  void printArrivalPath(raw_ostream &OS, VASTSelector *To, VASTExpr *Thu, VASTValue *From) {
    printArrivalPathImpl(OS, Thu, From);
    printArrivalPathImpl(OS, To->getFanin().get(), Thu);
    printArrivalPathImpl(OS, To->getGuard().get(), Thu);
  }

  bool isBasicBlockUnreachable(BasicBlock *BB) const {
    return false;
  }

  virtual void releaseMemory();
  virtual bool runOnVASTModule(VASTModule &VM);
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  void print(raw_ostream &OS) const;

  /// getAdjustedAnalysisPointer - This method is used when a pass implements
  /// an analysis interface through multiple inheritance.  If needed, it
  /// should override this to adjust the this pointer as needed for the
  /// specified pass info.
  virtual void *getAdjustedAnalysisPointer(const void *ID) {
    if (ID == &TimingAnalysis::ID)
      return (TimingAnalysis*)this;

    return this;
  }
};
}
//===----------------------------------------------------------------------===//
ArrivalTime::ArrivalTime(VASTValue *Src, float Arrival,
                         uint8_t ToUB, uint8_t ToLB)
  : Src(Src), Arrival(Arrival), ToUB(ToUB), ToLB(ToLB) {
  assert(ToUB > ToLB && "Bad Range!");
}

void ArrivalTime::verify() const {
  assert(ToUB > ToLB && "Bad Range!");
}

//===----------------------------------------------------------------------===//
DelayModel::DelayModel(VASTExpr *Node, ArrayRef<DelayModel*> Fanins)
  : Node(Node) {
  DelayModel **Data = new DelayModel*[Fanins.size()];
  std::uninitialized_copy(Fanins.begin(), Fanins.end(), Data);
  this->Fanins = ArrayRef<DelayModel*>(Data, Fanins.size());
}

DelayModel::~DelayModel() {
  if (Fanins.data())
    delete[] Fanins.data();
}

void DelayModel::verify() const {
  const ArrivalTime *LastAT = NULL;

  for (const_arrival_iterator I = arrival_begin(), E = arrival_end(); I != E; ++I) {
    const ArrivalTime *AT = I;

    AT->verify();

    if (LLVM_UNLIKELY(LastAT == NULL) || LastAT->Src != AT->Src) {
      std::map<VASTValue*, ArrivalTime*>::const_iterator J = ArrivalStart.find(AT->Src);
      assert(J != ArrivalStart.end() && "Arrival Start missed!");
      assert(J->second == AT && "Broken arrival start map!");
      LastAT = AT;
      continue;
    }

    if (LastAT->ToUB > AT->ToLB)
      llvm_unreachable("Unexpected overapped arrival range!");

    if (LastAT->ToUB == AT->ToLB && LastAT->Arrival == AT->Arrival)
      llvm_unreachable("Unexpected segments with the same arrival!");

    LastAT = AT;
  }
}

DelayModel::arrival_iterator
DelayModel::findInsertPosition(ArrivalTime *Start, VASTValue *V, uint8_t ToLB) {
  if (Start == NULL)
    return arrival_begin();

  // Find the insert position.
  arrival_iterator InsertBefore = Start;
  do {

    if (InsertBefore->ToLB > ToLB)
      break;

    ++InsertBefore;
  } while (inRange(InsertBefore, V));

  return InsertBefore;
}

namespace {
struct ArrivalVerifier {
  DelayModel *M;
  ArrivalVerifier(DelayModel *M) : M(M) {}
  ~ArrivalVerifier() { M->verify(); }
};
}

void DelayModel::addArrival(VASTValue *V, float Arrival, uint8_t ToUB, uint8_t ToLB) {
  ArrivalVerifier AV(this);
  ArrivalTime *&Start = ArrivalStart[V];
  // Trivial Case: Simply add the new arrival time.
  if (Start == NULL) {
    Start = new ArrivalTime(V, Arrival, ToUB, ToLB);
    Arrivals.push_front(Start);
    return;
  }

  ilist<ArrivalTime> BiggerArrivals;

  // Remove any range the are hidden by the current range.
  for (arrival_iterator I = Start; inRange(I, V); /*++I*/) {
    ArrivalTime *AT = I++;
    // Update start if it is removed.
    if (Start == NULL)
      Start = AT;

    // Not overlapped, skip.
    if (AT->ToUB <= ToLB)
      continue;

    // Out of range, the current range will not hide them.
    if (AT->ToLB >= ToUB)
      break;

    if (AT->Arrival > Arrival) {
      // The new range is completely hidden, nothing to do.
      if (AT->ToLB <= ToLB && AT->ToUB >= ToUB)
        return;

      // Try to reduce the range.
      if (AT->ToLB <= ToLB) {
        ToLB = AT->ToUB;
        continue;
      }

      if (AT->ToUB >= ToUB) {
        ToUB = AT->ToLB;
        break;
      }

      if (AT == Start)
        Start = NULL;

      // The bigger arrival is a subset of [ToLB, ToUB)
      // Temporary move the bigger arrivals to another list.
      Arrivals.remove(AT);
      BiggerArrivals.push_back(AT);
      continue;
    }

    // Completely hidden.
    if (AT->ToLB >= ToLB && AT->ToUB <= ToUB) {
      if (AT == Start)
        Start = NULL;

      Arrivals.erase(AT);
      continue;
    }

    // Not only 3 situation can be happened:
    // 1:
    // |- NewRange -|
    //    |-     AT   -|
    if (AT->ToLB >= ToLB) {
      assert(AT->ToUB > ToUB && "Unexpected UB of current range!");
      // Cut off the range the intersects with the new range.
      AT->ToLB = ToUB;
      continue;
    }

    // 2:
    //      |- NewRange -|
    // |-     AT   -|
    if (AT->ToUB <= ToUB) {
      assert(AT->ToLB < ToLB && "Unexpected UB of current range!");
      // Cut off the range the intersects with the new range.
      AT->ToUB = ToLB;
      continue;
    }

    // 3:
    //    |- NewRange -|
    // |-         AT       -|
    // Now Split AT to:
    //      |- NewRange -|
    // |-AT-|            |-NewAT-|
    assert(AT->ToLB < ToLB && AT->ToUB > ToUB && "Unexpected range of AT!");
    uint8_t TmpUB = AT->ToUB;
    AT->ToUB = ToLB;
    ArrivalTime *NewAT = new ArrivalTime(V, AT->Arrival, TmpUB, ToUB);
    Arrivals.insertAfter(AT, NewAT);
    break;
  }

#ifndef NDEBUG
  verify();
#endif

  // Find the insert position.
  arrival_iterator InsertBefore = findInsertPosition(Start, V, ToLB);

  // Now insert the arrival
  do {
    uint8_t CurUB = ToUB;
    if (!BiggerArrivals.empty()) {
      CurUB = BiggerArrivals.front().ToLB;
      assert(CurUB <= ToUB && "Bad UB!");
    }

    if (CurUB > ToLB) {
      ArrivalTime *AT = new ArrivalTime(V, Arrival, CurUB, ToLB);
      Arrivals.insert(InsertBefore, AT);

      if (Start == InsertBefore || Start == NULL)
        Start = AT;
    }

    ToLB = CurUB;

    if (!BiggerArrivals.empty()) {
      ArrivalTime *BiggerAT = BiggerArrivals.begin();
      BiggerArrivals.remove(BiggerAT);
      // Put back the bigger arrivals.
      Arrivals.insert(InsertBefore, BiggerAT);
      // This means the new range is split.
      ToLB = BiggerAT->ToUB;
    }
  } while (ToLB < ToUB);

  assert(BiggerArrivals.empty() && "Unexpected bigger arrivals!");

  // Merge the range with the same source node and arrival time.
  ArrivalTime *LastAT = NULL;
  for (arrival_iterator I = Start; inRange(I, V); LastAT = I, ++I) {
    if (LastAT == NULL)
      continue;

    if (LastAT->Arrival != I->Arrival)
      continue;

    if (LastAT->ToUB != I->ToLB)
      continue;

    I->ToLB = LastAT->ToLB;

    // Update start if we erased it.
    if (LastAT == Start)
      Start = I;

    Arrivals.erase(LastAT);
  }
}

//===----------------------------------------------------------------------===//
static VASTValue *GetAsLeaf(VASTValPtr V) {
  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>())
    return SV;

  if (VASTExpr *Expr = V.getAsLValue<VASTExpr>())
  if (Expr->isHardAnnotation())
    return Expr;

  return NULL;
}

static float GetLeafDelay(VASTValue *V) {
  VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V);

  if (SV == NULL || !SV->isFUOutput())
    return 0.0f;

  if (!isa<VASTMemoryBank>(SV->getParent()))
    return 0.0f;

  return LuaI::Get<VFUMemBus>()->AddrLatency;
}

//===----------------------------------------------------------------------===//

void DelayModel::updateArrivalParallel(unsigned i, float Delay) {
  VASTValPtr V = Node->getOperand(i);

  if (VASTValue *Val = GetAsLeaf(V)) {
    addArrival(Val, Delay + GetLeafDelay(Val), Node->getBitWidth(), 0);
    return;
  }

  DelayModel *M = Fanins[i];
  if (M == NULL)
    return;

  // Forward the arrival from M with delay without changing the output bit.
  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I)
    addArrival(I->Src, I->Arrival + Delay, I->ToUB, I->ToLB);
}

void DelayModel::updateArrivalParallel(float delay) {
  for (unsigned i = 0, e = Node->size(); i < e; ++i)
    updateArrivalParallel(i, delay);
}

void DelayModel::updateArrivalCritial(unsigned i, float Delay,
                                      uint8_t ToUB, uint8_t ToLB) {
  VASTValPtr V = Node->getOperand(i);

  if (VASTValue *Val = GetAsLeaf(V)) {
    addArrival(Val, Delay + GetLeafDelay(Val), ToUB, ToLB);
    return;
  }

  DelayModel *M = Fanins[i];
  if (M == NULL)
    return;

    // Forward the arrival from M with delay.
  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I)
    addArrival(I->Src, I->Arrival + Delay, ToUB, ToLB);
}

void DelayModel::updateArrivalCritial(float delay, uint8_t ToUB, uint8_t ToLB) {
  for (unsigned i = 0, e = Node->size(); i < e; ++i)
    updateArrivalCritial(i, delay, ToUB, ToLB);
}

void DelayModel::updateBitCatArrival() {
  unsigned OffSet = Node->getBitWidth();

  for (unsigned i = 0, e = Node->size(); i < e; ++i) {
    VASTValPtr V = Node->getOperand(i);
    unsigned BitWidth = V->getBitWidth();
    OffSet -= BitWidth;

    if (VASTValue *Val = GetAsLeaf(V)) {
      addArrival(Val, 0.0f + GetLeafDelay(Val), OffSet + BitWidth, OffSet);
      continue;
    }

    DelayModel *M = Fanins[i];
    if (M == NULL)
      continue;

    // Otherwise Transform the arrival bits.
    for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I)
      addArrival(I->Src, I->Arrival + 0.0f, I->ToUB + OffSet, I->ToLB + OffSet);
  }

  assert(OffSet == 0 && "Bad Offset!");
}

void DelayModel::updateBitRepeatArrival() {
  unsigned BitWidth = Node->getBitWidth(); 
  float delay = 0.0f;

  updateArrivalCritial(0, delay, BitWidth, 0);
}

void DelayModel::updateBitExtractArrival() {
  unsigned BitWidth = Node->getBitWidth();

  VASTValPtr V = Node->getOperand(0);

  if (VASTValue *Val = GetAsLeaf(V)) {
    addArrival(Val, 0.0f + GetLeafDelay(Val), BitWidth, 0);
    return;
  }

  DelayModel *M = Fanins[0];

  if (M == NULL)
    return;

  uint8_t UB = Node->getUB(), LB = Node->getLB();

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    unsigned CurLB = std::max(AT->ToLB, LB), CurUB = std::min(AT->ToUB, UB);

    if (CurLB >= CurUB)
      continue;

    // Transform the arrival bits.
    addArrival(AT->Src, AT->Arrival, CurUB - LB, CurLB - LB);
  }
}

void DelayModel::updateBitMaskArrival() {
  // BitMask is a trivial operator.
  // TODO: Ignore the known bits?
  updateArrivalParallel(0.0f);
}

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

void DelayModel::updateReductionArrival() {
  VASTValPtr V = Node->getOperand(0);
  unsigned NumBits = V->getBitWidth();
  // Only reduce the unknow bits.
  NumBits -= VASTBitMask(V).getNumKnownBits();
  unsigned LogicLevels = LogCeiling(NumBits, VFUs::MaxLutSize);
  updateArrivalParallel(LogicLevels * VFUs::LUTDelay);
}

void DelayModel::updateROMLookUpArrival() {
  VASTValPtr Addr = Node->getOperand(0);
  unsigned NumBits = Addr->getBitWidth();
  // Only reduce the unknow bits.
  NumBits -= VASTBitMask(Addr).getNumKnownBits();
  unsigned LogicLevels = LogCeiling(NumBits, VFUs::MaxLutSize);
  float delay = LogicLevels * VFUs::LUTDelay;
  unsigned BitWidth = Node->getBitWidth();
  updateArrivalCritial(delay, BitWidth, 0);
}

void DelayModel::updateArrivalCarryChain(unsigned i, float Base, float PerBit) {
  unsigned BitWidth = Node->getBitWidth();
  VASTValPtr V = Node->getOperand(i);
  APInt KnwonBits = VASTBitMask(V).getKnownBits();
  // Get the upperbound and lowerbound of the unknwon bits, only calculate the
  // carry chain propagation delay for the unknwon bits. For the upper bound,
  // we also consider the extra carry bit.
  unsigned UB = std::min(BitWidth - KnwonBits.countLeadingOnes() + 1, BitWidth),
           LB = KnwonBits.countTrailingOnes();

  if (VASTValue *Val = GetAsLeaf(V)) {
    float OutputDelay = GetLeafDelay(Val);
    for (unsigned j = LB; j < UB; ++j) {
      // Do not need the arrival time if the bit is known.
      if (Node->isBitKnownAt(j))
        continue;

      addArrival(Val, Base + j * PerBit + OutputDelay, j + 1, j);
    }

    return;
  }

  DelayModel *M = Fanins[i];

  if (M == NULL)
    return;

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;

    // Propagate the carry bit till the MSB of the result.
    for (unsigned j = std::max(LB, unsigned(AT->ToLB)), e = UB; j < e; ++j) {
      // Do not need the arrival time if the bit is known.
      if (Node->isBitKnownAt(j))
        continue;

      // Calculate the number of logic level from the lsb of the operand to
      // current bit.
      unsigned k = j - AT->ToLB;
      addArrival(AT->Src, AT->Arrival + Base + k * PerBit, j + 1, j);
    }
  }
}

void DelayModel::updateCmpArrivial() {
  APInt KnownBits = VASTBitMask(Node->getOperand(0)).getKnownBits() &
                    VASTBitMask(Node->getOperand(1)).getKnownBits();
  // Get the unknown bits.
  unsigned BitWidth = KnownBits.getBitWidth() - KnownBits.countPopulation();
  // Get the delay of comparison between the unknown bits.
  float Delay = LuaI::Get<VFUICmp>()->lookupLatency(std::min(BitWidth, 64u));
  // TODO: Caculate the number of logic levels.

  for (unsigned i = 0, e = Node->size(); i < e; ++i) {
    VASTValPtr V = Node->getOperand(i);

    // TODO: Consider the bitmask.
    if (VASTValue *Val = GetAsLeaf(V)) {
      addArrival(Val, Delay + GetLeafDelay(Val), 1, 0);
      continue;
    }

    DelayModel *M = Fanins[i];

    if (M == NULL)
      continue;

    for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
      ArrivalTime *AT = I;
      addArrival(AT->Src, AT->Arrival + Delay, 1, 0);
    }
  }
}

void DelayModel::updateShiftAmt() {
  VASTValPtr V = Node->getOperand(1);
  APInt KnwonBits = VASTBitMask(V).getKnownBits();

  // A shift is required if the any bit is unknown.
  // But wire delay is required by all bits except the known tailing/leading
  // bits. Also consider the wire delay due to duplication.

  // TODO: Consider the bitmask.
  if (VASTValue *Val = GetAsLeaf(V)) {
    unsigned LL = V->getBitWidth() - KnwonBits.countPopulation();

    addArrival(Val, LL * VFUs::LUTDelay + GetLeafDelay(Val), 1, 0);
    return;
  }

  DelayModel *M = Fanins[1];

  if (M == NULL)
    return;

  unsigned UB = V->getBitWidth() - KnwonBits.countLeadingOnes(),
           LB = KnwonBits.countTrailingOnes();

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    if (AT->ToLB > UB)
      continue;

    unsigned LL = UB - std::max(LB, unsigned(AT->ToLB));
    addArrival(AT->Src, AT->Arrival + LL * VFUs::LUTDelay, 1, 0);
  }
}

void DelayModel::updateShlArrival() {
  updateShiftAmt();

  VASTValPtr V = Node->getOperand(0);
  unsigned BitWidth = Node->getBitWidth();

  float Delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));

  // TODO: Consider the bitmask.
  if (VASTValue *Val = GetAsLeaf(V)) {
    addArrival(Val, Delay + GetLeafDelay(Val), BitWidth, 0);
    return;
  }

  DelayModel *M = Fanins[0];

  if (M == NULL)
    return;

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    addArrival(AT->Src, AT->Arrival + Delay, BitWidth, AT->ToLB);
  }
}

void DelayModel::updateShrArrival() {
  updateShiftAmt();

  VASTValPtr V = Node->getOperand(0);
  unsigned BitWidth = Node->getBitWidth();

  float Delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));

  // TODO: Consider the bitmask.
  if (VASTValue *Val = GetAsLeaf(V)) {
    addArrival(Val, Delay + GetLeafDelay(Val), BitWidth, 0);
    return;
  }

  DelayModel *M = Fanins[0];

  if (M == NULL)
    return;

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    addArrival(AT->Src, AT->Arrival + Delay, AT->ToUB, 0);
  }
}

void DelayModel::updateArrival() {
  VASTExpr::Opcode Opcode = Node->getOpcode();

  switch (Opcode) {
  case vast::VASTExpr::dpBitCat:
    return updateBitCatArrival();
  case vast::VASTExpr::dpBitRepeat:
    return updateBitRepeatArrival();
  case vast::VASTExpr::dpBitExtract:
    return updateBitExtractArrival();
  case vast::VASTExpr::dpBitMask:
    return updateBitMaskArrival();
  case vast::VASTExpr::dpAnd: {
    unsigned LogicLevels = LogCeiling(Node->size(), VFUs::MaxLutSize);
    return updateArrivalParallel(LogicLevels * VFUs::LUTDelay);
  }
  case vast::VASTExpr::dpRAnd:
    return updateReductionArrival();
  case vast::VASTExpr::dpAdd:
    return updateCarryChainArrival(LuaI::Get<VFUAddSub>());
  case vast::VASTExpr::dpMul:
    return updateCarryChainArrival(LuaI::Get<VFUMult>());
  case vast::VASTExpr::dpShl:
    return updateShlArrival();
  case vast::VASTExpr::dpAshr:
  case vast::VASTExpr::dpLshr:
    return updateShrArrival();
  case vast::VASTExpr::dpSGT:
  case vast::VASTExpr::dpUGT:
    return updateCmpArrivial();
  case vast::VASTExpr::dpLUT:
    return updateArrivalParallel(VFUs::LUTDelay);
  case vast::VASTExpr::dpROMLookUp:
    return updateROMLookUpArrival();
  case vast::VASTExpr::dpSAnn:
  case vast::VASTExpr::dpHAnn:
    return updateArrivalParallel(0.0f);
  default:
    llvm_unreachable("Unexpected opcode!");
    break;
  }
}

//===----------------------------------------------------------------------===//
DelayModel *TimingNetlist::createModel(VASTExpr *Expr) {
  DelayModel *&Model = ModelMap[Expr];
  assert(Model == NULL && "Model had already existed!");
  SmallVector<DelayModel*, 8> Fanins;

  // Fill the fanin list.
  for (unsigned i = 0; i < Expr->size(); ++i) {
    VASTExpr *ChildExpr = Expr->getOperand(i).getAsLValue<VASTExpr>();

    // There is no fanin delay model if we reach the leaf of a combinational
    // cone. The expressions with hard annotation are also considered as
    // a leaf.
    if (ChildExpr == NULL || ChildExpr->isHardAnnotation()) {
      Fanins.push_back(NULL);
      continue;
    }

    Fanins.push_back(lookUpDelayModel(ChildExpr));
  }

  Model = new DelayModel(Expr, Fanins);
  Models.push_back(Model);
  return Model;
}

void TimingNetlist::buildTimingNetlist(VASTValue *V) {
  VASTExpr *Root = dyn_cast<VASTExpr>(V);

  if (Root == NULL)
    return;

  if (ModelMap.count(Root))
    return;

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(Root, Root->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt &It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();

      // Caculate the arrivial time to the output of this node.
      DelayModel *M = createModel(Node);
      M->updateArrival();
      continue;
    }

    VASTValue *Child = It->getAsLValue<VASTValue>();
    ++It;

    if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(Child)) {
      if (!ModelMap.count(ChildExpr) && !ChildExpr->isHardAnnotation())
        VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));

      continue;
    }
  }
}

TimingNetlist::TimingNetlist() : VASTModulePass(ID) {
  initializeTimingNetlistPass(*PassRegistry::getPassRegistry());
}

char TimingNetlist::ID = 0;

INITIALIZE_AG_PASS_BEGIN(TimingNetlist, TimingAnalysis,
                         "shang-timing-netlist",
                         "Preform Timing Estimation on the RTL Netlist",
                         false, true, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
INITIALIZE_AG_PASS_END(TimingNetlist, TimingAnalysis,
                       "shang-timing-netlist",
                       "Preform Timing Estimation on the RTL Netlist",
                       false, true, true)

void TimingNetlist::releaseMemory() {
  ModelMap.clear();
  Models.clear();
}

void TimingNetlist::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.setPreservesAll();
}

//===----------------------------------------------------------------------===//

bool TimingNetlist::runOnVASTModule(VASTModule &VM) {
  // Build the timing path for datapath nodes.
  typedef DatapathContainer::expr_iterator expr_iterator;
  for (expr_iterator I = VM.expr_begin(), E = VM.expr_end(); I != E; ++I) {
    if (!I->use_empty())
      buildTimingNetlist(I);
  }

  DEBUG(dbgs() << "Timing Netlist: \n";
        print(dbgs()););

  return false;
}

void TimingNetlist::print(raw_ostream &OS) const {
}

TimingAnalysis::PhysicalDelay
TimingNetlist::getArrivalTimeImpl(VASTValue *To, VASTValue *From) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(To);

  if (Expr == NULL)
    return To == From ? PhysicalDelay(GetLeafDelay(From)) : None;

  DelayModel *M = lookUpDelayModel(Expr);
  PhysicalDelay Arrival = None;

  typedef DelayModel::const_arrival_iterator iterator;
  for (iterator I = M->arrival_begin(From); M->inRange(I, From); ++I)
    Arrival = std::max(Arrival, PhysicalDelay(I->Arrival));

  return Arrival;
}

TimingAnalysis::PhysicalDelay
TimingNetlist::getArrivalTimeImpl(VASTSelector *To, VASTValue *From) {
  PhysicalDelay FIArrival = getArrivalTimeImpl(To->getFanin().get(), From);
  PhysicalDelay GuardArrival = getArrivalTimeImpl(To->getGuard().get(), From);
  // Also consider the delay from the d pin of the register.
  // TODO: Consider wire delay based on the connections.
  return std::max(FIArrival + PhysicalDelay(VFUs::RegDelay),
                  GuardArrival + PhysicalDelay(VFUs::ClkEnDelay));
}

TimingAnalysis::PhysicalDelay
TimingNetlist::getArrivalTime(VASTValue *To, VASTValue *From) {
  return getArrivalTimeImpl(To, From);
}

TimingAnalysis::PhysicalDelay
TimingNetlist::getArrivalTime(VASTSelector *To, VASTSeqValue *From) {
  return getArrivalTimeImpl(To, From);
}

TimingAnalysis::PhysicalDelay TimingNetlist::getArrivalTime(VASTSelector *To, VASTExpr *Thu,
                                                            VASTSeqValue *From) {
  if (Thu == NULL)
    return getArrivalTimeImpl(To, From);

  return getArrivalTimeImpl(To, Thu) + getArrivalTimeImpl(Thu, From);
}

namespace {
struct ArrivalPrinter {
  raw_ostream &OS;
  TimingNetlist &TNL;
  VASTValue *Src;
  std::set<VASTExpr*> Visited;

  ArrivalPrinter(raw_ostream &OS, TimingNetlist &TNL, VASTValue *Src)
    : OS(OS), TNL(TNL), Src(Src) {}

  void printExpr(VASTExpr *Expr, unsigned Level) {
    DelayModel *M = TNL.lookUpDelayModel(Expr);

    if (!M->hasArrivalFrom(Src))
      return;

    OS.indent(Level) << "Expr: ";
    Expr->printName(OS);
    OS << ' ';
    Expr->printExpr(OS);
    OS << '\n';

    typedef DelayModel::const_arrival_iterator iterator;
    for (iterator I = M->arrival_begin(Src); M->inRange(I, Src); ++I) {
      const ArrivalTime *AT = I;
      OS.indent(Level + 2)
        << "Delay: [" << unsigned(AT->ToUB) << ':' << unsigned(AT->ToLB) << "] "
        << AT->Arrival * VFUs::Period << '\n';
    }
  }

  void print(VASTValue *Root) {
    VASTExpr *Expr = dyn_cast_or_null<VASTExpr>(Root);

    if (Expr == NULL)
      return;

    // The entire tree had been visited.
    if (!Visited.insert(Expr).second) return;

    typedef VASTOperandList::op_iterator ChildIt;
    std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;

    VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

    while (!VisitStack.empty()) {
      VASTExpr *Node = VisitStack.back().first;
      ChildIt It = VisitStack.back().second;

      // We have visited all children of current node.
      if (It == Node->op_end()) {
        VisitStack.pop_back();

        // Visit the current Node.
        printExpr(Node, VisitStack.size());

        continue;
      }

      // Otherwise, remember the node and visit its children first.
      VASTValue *ChildNode = It->unwrap().get();
      ++VisitStack.back().second;

      if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(ChildNode)) {
        // ChildNode has a name means we had already visited it.
        if (!Visited.insert(ChildExpr).second || ChildExpr->isHardAnnotation())
          continue;

        VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
      }
    }
    OS << '\n';
  }
};
}

void TimingNetlist::printArrivalPathImpl(raw_ostream &OS, VASTValue *To,
                                         VASTValue *From) {
  ArrivalPrinter(OS, *this, From).print(To);
}
