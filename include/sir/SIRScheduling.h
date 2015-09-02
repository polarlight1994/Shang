//===-------   SIRScheduling.h - Scheduling Graph on SIR  ------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the SIRScheduling and SIRSchedulingEmitter. With these class 
// we perform scheduling in High-level Synthesis and re-implement the
// state-transition graph according to the scheduling results. It also re-time
// the data-path if necessary.
//
//===----------------------------------------------------------------------===//
#ifndef SIR_SCHEDULING_H
#define SIR_SCHEDULING_H

#include "SIR.h"
#include "SIRBuild.h"
#include "SIRPass.h"
#include "SIRTimingAnalysis.h"
#include "SIRSchedGraph.h"
#include "SIRListScheduler.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Dominators.h"
#include <map>
#include <set>

namespace llvm {
class SIRScheduling : public SIRPass {
private:
	SIR *SM;
	SIRSchedGraph *G;
	SIRTimingAnalysis *TA;
	AliasAnalysis *AA;
	DominatorTree *DT;
	DataLayout *TD;

	inline bool isCall(Instruction *Inst) const {
		CallInst *CI = dyn_cast<CallInst>(Inst);

		if (CI == 0) return false;

		// Since all the Shang instructions are IntrinsicInst,
		// we should handle them specially.
		if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(CI)) {
			switch (II->getIntrinsicID()) {
			default: break;
			case Intrinsic::shang_add:
			case Intrinsic::shang_lshr:
			case Intrinsic::shang_ashr:
			case Intrinsic::shang_shl:
			case Intrinsic::shang_bit_cat:
			case Intrinsic::shang_bit_extract:
			case Intrinsic::shang_bit_repeat:
			case Intrinsic::shang_mul:
			case Intrinsic::shang_sdiv:
			case Intrinsic::shang_udiv:
			case Intrinsic::shang_and:
			case Intrinsic::shang_not:
			case Intrinsic::shang_rand:
			case Intrinsic::shang_rxor:
			case Intrinsic::shang_sgt:
			case Intrinsic::shang_ugt:
			case Intrinsic::shang_pseudo:
				return false;
			}
		}

		return true;
	}
	inline bool isLoadStore(Instruction *Inst) const {
		return isa<LoadInst>(Inst) || isa<StoreInst>(Inst);
	}

	AliasAnalysis::Location
	getPointerLocation(Instruction *I, AliasAnalysis *AA) {
		if (LoadInst *LI = dyn_cast<LoadInst>(I))
			return AA->getLocation(LI);

		if (StoreInst *SI = dyn_cast<StoreInst>(I))
			return AA->getLocation(SI);

		llvm_unreachable("Unexpected instruction type!");
		return AliasAnalysis::Location();
	}

	bool isNoAlias(Instruction *Src, Instruction *Dst,
		             AliasAnalysis *AA) {
		return AA->isNoAlias(getPointerLocation(Src, AA),
			                   getPointerLocation(Dst, AA));
	}

	SIRSchedUnit *getOrCreateBBEntry(BasicBlock *BB);

	void constraintTerminators(BasicBlock *BB);

	void buildDependencies();
	void buildDataDependencies(SIRSchedUnit *U);
	void buildControlDependencies(SIRSchedUnit *U);
	void buildMemoryDependency(Instruction *SrcInst, Instruction *DstInst);
	void buildLocalMemoryDependencies(BasicBlock *BB);

	ArrayRef<SIRSchedUnit *> getDataFlowSU(Value *V);

	void buildSchedulingUnits(SIRSlot *S);

	void finishBuildingSchedGraph();

	void buildSchedulingGraph();

	void schedule();

	void emitSchedule();

public:
	static char ID;
	SIRScheduling();

	void getAnalysisUsage(AnalysisUsage &AU) const;

	bool runOnSIR(SIR &SM);
};

class SIRScheduleEmitter {
private:
	DataLayout &TD;
	SIR *SM;  
	SIRSchedGraph &G;
	SIRDatapathBuilder D_Builder;
	SIRCtrlRgnBuilder C_Builder;
	SmallVector<SIRSlot *, 8> OldSlots;  

public:
	SIRScheduleEmitter(DataLayout &TD, SIR *SM, SIRSchedGraph &G)
		: TD(TD), SM(SM), G(G), D_Builder(SM, TD), C_Builder(SM, TD) {};
	~SIRScheduleEmitter() {
		// Clear up the SIRSeqOp in the old slots.
		OldSlots.clear();
	}

	unsigned getBitWidth(Value *U) { return D_Builder.getBitWidth(U); }

	void insertSlotBefore(SIRSlot *S, SIRSlot *DstS, SIRSlot::EdgeType T, Value *Cnd);
	void emitSUsInBB(MutableArrayRef<SIRSchedUnit *> SUs);
	void emitSchedule();
};
}

#endif