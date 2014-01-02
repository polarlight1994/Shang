//=----- TimingNetlist.h - The Netlist for Delay Estimation -------*- C++ -*-=//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the timing netlist, which enable performing delay estimation
// on the RTL netlist. Please note that with this interface, we can perform
// timing estimation/analysis before and after scheduling/FU binding.
//
//===----------------------------------------------------------------------===//
#ifndef VAST_TIMING_NETLIST_H
#define VAST_TIMING_NETLIST_H

#include "vast/FUInfo.h"
#include "vast/VASTModulePass.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"

#include <map>

namespace vast {
using namespace llvm;

class VASTSelector;
class VASTValue;

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
  bool inRange(ilist<ArrivalTime>::const_iterator I, VASTValue *V) const {
    if (I == Arrivals.end())
      return false;

    return I->Src == V;
  }

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
  void verifyConnectivity() const;

  typedef ilist<ArrivalTime>::iterator arrival_iterator;
  arrival_iterator arrival_begin() { return Arrivals.begin(); }
  arrival_iterator arrival_end() { return Arrivals.end(); }
  typedef ilist<ArrivalTime>::const_iterator const_arrival_iterator;
  const_arrival_iterator arrival_begin() const { return Arrivals.begin(); }
  const_arrival_iterator arrival_end() const { return Arrivals.end(); }
};

/// Timinging Netlist - Annotate the timing information to the RTL netlist.
class TimingNetlist : public VASTModulePass {
public:
// The bit-level delay model.
  ilist<DelayModel> Models;
  std::map<VASTExpr*, DelayModel*> ModelMap;

  DelayModel *createModel(VASTExpr *Expr);

  void buildTimingNetlist(VASTValue *V);
public: 
  enum ModelType {
    ZeroDelay, BlackBox, Bitlevel, External
  };

  static char ID;

  TimingNetlist();

  /// Get the delay between nodes in the timing netlist.
  ///
  float getDelay(VASTValue *Src, VASTSelector *Dst) const;
  float getDelay(VASTValue *Src, VASTValue *Dst) const;
  float getDelay(VASTValue *Src, VASTValue *Thu, VASTSelector *Dst) const;


  virtual void releaseMemory();
  virtual bool runOnVASTModule(VASTModule &VM);
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  void print(raw_ostream &OS) const;
};
}

#endif
