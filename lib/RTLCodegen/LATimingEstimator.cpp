//- LATimingEstimator.cpp-Estimate Delay with Linear Approximation -*- C++ -*-//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file datapath define the delay estimator based on linear approximation.
//
//===----------------------------------------------------------------------===//
#include "LATimingEstimator.h"

#define DEBUG_TYPE "timing-estimator"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
// Accumulating the delay according to the blackbox model.
// This is the baseline delay estimate model.
class BlackBoxTimingEstimator
  : public TimingEstimatorBase<BlackBoxTimingEstimator, double> {
  typedef double delay_type;
  typedef TimingEstimatorBase<BlackBoxTimingEstimator, double> Base;

  static SrcEntryTy AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                       const SrcEntryTy DelayFromSrc);

  static unsigned ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
    return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
  }

  template<unsigned ROWNUM>
  static SrcEntryTy AccumulateWithDelayTable(VASTValue *Dst, unsigned SrcPos,
                                             const SrcEntryTy DelayFromSrc);

  explicit BlackBoxTimingEstimator(TimingNetlist &Netlist) : Base(Netlist) {}

  friend void llvm::delay_estimation::estimateDelaysWithBB(TimingNetlist &Netlist);
public:
  void accumulateDelayThuLUT(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateLUTDelay);
  }

  void accumulateDelayThuAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateLUTDelay);
  }

  void accumulateDelayThuRAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<5>);
  }

  void accumulateDelayThuRXor(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<5>);
  }

  void accumulateDelayThuCmp(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<3>);
  }

  void accumulateDelayThuAdd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<0>);
  }

  void accumulateDelayThuMul(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<2>);
  }

  void accumulateDelayThuShl(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<1>);
  }

  void accumulateDelayThuSRL(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<1>);
  }

  void accumulateDelayThuSRA(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<1>);
  }

  void accumulateDelayThuSel(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<4>);
  }

  void accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRBitCat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                 SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayBitRepeat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }
};
}

BlackBoxTimingEstimator::SrcEntryTy
BlackBoxTimingEstimator::AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                            const SrcEntryTy DelayFromSrc) {
  return SrcEntryTy(DelayFromSrc.first, DelayFromSrc.second + 0.635);
}

template<unsigned ROWNUM>
BlackBoxTimingEstimator::SrcEntryTy
BlackBoxTimingEstimator::AccumulateWithDelayTable(VASTValue *Dst, unsigned SrcPos,
                                                  const SrcEntryTy DelayFromSrc)
{
  // Delay table in nanosecond.
  static delay_type DelayTable[][5] = {
    { 1.430 , 2.615 , 3.260 , 4.556 , 7.099 }, //Add 0
    { 1.191 , 3.338 , 4.415 , 5.150 , 6.428 }, //Shift 1
    { 1.195 , 4.237 , 4.661 , 9.519 , 12.616 }, //Mul 2
    { 1.191 , 2.612 , 3.253 , 4.531 , 7.083 }, //Cmp 3
    { 1.376 , 1.596 , 1.828 , 1.821 , 2.839 }, //Sel 4
    { 0.988 , 1.958 , 2.103 , 2.852 , 3.230 }  //Red 5
  };

  delay_type *CurTable = DelayTable[ROWNUM];

  unsigned BitWidth = Dst->getBitWidth();

  int i = ComputeOperandSizeInByteLog2Ceil(BitWidth);

  delay_type RoundUpLatency = CurTable[i + 1],
              RoundDownLatency = CurTable[i];
  unsigned SizeRoundUpToByteInBits = 8 << i;
  unsigned SizeRoundDownToByteInBits = i ? (8 << (i - 1)) : 0;
  delay_type PerBitLatency =
    RoundUpLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits) -
    RoundDownLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits);
  // Scale the latency according to the actually width.
  delay_type Delay =
    (RoundDownLatency + PerBitLatency * (BitWidth - SizeRoundDownToByteInBits));

  return SrcEntryTy(DelayFromSrc.first, DelayFromSrc.second + Delay);
}

namespace {
struct BitLevelDelay {
  unsigned MSB, LSB;
  unsigned MSBDelay, LSBDelay;

  operator double() const {
    return double(std::max(MSBDelay, LSBDelay)) * VFUs::LUTDelay;
  }
};

// Estimate the bit-level delay with linear approximation.
class BitlevelDelayEsitmator : public TimingEstimatorBase<BitlevelDelayEsitmator,
                                                          BitLevelDelay> {
  typedef TimingEstimatorBase<BitlevelDelayEsitmator, BitLevelDelay> Base;
  typedef BitLevelDelay delay_type;

  explicit BitlevelDelayEsitmator(TimingNetlist &Netlist) : Base(Netlist) {}
  friend void llvm::delay_estimation::esitmateDelayWithLA(TimingNetlist &Netlist);
public:

  void accumulateDelayThuLUT(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRXor(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuCmp(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAdd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuMul(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuShl(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSRL(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSRA(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSel(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRBitCat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                 SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayBitRepeat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }
};
}

namespace llvm {
  namespace delay_estimation {
    void estimateDelaysWithBB(TimingNetlist &Netlist) {
      BlackBoxTimingEstimator(Netlist).runTimingAnalysis();
    }

    void esitmateDelayWithLA(TimingNetlist &Netlist) {
      BitlevelDelayEsitmator(Netlist).runTimingAnalysis();
    }
  }
}
