//=--- TimingNetlist.cpp - The Netlist for Delay Estimation -------*- C++ -*-=//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface timing netlist.
//
//===----------------------------------------------------------------------===//

#include "TimingNetlist.h"
#include "DelayMatrix.h"

#include "vast/VASTMemoryBank.h"
#include "vast/VASTModule.h"
#include "vast/Passes.h"
#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-timing-netlist"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<enum TimingNetlist::ModelType>
TimingModel("timing-model", cl::Hidden,
            cl::desc("The Timing Model of the Delay Estimator"),
            cl::values(
  clEnumValN(TimingNetlist::External, "external",
            "Perform delay estimation with synthesis tool"),
  clEnumValN(TimingNetlist::BlackBox, "blackbox",
            "Only accumulate the critical path delay of each FU"),
  clEnumValN(TimingNetlist::ZeroDelay, "zero-delay",
            "Assume all datapath delay is zero"),
  clEnumValEnd));

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

void DelayModel::verifyConnectivity() const {
  std::set<VASTSeqValue*> Srcs;
  Node->extractSupportingSeqVal(Srcs);

  for (const_arrival_iterator I = arrival_begin(), E = arrival_end();
       I != E; ++I) {
    const ArrivalTime *AT = I;

    if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(AT->Src))
      Srcs.erase(SV);
  }

  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;
    const_cast<DelayModel*>(this)->addArrival(SV, 0.0f, Node->getBitWidth(), 0);
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
struct ConnectivityVerifier {
  DelayModel *M;
  ConnectivityVerifier(DelayModel *M) : M(M) {}
  ~ConnectivityVerifier() { M->verifyConnectivity(); }
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

void DelayModel::updateArrivalParallel(unsigned i, float Delay) {
  VASTValPtr V = Node->getOperand(i);

  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
    addArrival(SV, 0.0f, Node->getBitWidth(), 0);
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

  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
    addArrival(SV, 0.0f, ToUB, ToLB);
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

    if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
      addArrival(SV, 0.0f, OffSet + BitWidth, OffSet);
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

  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
    addArrival(SV, 0.0f, BitWidth, 0);
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
    addArrival(AT->Src, 0.0f, CurUB - LB, CurLB - LB);
  }
}

void DelayModel::updateBitMaskArrival() {
  // BitMask is a trivial operator.
  // TODO: Ignore the known bits?
  updateArrivalParallel(0.0f);
}

static unsigned log(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

void DelayModel::updateReductionArrival() {
  VASTValPtr V = Node->getOperand(0);
  unsigned NumBits = V->getBitWidth();
  // Only reduce the unknow bits.
  NumBits -= VASTBitMask(V).getNumKnownBits();
  unsigned LogicLevels = log(NumBits, VFUs::MaxLutSize);
  updateArrivalParallel(LogicLevels * VFUs::LUTDelay);
}

void DelayModel::updateROMLookUpArrival() {
  VASTValPtr Addr = Node->getOperand(0);
  unsigned NumBits = Addr->getBitWidth();
  // Only reduce the unknow bits.
  NumBits -= VASTBitMask(Addr).getNumKnownBits();
  unsigned LogicLevels = log(NumBits, VFUs::MaxLutSize);
  float delay = LogicLevels * VFUs::LUTDelay;
  unsigned BitWidth = Node->getBitWidth();
  updateArrivalCritial(delay, BitWidth, 0);
}

void DelayModel::updateArrivalCarryChain(unsigned i, float Base, float PerBit) {
  unsigned BitWidth = Node->getBitWidth();
  VASTValPtr V = Node->getOperand(i);

  // TODO: Consider the bitmask.
  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
    for (unsigned j = 0; j < BitWidth; ++j)
      addArrival(SV, Base + j * PerBit, j + 1, j);

    return;
  }

  DelayModel *M = Fanins[i];

  if (M == NULL)
    return;

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;

    // Propagate the carry bit till the MSB of the result.
    for (unsigned j = AT->ToLB, e = BitWidth; j < e; ++j) {
      // Calculate the number of logic level from the lsb of the operand to
      // current bit.
      unsigned k = j - AT->ToLB;
      addArrival(AT->Src, AT->Arrival + Base + k * PerBit, j + 1, j);
    }
  }
}

void DelayModel::updateCmpArrivial() {
  unsigned BitWidth = Node->getOperand(0)->getBitWidth();
  float Delay = LuaI::Get<VFUICmp>()->lookupLatency(std::min(BitWidth, 64u));
  float PerBit = Delay / BitWidth;

  for (unsigned i = 0, e = Node->size(); i < e; ++i) {
    VASTValPtr V = Node->getOperand(i);

    // TODO: Consider the bitmask.
    if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
      addArrival(SV, Delay, 1, 0);
      continue;
    }

    DelayModel *M = Fanins[i];

    if (M == NULL)
      continue;

    for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
      ArrivalTime *AT = I;
      unsigned Distance = BitWidth - AT->ToLB;
      float CurDelay = Distance * PerBit;
      addArrival(AT->Src, AT->Arrival + CurDelay, 1, 0);
    }
  }
}

void DelayModel::updateShiftAmt() {
  VASTValPtr V = Node->getOperand(1);
  unsigned LL = V->getBitWidth();

  // TODO: Consider the bitmask.
  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
    addArrival(SV, LL * VFUs::LUTDelay, 1, 0);
    return;
  }

  DelayModel *M = Fanins[1];

  if (M == NULL)
    return;

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    unsigned Distance = LL - AT->ToLB;
    addArrival(AT->Src, AT->Arrival + Distance * VFUs::LUTDelay, 1, 0);
  }
}

void DelayModel::updateShlArrival() {
  updateShiftAmt();

  VASTValPtr V = Node->getOperand(0);
  unsigned BitWidth = Node->getBitWidth();

  float Delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));

  // TODO: Consider the bitmask.
  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
    addArrival(SV, Delay, BitWidth, 0);
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
  if (VASTSeqValue *SV = V.getAsLValue<VASTSeqValue>()) {
    addArrival(SV, Delay, BitWidth, 0);
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
  ConnectivityVerifier CV(this);

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
    unsigned LogicLevels = log(Node->size(), VFUs::MaxLutSize);
    return updateArrivalParallel(LogicLevels * VFUs::LUTDelay);
  }
  case vast::VASTExpr::dpRAnd:
  case vast::VASTExpr::dpRXor:
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
  case vast::VASTExpr::dpKeep:
    return updateArrivalParallel(0.0f);
  default:
    llvm_unreachable("Unexpected opcode!");
    break;
  }
}

//===----------------------------------------------------------------------===//
float TimingNetlist::getDelay(VASTValue *Src, VASTSelector *Dst) const {
  return 0.0f;
}

float TimingNetlist::getDelay(VASTValue *Src, VASTValue *Dst) const {
  // TODO:
  //if (VASTSeqValue *SVal = dyn_cast<VASTSeqValue>(Dst)) {
  //  for each fanin fi of Dst,
  //    get the CRITICAL path delay from Src to fi
  //    max reduction

  //  return CRITICAL delay to all fanins + Mux delay?
  //}
  return 0.0f;
}

float TimingNetlist::getDelay(VASTValue *Src, VASTValue *Thu,
                              VASTSelector *Dst) const {
  if (Thu == 0) return getDelay(Src, Dst);

  float S2T = getDelay(Src, Thu), T2D = getDelay(Thu, Dst);
  return S2T + T2D;
}

DelayModel *TimingNetlist::createModel(VASTExpr *Expr) {
  DelayModel *&Model = ModelMap[Expr];
  assert(Model == NULL && "Model had already existed!");
  SmallVector<DelayModel*, 8> Fanins;

  // Fill the fanin list.
  for (unsigned i = 0; i < Expr->size(); ++i) {
    VASTExpr *ChildExpr = Expr->getOperand(i).getAsLValue<VASTExpr>();

    if (ChildExpr == NULL) {
      Fanins.push_back(NULL);
      continue;
    }

    std::map<VASTExpr*, DelayModel*>::iterator I = ModelMap.find(ChildExpr);
    assert(I != ModelMap.end() && "Model of childexpr cannot be found!");
    Fanins.push_back(I->second);
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
      if (!ModelMap.count(ChildExpr))
        VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));

      continue;
    }
  }
}

TimingNetlist::TimingNetlist() : VASTModulePass(ID) {
  initializeTimingNetlistPass(*PassRegistry::getPassRegistry());
}

char TimingNetlist::ID = 0;

INITIALIZE_PASS_BEGIN(TimingNetlist, "shang-timing-netlist",
                      "Preform Timing Estimation on the RTL Netlist",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(TimingNetlist, "shang-timing-netlist",
                    "Preform Timing Estimation on the RTL Netlist",
                    false, true)

Pass *vast::createTimingNetlistPass() {
  return new TimingNetlist();
}

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

void TimingNetlist::print(raw_ostream &OS) const {}
