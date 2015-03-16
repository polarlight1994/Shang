//--SIRTimingAnalysis.cpp - Abstract Interface for Timing Analysis -*- C++ -*-//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file data-path define the delay estimator based on linear approximation.
//
//===----------------------------------------------------------------------===//
#include "sir/SIRTimingAnalysis.h"
#include "sir/Passes.h"
#include "sir/SIR.h"
#include "sir/SIRPass.h"

#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-sir-timing-estimator"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;

static unsigned LogCeiling(unsigned x, unsigned n) {
	unsigned log2n = Log2_32_Ceil(n);
	return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

static bool IsLeafValue(SIR *SM, Value *V) {
	// When we visit the Srcs of value, the Leaf Value
	// means the top nodes of the Expr-Tree. There are 
	// three kinds of Leaf Value:
	// 1) Argument 2) Register 3) ConstantValue
	// The path between Leaf Value and other values 
	// will cost no delay (except wire delay).
	// However, since the ConstantValue will have
	// no impact on the scheduling process, so 
	// we will just ignore the ConstantInt in
	// previous step.

	assert(!isa<ConstantInt>(V) 
		     && "ConstantInt should be ignored in previous process!");

	if (isa<Argument>(V))	return true;

	if (Instruction *Inst = dyn_cast<Instruction>(V))
		if (SM->lookupSIRReg(Inst))
			return true;

	return false;
}

SIRDelayModel::SIRDelayModel(SIR *SM, DataLayout *TD, Instruction *Node,
                             ArrayRef<SIRDelayModel *> Fanins)
                             : SM(SM), TD(TD), Node(Node) {
  SIRDelayModel **Data = new SIRDelayModel *[Fanins.size()];
  std::uninitialized_copy(Fanins.begin(), Fanins.end(), Data);
  this->Fanins = ArrayRef<SIRDelayModel *>(Data, Fanins.size());
}

SIRDelayModel::~SIRDelayModel() {
  if (Fanins.data())
    delete[] Fanins.data();
}

ilist<ArrivalTime>::iterator
SIRDelayModel::findInsertPosition(ArrivalTime *Start, Value *V, uint8_t ToLB) {
  // Trivial Cases: just insert it in the beginning.
  if (Start == NULL)
    return arrival_begin();

  // Find the insert position according to the order of ToLB.
  arrival_iterator InsertBefore = Start;
  do {
    if (InsertBefore->ToLB > ToLB)
      break;

    ++InsertBefore;
  } while (inRange(InsertBefore, V));

  return InsertBefore;
}

void SIRDelayModel::addArrival(Value *V, float Arrival, uint8_t ToUB, uint8_t ToLB) {
  ArrivalTime *&Start = ArrivalStart[V];
  // Trivial Case: Simply add the new arrival time.
  if (Start == NULL) {
    Start = new ArrivalTime(V, Arrival, ToUB, ToLB);
    Arrivals.push_front(Start);
    return;
  }

  // If the Start is not NULL, then it means this Src Value
  // has been added to ArrivalStarts before. That is there
  // are two paths from Src Value to this model.
  ilist<ArrivalTime> BiggerArrivals;

  // Remove any range which are hidden by the current range.
  // The visit order is from the old Arrival to the end of ArrivalStarts.
  for (arrival_iterator I = Start; inRange(I, V);) {
    ArrivalTime *AT = I++;
    // Update start if it is removed. Then we can 
    // make sure that the Start is really the start
    // of all nodes which targets V, since during 
    // the process, we may create new Arrival 
    // which may be inserted before OldStart.
    if (Start == NULL)
      Start = AT;

    // |-  NewRange  -|
    //                  |-  AT  -|
    // Not overlapped, skip and iterate to see
    // what will happen to the next AT.
    if (AT->ToUB <= ToLB)
      continue;

    //            |-  NewRange  -|
    // |-  AT  -|
    // Out of range, so we do not need to iterate any more.
    if (AT->ToLB <= ToUB)
      break;

    // If OldArrival is bigger than NewArrival.
    // In this condition, we need to break the new arrival
    // into two parts. Only one of them can be inserted.
    if (AT->Arrival > Arrival) {
      //    |- NewRange -|
      // |-       AT       -|
      // The new range is completely hidden, nothing to do.
      if (AT->ToLB <= ToLB && AT->ToUB >= ToUB)
        return;

      // |-  NewRange -|
      //    |-     AT     -|
      if (AT->ToLB <= ToLB) {
        assert(AT->ToUB < ToUB && "Unexpected UB!");
        // The arrival time of [OldToUB, NewToUB) bit range
        // should be NewArrival and the arrival time of
        // [OldToLB, OldToUB) should still be OldArrival.
        // So we need to continue to iterate to capture
        // the [OldToUB, NewToUB) part.
        ToLB = AT->ToUB;
        continue;
      }

      //    |-  NewRange -|
      // |-     AT   -|
      if (AT->ToUB >= ToUB) {
        assert(AT->ToLB > ToLB && "Unexpected LB!");
        // The arrival time of [NewToLB, OldToLB) bit range
        // should be NewArrival and the arrival time of
        // [OldToLB, OldToUB) should still be OldArrival.
        // The [NewToLB, OldToLB) part has been captured
        // in last iterate, so we can break here.
        ToUB = AT->ToLB;
        break;
      }

      assert(AT->ToLB > ToLB && AT->ToUB < ToUB && "Unexpected range!");

      // |-    NewRange    -|
      //    |-     AT   -|
      if (AT->ToLB > ToLB && AT->ToUB < ToUB) {
        // In this condition, we'll remove the Start,
        // So we need to set the next Arrival as New Start,
        // that is why we set Start to NULL here.
        if (AT == Start)
          Start = NULL;

        // The bigger arrival is a subset of [ToLB, ToUB)
        // Temporary move the bigger arrivals to another list.
        Arrivals.remove(AT);
        BiggerArrivals.push_back(AT);
        continue;
      }        
    } else {
      // Then NewArrival is bigger than Old Arrival.
      // In this condition, we just need to modify the
      // ToLB and ToUB of old arrivals(this may influent 
      // more than one old arrivals) and insert a new
      // arrival iterator for new arrival.

      // |-    NewRange    -|
      //    |-     AT   -|
      // Completely hidden.
      if (AT->ToLB >= ToLB && AT->ToUB <= ToUB) {
        if (AT == Start)
          Start = NULL;

        Arrivals.erase(AT);
        continue;
      }

      //    |-  NewRange -|
      // |-     AT   -|
      if (AT->ToLB >= ToLB) {
        assert(AT->ToUB > ToUB && "Unexpected UB!");
        // The arrival time of [NewToLB, NewToUB) bit range
        // should be NewArrival and the arrival time of
        // [NewToUB, OldToUB) should still be OldArrival.
        AT->ToLB = ToUB;
        continue;
      }

      // |-  NewRange -|
      //    |-     AT     -|
      if (AT->ToUB <= ToUB) {
        assert(AT->ToLB < ToLB && "Unexpected LB!");
        // The arrival time of [NewToLB, NewToUB) bit range
        // should be NewArrival and the arrival time of
        // [OldToLB, NewToLB) should still be OldArrival.
        AT->ToUB = ToLB;
        continue;
      }

      assert(AT->ToLB < ToLB && AT->ToUB > ToUB && "Unexpected range of AT!");

      //    |- NewRange -|
      // |-       AT       -|
      if (AT->ToLB < ToLB && AT->ToUB > ToUB) {
        // In this condition, we'll change it into:
        //         |- NewRange -|
        // |-NewAT-|            |-AT-|
        uint8_t TempUB = AT->ToUB;
        AT->ToUB = ToLB;
        ArrivalTime *NewAT = new ArrivalTime(V, AT->Arrival, TempUB, ToUB);
        Arrivals.insertAfter(AT, NewAT);
        break;
      }
    }    
  }
  
  // Find the insert position of new arrival time.
  arrival_iterator InsertBefore = findInsertPosition(Start, V, ToLB);

  // Insert the arrival.
  do {
    uint8_t CurUB = ToUB;
    if (!BiggerArrivals.empty()) {
      CurUB = BiggerArrivals.front().ToLB;
      assert(CurUB <= ToUB && "Unexpected UB!");
    }

    if (CurUB > ToLB) {
      ArrivalTime *AT = new ArrivalTime(V, Arrival, CurUB, ToLB);
      Arrivals.insert(InsertBefore, AT);

      // After creating new Arrival, we should update Start.
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

void SIRDelayModel::updateBitCatArrival() {
	// OffSet is the whole BitWidth of the Value this Delay Model holds.
  unsigned OffSet = TD->getTypeSizeInBits(Node->getType());
  
	// Since the Value this Delay Model holds is a BitCat instruction,
	// the delay coming from Srcs will be divided into several parts.
	// /-input.a-/ /-input.b-/ /-input.c-/
	// ~~delay.a~~~~~delay.b~~~~~delay.c~~
	// /-------------output--------------/
	// Also note that the real numbers of operands should be minus 1.
  for (unsigned I = 0, E = Node->getNumOperands() - 1; I < E; ++I) {
    Value *V = Node->getOperand(I);
    unsigned BitWidth = TD->getTypeSizeInBits(V->getType());
    OffSet -= BitWidth;

		// If the operand is a ConstantInt, then ignore it because
		// it will have no impact on the scheduling process.
		if (isa<ConstantInt>(V)) continue;

		// Simply add the zero delay if the Src itself is a Leaf Value.
		if (IsLeafValue(SM, V)) {
			addArrival(V, 0.0f + 0.0f, OffSet + BitWidth, OffSet);
			continue;
		}

    SIRDelayModel *M = Fanins[I];
    if (M == NULL)
      continue;

    // Otherwise inherit the arrival information from the higher level.
    for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
      addArrival(I->Src, I->Arrival + 0.0f, I->ToUB + OffSet, I->ToLB + OffSet);
    }
  }
  
  assert(OffSet == 0 && "Bad Offset!");
}

void SIRDelayModel::updateBitExtractArrival() {
	unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());

	Value *V = Node->getOperand(0);

	// If the operand is a ConstantInt, then ignore it because
	// it will have no impact on the scheduling process.
	if (isa<ConstantInt>(V)) return;

	// Simply add the zero delay if the Src itself is a Leaf Value.
	if (IsLeafValue(SM, V)) {
		addArrival(V, 0.0f + 0.0f, BitWidth, 0);
		return;
	}

	SIRDelayModel *M = Fanins[0];
	if (M == NULL)
		return;

	uint8_t UB = getConstantIntValue(Node->getOperand(1));
	uint8_t LB = getConstantIntValue(Node->getOperand(2));

	// Since the Value this Delay Model holds is a BitExtract instruction,
	// the delay coming from V will inherit form the V's SrcVal.
	// /----input.a----/ /----input.b----/ /----input.c----/
	// ~~~~~delay.a~~~~~~~~~~~delay.b~~~~~~~~~~~delay.c~~~~~
	// /------------------------V--------------------------/ 
	//       /~delay.a~/ /~~~~delay.b~~~~//~delay.c~/
	//       /----------------output----------------/
	for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
		ArrivalTime *AT = I;
		unsigned CurLB = std::max(AT->ToLB, LB), CurUB = std::min(AT->ToUB, UB);

		if (CurLB >= CurUB)
			continue;

		// Transform the arrival bits.
		addArrival(AT->Src, AT->Arrival, CurUB - LB, CurLB - LB);
	}
}

void SIRDelayModel::updateBitRepeatArrival() {
	// Since the Value this Delay Model holds is a BitRepeat instruction,
	// the delay model fits the ArrivalCritial Model.
  updateArrivalCritial(0, 0.0f);
}

void SIRDelayModel::updateBitMaskArrival() {
  // Hack: Unfinished function
}

void SIRDelayModel::updateArrivalParallel(unsigned i, float Delay) {
  Value *V = Node->getOperand(i);

	// If the operand is a ConstantInt, then ignore it because
	// it will have no impact on the scheduling process.
	if (isa<ConstantInt>(V)) return;

	// Simply add the zero delay if the Src itself is a Leaf Value.
	if (IsLeafValue(SM, V)) {
		addArrival(V, Delay + 0.0f, TD->getTypeSizeInBits(Node->getType()), 0);
		return;
	}

  SIRDelayModel *M = Fanins[i];
  if (M == NULL)
    return;

  // Forward the arrival from M with delay without changing the output bit.
  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I)
    addArrival(I->Src, I->Arrival + Delay, I->ToUB, I->ToLB);
}

void SIRDelayModel::updateArrivalParallel(float delay) {
	// Also note that the real numbers of operands should be minus 1.
  for (unsigned I = 0, E = Node->getNumOperands() - 1; I < E; ++I)
    updateArrivalParallel(I, delay);
}

void SIRDelayModel::updateArrivalCritial(unsigned i, float Delay) {
  Value *V = Node->getOperand(i);
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());

	// If the operand is a ConstantInt, then ignore it because
	// it will have no impact on the scheduling process.
	if (isa<ConstantInt>(V)) return;

	// Simply add the zero delay if the Src itself is a Leaf Value.
	if (IsLeafValue(SM, V)) {
		addArrival(V, Delay + 0.0f, BitWidth, 0);
		return;
	}

  SIRDelayModel *M = Fanins[i];
  if (M == NULL)
    return;

  // Forward the arrival from M with delay.
  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I)
    addArrival(I->Src, I->Arrival + Delay, BitWidth, 0);
}

void SIRDelayModel::updateArrivalCritial(float delay) {
	// Also note that the real numbers of operands should be minus 1.
  for (unsigned I = 0, E = Node->getNumOperands() - 1; I < E; ++I)
    updateArrivalCritial(I, delay);
}

void SIRDelayModel::updateArrivalCarryChain(unsigned i, float Base,
	                                          float PerBit) {
	unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());
	Value *V = Node->getOperand(i);

	// If the operand is a ConstantInt, then ignore it because
	// it will have no impact on the scheduling process.
	if (isa<ConstantInt>(V)) return;

	// Simply add the zero delay if the Src itself is a Leaf Value.
	if (IsLeafValue(SM, V)) {
		addArrival(V, Base + BitWidth * PerBit + 0.0f, BitWidth, 0);
		return;
	}

	SIRDelayModel *M = Fanins[i];
	if (M == NULL)
		return;

	for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
		ArrivalTime *AT = I;
		addArrival(AT->Src, AT->Arrival + Base + BitWidth * PerBit,
			         BitWidth, AT->ToLB);
	}
}

void SIRDelayModel::updateReductionArrival() {
  Value *V = Node->getOperand(0);
  unsigned NumBits = TD->getTypeSizeInBits(V->getType());
  // Hack: we do not have BitMask now,so all bits are unknown bits.
  // Only reduce the unknown bits.
  //NumBits -= VASTBitMask(V).getNumKnownBits();
  unsigned LogicLevels = LogCeiling(NumBits, VFUs::MaxLutSize);
  updateArrivalParallel(LogicLevels * VFUs::LUTDelay);
}

void SIRDelayModel::updateROMLookUpArrival() {
  // Hack: unfinished function.
}

void SIRDelayModel::updateCmpArrivial() {
  // Hack: we do not have BitMask now,so all bits are unknown bits.
  // and we should calculate the number of logic levels in the future.
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());
  float Delay = LuaI::Get<VFUICmp>()->lookupLatency(std::min(BitWidth, 64u));

	// Also note that the real numbers of operands should be minus 1.
  for (unsigned I = 0, E = Node->getNumOperands() - 1; I < E; ++I) {
    Value *V = Node->getOperand(I);

		// If the operand is a ConstantInt, then ignore it because
		// it will have no impact on the scheduling process.
		if (isa<ConstantInt>(V)) continue;

		// Simply add the zero delay if the Src itself is a Leaf Value.
		if (IsLeafValue(SM, V)) {
			addArrival(V, Delay + 0.0f, 1, 0);
			continue;
		}

    SIRDelayModel *M = Fanins[I];
    if (M == NULL)
      continue;

    for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
      ArrivalTime *AT = I;
      addArrival(AT->Src, AT->Arrival + Delay, 1, 0);
    }
  }
}

void SIRDelayModel::updateShiftAmt() {
  Value *V = Node->getOperand(1);  

	// If the operand is a ConstantInt, then ignore it because
	// it will have no impact on the scheduling process.
	if (isa<ConstantInt>(V)) return;

	// Simply add the zero delay if the Src itself is a Leaf Value.
	if (IsLeafValue(SM, V)) {
		unsigned LL = TD->getTypeSizeInBits(V->getType());
		// Hack: the second 0.0f should be modified when the value
		// is MemBus.
		addArrival(V, LL * VFUs::LUTDelay + 0.0f, 1, 0);
		return;
	}

  SIRDelayModel *M = Fanins[1];
  if (M == NULL)
    return;

  // Hack: we do not have BitMask now,so all bits are unknown bits.
  unsigned UB = TD->getTypeSizeInBits(V->getType());
  unsigned LB = 0;
  
  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    if (AT->ToLB > UB)
      continue;

    unsigned LL = UB - std::max(LB, unsigned(AT->ToLB));
    addArrival(AT->Src, AT->Arrival + LL * VFUs::LUTDelay, 1, 0);
  }
}

void SIRDelayModel::updateShlArrival() {
  // Update the arrival time from the second operand.
  updateShiftAmt();

  Value *V = Node->getOperand(0);
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());

  float Delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));

	// If the operand is a ConstantInt, then ignore it because
	// it will have no impact on the scheduling process.
	if (isa<ConstantInt>(V)) return;

	// Simply add the zero delay if the Src itself is a Leaf Value.
	if (IsLeafValue(SM, V)) {
		addArrival(V, Delay + 0.0f, BitWidth, 0);
		return;
	}

  SIRDelayModel *M = Fanins[0];
  if (M == NULL)
    return;

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    addArrival(AT->Src, AT->Arrival + Delay, BitWidth, AT->ToLB);
  }
}

void SIRDelayModel::updateShrArrival() {
  // Update the arrival time from the second operand.
  updateShiftAmt();

  Value *V = Node->getOperand(0);
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());

  float Delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));

	// If the operand is a ConstantInt, then ignore it because
	// it will have no impact on the scheduling process.
	if (isa<ConstantInt>(V)) return;

	// Simply add the zero delay if the Src itself is a Leaf Value.
	if (IsLeafValue(SM, V)) {
		addArrival(V, Delay + 0.0f, BitWidth, 0);
		return;
	}

  SIRDelayModel *M = Fanins[0];
  if (M == NULL)
    return;

  for (arrival_iterator I = M->arrival_begin(); I != M->arrival_end(); ++I) {
    ArrivalTime *AT = I;
    addArrival(AT->Src, AT->Arrival + Delay, AT->ToUB, 0);
  }
}

void SIRDelayModel::updateArrival() {
  // If the Node is PHI instruction, then it is
	// associated with a register. So the delay is
	// also 0.0f.
	if (isa<PHINode>(Node))
		return updateArrivalParallel(0.0f);

	// Since all data-path instruction in SIR
	// is Intrinsic Inst. So the opcode of 
	// data-path instruction is its InstrisicID.
  IntrinsicInst *I = dyn_cast<IntrinsicInst>(Node);
	assert(I && "Unexpected non-IntrinsicInst!");

	Intrinsic::ID ID = I->getIntrinsicID();

  switch (ID) {
  case Intrinsic::shang_bit_cat:
    return updateBitCatArrival();
  case Intrinsic::shang_bit_repeat:
    return updateBitRepeatArrival();
  case Intrinsic::shang_bit_extract:
    return updateBitExtractArrival();

	case Intrinsic::shang_not:
		return updateArrivalParallel(0.0f);
  case Intrinsic::shang_and: {
		// To be noted that, in LLVM IR the return value
		// is counted in Operands, so the real numbers
		// of operands should be minus one.
		unsigned IONums = Node->getNumOperands() - 1;
    unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
    return updateArrivalParallel(LogicLevels * VFUs::LUTDelay);
  }
  case Intrinsic::shang_rand: {
		unsigned IONums = TD->getTypeSizeInBits(Node->getOperand(0)->getType());
    unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
		return updateArrivalParallel(LogicLevels * VFUs::LUTDelay);
	}

  case Intrinsic::shang_add:
	case Intrinsic::shang_addc:
    return updateCarryChainArrival(LuaI::Get<VFUAddSub>());
  case Intrinsic::shang_mul:
    return updateCarryChainArrival(LuaI::Get<VFUMult>());

  case Intrinsic::shang_shl:
    return updateShlArrival();
  case Intrinsic::shang_ashr:
  case Intrinsic::shang_lshr:
    return updateShrArrival();
  case Intrinsic::shang_sgt:
  case Intrinsic::shang_ugt:
    return updateCmpArrivial();

	case  Intrinsic::shang_pseudo:
		// To be noted that, pseudo instruction is created
		// to represent the assignment to SlotReg, so it
		// will devote 0 delay.
		return updateArrivalParallel(0.0f);

  default:
    llvm_unreachable("Unexpected opcode!");
    break;
  }
}

SIRDelayModel *SIRTimingAnalysis::createModel(Instruction *Inst, SIR *SM, DataLayout &TD) {
  SIRDelayModel *&Model = ModelMap[Inst];
  assert(Model == NULL && "Model had already existed!");
  SmallVector<SIRDelayModel *, 8> Fanins;

  // Fill the Fanin list by visit all operands and operands of
  // operands. And remember that the real number of operands
	// should be minus 1.
  for (unsigned i = 0; i < Inst->getNumOperands() - 1; ++i) {
    Instruction *ChildInst = dyn_cast<Instruction>(Inst->getOperand(i));

		// If we reach the leaf of this data-path, then we get no Delay Model.
		if (!ChildInst) {
			Fanins.push_back(NULL);
			continue;
		}

    Fanins.push_back(lookUpDelayModel(ChildInst));
  }

  Model = new SIRDelayModel(SM, &TD, Inst, Fanins);
  Models.push_back(Model);
  return Model;
}

SIRDelayModel *SIRTimingAnalysis::lookUpDelayModel(Instruction *Inst) const {
  std::map<Instruction *, SIRDelayModel *>::const_iterator I = ModelMap.find(Inst);
  assert(I != ModelMap.end() && "Model of Inst cannot be found!");
  return I->second;
}

void SIRTimingAnalysis::buildTimingNetlist(Value *V, SIR *SM, DataLayout &TD) {
  Instruction *Root = dyn_cast<Instruction>(V);

  if (!Root)
    return;

  if (ModelMap.count(Root))
    return;

  typedef Instruction::op_iterator ChildIt;
  std::vector<std::pair<Instruction *, ChildIt>> VisitStack;

  VisitStack.push_back(std::make_pair(Root, Root->op_begin()));

  while (!VisitStack.empty()) {
    Instruction *Node = VisitStack.back().first;
    ChildIt &It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();

      // Calculate the arrival time to the output of this node.
      SIRDelayModel *M = createModel(Node, SM, TD);
      M->updateArrival();
      continue;
    }

    Value *ChildNode = *It;
    ++It;

    if (Instruction *ChildInst = dyn_cast<Instruction>(ChildNode)) {
      if (!ModelMap.count(ChildInst))
        VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));

      continue;
    }
  }
}

SIRTimingAnalysis::PhysicalDelay SIRTimingAnalysis::getArrivalTime(Value *To,
                                                                   Value *From) {
  // Hack: The From here must be a SIRSeqVal.
  Instruction *Inst = dyn_cast<Instruction>(To);

  assert(Inst && "This should be a instruction!");

  SIRDelayModel *M = lookUpDelayModel(Inst);
  PhysicalDelay Arrival = None;

  // Visit all Arrivals which has the Src Value as From and get the biggest Arrival.
  typedef SIRDelayModel::const_arrival_iterator iterator;
  for (iterator I = M->arrival_begin(From); M->inRange(I, From); ++I)
    Arrival = std::max(Arrival, PhysicalDelay(I->Arrival));

  return Arrival;
}

SIRTimingAnalysis::PhysicalDelay SIRTimingAnalysis::getArrivalTime(SIRRegister *To,
                                                                   Value *From) {
  // Hack: The From here must be a SIRSeqVal.
  PhysicalDelay FIArrival = getArrivalTime(To->getRegVal(), From);
  PhysicalDelay GuardArrival = getArrivalTime(To->getRegGuard(), From);
  // Also consider the delay from the d pin of the register.
  // TODO: Consider wire delay based on the connections.
  // Hack: Need to copy the VFUs from vast to SIR
  return std::max(FIArrival + PhysicalDelay(VFUs::RegDelay),
                  GuardArrival + PhysicalDelay(VFUs::ClkEnDelay));
}

SIRTimingAnalysis::PhysicalDelay SIRTimingAnalysis::getArrivalTime(SIRRegister *To,
                                                                   Value *Thu,
                                                                   Value *From) {
  // Hack: The From and Thu here must be a SIRSeqVal.
  if (Thu == NULL)
    return getArrivalTime(To, From);

  return getArrivalTime(To, Thu) + getArrivalTime(Thu, From);
}

void SIRTimingAnalysis::extractArrivals(SIR *SM, SIRSeqOp *Op, ArrivalMap &Arrivals) {
	SIRRegister *DstReg = Op->getDst();

	// Considering two data-path coming to the Op: 1) SrcVal; 2) Guard.
	Value *SrcVal = Op->getSrc();
	Value *Guard = Op->getGuard();

	SmallVector<Value *, 4> Srcs;
	Srcs.push_back(SrcVal);
	Srcs.push_back(Guard);

	for (int i = 0; i < Srcs.size(); i++) {
		Value *V = Srcs[i];

		// If the operand is a ConstantInt, then ignore it because
		// it will have no impact on the scheduling process.
		if (isa<ConstantInt>(V)) continue;

		// Simply add the zero delay if the Src itself is a Leaf Value.
		if (IsLeafValue(SM, V)) {
			Arrivals.insert(std::make_pair(V, PhysicalDelay(0.0f)));
			continue;;			
		}

		// Then collect all the SeqVals when we traverse the data-path
		// reversely, so to be noted that all the Values down here must
	  // be SeqVal.
		typedef std::set<Value *> LeafSet;
		LeafSet Leaves;

		std::set<Value *> Visited;
		Instruction *Inst = dyn_cast<Instruction>(V);
		assert(Inst && "Unexpected Value type!");

		typedef Instruction::op_iterator ChildIt;
		std::vector<std::pair<Instruction *, ChildIt>> VisitStack;

		VisitStack.push_back(std::make_pair(Inst, Inst->op_begin()));

		while (!VisitStack.empty()) {
			Instruction *Node = VisitStack.back().first;
			ChildIt It = VisitStack.back().second;

			// We have visited all children of current node.
			if (It == Node->op_begin()) {
				VisitStack.pop_back();
				continue;
			}

			// Otherwise, remember the node and visit its children first.
			Value *ChildNode = *It;
			++VisitStack.back().second;

			if (Instruction *ChildInst = dyn_cast<Instruction>(ChildNode)) {
				// ChildInst has a name means we had already visited it.
				if (!Visited.insert(ChildInst).second) continue;

				VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
				continue;
			}

			// Also ignore the ConstantInt.
			if (isa<ConstantInt>(ChildNode)) continue;

			// The Leaf Value is what we want to find.
			if (IsLeafValue(SM, ChildNode)) {
				Leaves.insert(ChildNode);
				continue;
			}
		}

		if (Leaves.empty())	return;

		SmallVector<Value *, 4> MissedLeaves;

		typedef LeafSet::iterator iterator;
		for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
			Value *Leaf = *I;

			// Compute the delay from Leaf to Dst Reg.
			PhysicalDelay Delay = getArrivalTime(DstReg, Leaf);

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

		// Handle the missed leaves.
		// Hack : Need to handle the missed leaves.
		assert(MissedLeaves.empty() && "This function not finished yet!");
	}
}

void SIRTimingAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
	SIRPass::getAnalysisUsage(AU);
  AU.addRequired<DataLayout>();
  AU.addRequiredID(SIRRegisterSynthesisForAnnotationID);
  AU.setPreservesAll();
}

char SIRTimingAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(SIRTimingAnalysis,
                      "SIR-timing-analysis",
                      "Implement the timing analysis for SIR",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForAnnotation)
INITIALIZE_PASS_END(SIRTimingAnalysis,
                    "SIR-timing-analysis",
                    "Implement the timing analysis for SIR",
                    false, true)

bool SIRTimingAnalysis::runOnSIR(SIR &SM) {
  DataLayout &TD = getAnalysis<DataLayout>();

  // Build the timing path for data-path nodes.
  typedef SIR::datapathinst_iterator inst_iterator;
  for (inst_iterator I = SM.datapathinst_begin(), E = SM.datapathinst_end();
       I != E; ++I) {
    buildTimingNetlist(*I, &SM, TD);
  }

  return false;
}