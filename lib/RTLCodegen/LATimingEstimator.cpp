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

using namespace llvm;

namespace {
//===----------------------------------------------------------------------===//
// Accumulating the delay according to the blackbox model.
struct BlackBoxTimingEstimator
  : public TimingEstimatorBase<BlackBoxTimingEstimator, double> {
  typedef double delay_type;

  // For trivial expressions, the delay is zero.
  static SrcEntryTy AccumulateTrivialExprDelay(VASTValue *Dst, unsigned SrcPos,
                                               const SrcEntryTy DelayFromSrc) {
    return DelayFromSrc;
  }

  static SrcEntryTy AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                        const SrcEntryTy DelayFromSrc) {
    return SrcEntryTy(DelayFromSrc.first, DelayFromSrc.second + 0.635 / VFUs::Period);
  }

  static unsigned ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
    return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
  }

  template<unsigned ROWNUM>
  static SrcEntryTy AccumulateWithDelayTable(VASTValue *Dst, unsigned SrcPos,
                                              const SrcEntryTy DelayFromSrc) {
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

    // Translate delay to cycle.
    Delay /= VFUs::Period;

    return SrcEntryTy(DelayFromSrc.first, DelayFromSrc.second + Delay);
  }

  void accumulateExprDelay(VASTExpr *Expr) {
    SrcDelayInfo &CurSrcInfo = PathDelay[Expr];
    assert(CurSrcInfo.empty() && "We are visiting the same Expr twice?");

    typedef VASTExpr::op_iterator op_iterator;
    for (unsigned i = 0; i < Expr->NumOps; ++i) {
      VASTValPtr Operand = Expr->getOperand(i);
      switch (Expr->getOpcode()) {
      case VASTExpr::dpLUT:
      case VASTExpr::dpAnd:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo, AccumulateLUTDelay);
        break;
      case VASTExpr::dpRAnd:
      case VASTExpr::dpRXor:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo,
                            AccumulateWithDelayTable<5>);
        break;
      case VASTExpr::dpSCmp:
      case VASTExpr::dpUCmp:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo,
                            AccumulateWithDelayTable<3>);
        break;
      case VASTExpr::dpAdd:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo,
                            AccumulateWithDelayTable<0>);
        break;
      case VASTExpr::dpMul:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo,
                            AccumulateWithDelayTable<2>);
        break;
      case VASTExpr::dpShl:
      case VASTExpr::dpSRL:
      case VASTExpr::dpSRA:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo,
                            AccumulateWithDelayTable<1>);
        break;
      case VASTExpr::dpSel:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo,
                            AccumulateWithDelayTable<4>);
        break;
      case VASTExpr::dpAssign:
      case VASTExpr::dpBitCat:
      case VASTExpr::dpBitRepeat:
        accumulateDelayThu(Operand.get(), Expr, i, CurSrcInfo,
                            AccumulateTrivialExprDelay);
        break;
      default: llvm_unreachable("Unknown datapath opcode!"); break;
      }
    }  
  }

};

}
