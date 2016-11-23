//===----------------- SIR.cpp - Modules in SIR ----------------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the SIR.
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "llvm/IR/Value.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

static unsigned DFGNodeIdx = 1;

void SIRRegister::addAssignment(Value *Fanin, Value *FaninGuard) {
  Fanins.push_back(Fanin);
  FaninGuards.push_back(FaninGuard);
  assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");
}

void SIRRegister::addAssignment(Value *Fanin, Value *FaninGuard, SIRSlot *S) {
  Fanins.push_back(Fanin);
  FaninGuards.push_back(FaninGuard);
  FaninSlots.push_back(S);
  assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");
  assert(Fanins.size() == FaninSlots.size() && "Size not compatible!");
}

void SIRRegister::printDecl(raw_ostream &OS) const {
  OS << "reg" << BitRange(getBitWidth(), 0, false);
  OS << " " << Mangle(getName());
  // Set the IniteVal.
  OS << " = " << buildLiteralUnsigned(InitVal, getBitWidth())
     << ";\n";
}

void SIRRegister::printVirtualPortDecl(raw_ostream &OS, bool IsInput) const {
  OS << ",\n (* altera_attribute = \"-name VIRTUAL_PIN on\" *)";
  OS.indent(4);

  if (IsInput)
    OS << "input ";
  else
    OS << "output ";

  // If it is a output, then it should be a register.
  if (!IsInput)
    OS << "reg";
  else
    OS << "wire";

  if (getBitWidth() > 1) OS << "[" << utostr_32(getBitWidth() - 1) << ":0]";

  OS << " " << Mangle(getName());
}

void SIRPort::printDecl(raw_ostream &OS) const {
  if (isInput())
    OS << "input ";
  else
    OS << "output ";

  // If it is a output, then it should be a register.
  if (!isInput())
    OS << "reg";
  else
    OS << "wire";

  if (getBitWidth() > 1) OS << "[" << utostr_32(getBitWidth() - 1) << ":0]";

  OS << " " << Mangle(getName());
}

void DataFlowGraph::toposortCone(DFGNode *Root, std::set<DFGNode *> &Visited) {
  assert(Visited.insert(Root).second && "Unexpected insert failure!");

  /// Start from root, sort all its previous nodes according to the dependencies.

  typedef DFGNode::iterator iterator;
  std::vector<std::pair<DFGNode *, iterator> > WorkStack;
  WorkStack.push_back(std::make_pair(Root, Root->parent_begin()));

  while (!WorkStack.empty()) {
    DFGNode *CurNode = WorkStack.back().first;
    iterator &It = WorkStack.back().second;

    // Sort the node if all its previous nodes have been visited.
    if (It == CurNode->parent_end()) {
      WorkStack.pop_back();
      DFGNodeList.splice(DFGNodeList.end(), DFGNodeList, CurNode);

      continue;
    }

    DFGNode *ParentNode = *It;
    ++It;

    if (ParentNode->getType() == DFGNode::Entry)
      continue;

    if (!Visited.insert(ParentNode).second)
      continue;

    WorkStack.push_back(std::make_pair(ParentNode, ParentNode->parent_begin()));
  }
}

void DataFlowGraph::topologicalSortNodes() {
  DFGNode *Entry = getEntry(), *Exit = getExit();

  // Sort the Entry first.
  DFGNodeList.splice(DFGNodeList.end(), DFGNodeList, Entry);

  /// Use a BFS to visit all nodes and sort the node list
  /// by taking the visited node as root.

  std::set<DFGNode *> Visited;

  // BFS visit stack.
  std::list<DFGNode *> VisitStack;
  typedef DFGNode::iterator iterator;
  VisitStack.insert(VisitStack.end(), Entry);

  // Start the BFS.
  while (!VisitStack.empty()) {
    DFGNode *CurNode = *VisitStack.begin();

    for (iterator I = CurNode->child_begin(), E = CurNode->child_end();
         I != E; ++I) {
      DFGNode *ChildNode = *I;

      // If already visited, ignore it.
      if (Visited.count(ChildNode))
        continue;

      // Ignore the Exit.
      if (ChildNode->getType() == DFGNode::Exit)
        continue;

      toposortCone(ChildNode, Visited);

      VisitStack.insert(VisitStack.end(), ChildNode);
      Visited.insert(ChildNode);
    }

    VisitStack.pop_front();
  }

  // Sort the Exit last.
  DFGNodeList.splice(DFGNodeList.end(), DFGNodeList, Exit);
}

DFGNode *DataFlowGraph::createDFGNode(std::string Name, Value *Val, BasicBlock *BB,
                                      DFGNode::NodeType Ty, unsigned BitWidth) {
  assert(DFGNodeIdx < UINT_MAX && "Out of range!");

  // Create the DFG node.
  DFGNode *Node;
  if (Ty == DFGNode::Add || Ty == DFGNode::Mul || Ty == DFGNode::And ||
      Ty == DFGNode::Not || Ty == DFGNode::Or || Ty == DFGNode::Xor ||
      Ty == DFGNode::RAnd || Ty == DFGNode::EQ || Ty == DFGNode::NE ||
      Ty == DFGNode::TypeConversion || Ty == DFGNode::CompressorTree) {
    assert(BB && "Unexpected parent basic block!");

    Node = new CommutativeDFGNode(DFGNodeIdx++, Name, Val, BB, Ty, BitWidth);
  }
  else if (Ty == DFGNode::Div || Ty == DFGNode::LShr || Ty == DFGNode::AShr ||
           Ty == DFGNode::Shl || Ty == DFGNode::GT || Ty == DFGNode::LT ||
           Ty == DFGNode::BitExtract || Ty == DFGNode::BitCat ||
           Ty == DFGNode::BitRepeat || Ty == DFGNode::Register) {
    assert(BB && "Unexpected parent basic block!");

    Node = new NonCommutativeDFGNode(DFGNodeIdx++, Name, Val, BB, Ty, BitWidth);
  }
  else
    Node = new DFGNode(DFGNodeIdx++, Name, Val, BB, Ty, BitWidth);

  // Index the node.
  DFGNodeList.insert(DFGNodeList.back(), Node);
  if (Val)
    SM->indexDFGNodeOfVal(Val, Node);

  return Node;
}

DFGNode *DataFlowGraph::createDataPathNode(Instruction *Inst, unsigned BitWidth) {
  // Basic information of current value.
  std::string Name = Inst->getName();

  // The node we want to create.
  DFGNode *Node;

  // Identify the type of instruction.
  DFGNode::NodeType Ty = DFGNode::InValid;
  if (const IntrinsicInst *InstII = dyn_cast<IntrinsicInst>(Inst)) {
    Intrinsic::ID ID = InstII->getIntrinsicID();

    switch (ID) {
    // CommutativeDFGNodes
    case llvm::Intrinsic::shang_add:
    case llvm::Intrinsic::shang_addc:
      Ty = DFGNode::Add;
      break;
    case llvm::Intrinsic::shang_and:
      Ty = DFGNode::And;
      break;
    case llvm::Intrinsic::shang_ashr:
      Ty = DFGNode::AShr;
      break;
    case llvm::Intrinsic::shang_bit_cat:
      Ty = DFGNode::BitCat;
      break;
    case llvm::Intrinsic::shang_bit_extract:
      Ty = DFGNode::BitExtract;
      break;
    case llvm::Intrinsic::shang_bit_repeat:
      Ty = DFGNode::BitRepeat;
      break;
    case llvm::Intrinsic::shang_compressor:
      Ty = DFGNode::CompressorTree;
      break;
    case llvm::Intrinsic::shang_eq:
      Ty = DFGNode::EQ;
      break;
    case llvm::Intrinsic::shang_logic_operations:
      Ty = DFGNode::LogicOperationChain;
      break;
    case llvm::Intrinsic::shang_lshr:
      Ty = DFGNode::LShr;
      break;
    case llvm::Intrinsic::shang_mul:
      Ty = DFGNode::Mul;
      break;
    case llvm::Intrinsic::shang_ne:
      Ty = DFGNode::NE;
      break;
    case llvm::Intrinsic::shang_not:
      Ty = DFGNode::Not;
      break;
    case llvm::Intrinsic::shang_or:
      Ty = DFGNode::Or;
      break;
    case llvm::Intrinsic::shang_rand:
      Ty = DFGNode::RAnd;
      break;
    case llvm::Intrinsic::shang_reg_assign:
      Ty = DFGNode::Register;
      break;
    case llvm::Intrinsic::shang_shl:
      Ty = DFGNode::Shl;
      break;
    case llvm::Intrinsic::shang_sdiv:
      Ty = DFGNode::Div;
      break;
    case llvm::Intrinsic::shang_udiv:
      Ty = DFGNode::Div;
      break;
    case llvm::Intrinsic::shang_sgt:
      Ty = DFGNode::GT;
      break;
    case llvm::Intrinsic::shang_ugt:
      Ty = DFGNode::GT;
      break;
    case llvm::Intrinsic::shang_xor:
      Ty = DFGNode::Xor;
      break;
    default:
      llvm_unreachable("Unexpected instruction type!");
      break;
    }

    Node = createDFGNode(Name, Inst, Inst->getParent(), Ty, BitWidth);
  }
  else if (isa<IntToPtrInst>(Inst) || isa<PtrToIntInst>(Inst) || isa<BitCastInst>(Inst)) {
    Node = createDFGNode(Name, Inst, Inst->getParent(), DFGNode::TypeConversion, BitWidth);
  }
  else if (isa<ReturnInst>(Inst)) {
    Node = createDFGNode(Name, Inst, Inst->getParent(), DFGNode::Ret, BitWidth);
  }
  else {
    llvm_unreachable("Unexpected instruction type!");
  }

  return Node;
}

DFGNode *DataFlowGraph::createConstantIntNode(uint64_t Val, unsigned BitWidth) {
  assert(BitWidth <= 64 && "Out of range!");

  std::string Name = utostr(Val);
  DFGNode *Node = new ConstantIntDFGNode(DFGNodeIdx++, Name, Val, BitWidth);

  // Index the node.
  DFGNodeList.insert(DFGNodeList.back(), Node);

  return Node;
}

DFGNode *DataFlowGraph::createConstantIntNode(ConstantInt *CI, unsigned BitWidth) {
  assert(BitWidth <= 64 && "Out of range!");

  uint64_t Val = CI->getZExtValue();
  return createConstantIntNode(Val, BitWidth);
}

DFGNode *DataFlowGraph::createGlobalValueNode(GlobalValue *GV, unsigned BitWidth) {
  // Basic information of current value.
  std::string Name = GV->getName();

  DFGNode *Node = createDFGNode(Name, GV, NULL, DFGNode::GlobalVal, BitWidth);

  return Node;
}

DFGNode *DataFlowGraph::createUndefValueNode(UndefValue *UV, unsigned BitWidth) {
  // Basic information of current value.
  std::string Name = UV->getName();

  DFGNode *Node = new DFGNode(DFGNodeIdx++, Name, UV, NULL, DFGNode::UndefVal, BitWidth);

  // Index the node.
  DFGNodeList.insert(DFGNodeList.back(), Node);

  return Node;
}

DFGNode *DataFlowGraph::createArgumentNode(Argument *Arg, unsigned BitWidth) {
  // Basic information of current value.
  std::string Name = Arg->getName();

  DFGNode *Node = createDFGNode(Name, Arg, NULL, DFGNode::Argument, BitWidth);

  return Node;
}

void DataFlowGraph::createDependency(DFGNode *From, DFGNode *To, unsigned Idx) {
  // Handle the non-commutative node specially since it need to mark the index
  // of each parent node.
  if (NonCommutativeDFGNode *NCNode = dyn_cast<NonCommutativeDFGNode>(To)) {
    NCNode->addParentNode(From, Idx);
  }
  else if (CommutativeDFGNode *CTo = dyn_cast<CommutativeDFGNode>(To)){
    CTo->addParentNode(From, 1);
  }
  else {
    assert(From->isEntryOrExit() || To->isEntryOrExit() ||
           To->getType() == DFGNode::LogicOperationChain ||
           To->getType() == DFGNode::Ret &&
           "Unexpected type!");
    To->addParentNode(From);
  }

  assert(From->hasChildNode(To) && "Fail to create dependency!");
  assert(To->hasParentNode(From) && "Fail to create dependency!");
}

void SIRSubModuleBase::addFanin(SIRRegister *Fanin) {
  Fanins.push_back(Fanin);
}

void SIRSubModuleBase::addFanout(SIRRegister *Fanout) {
  Fanouts.push_back(Fanout);
}

std::string SIRSubModule::getPortName(const Twine &PortName) const {
  return "SubMod" + utostr(getNum()) + "_" + PortName.str();
}

void SIRSubModule::printDecl(raw_ostream &OS) const {
  if (SIRRegister *Start = getStartPort())
    Start->printDecl(OS);
  if (SIRRegister *Fin = getFinPort())
    Fin->printDecl(OS);
  if (SIRRegister *Ret = getRetPort())
    Ret->printDecl(OS);
}

SIRMemoryBank::SIRMemoryBank(unsigned BusNum, unsigned AddrSize, unsigned DataSize,
                             bool RequireByteEnable, bool IsReadOnly, unsigned ReadLatency)
  : SIRSubModuleBase(MemoryBank, BusNum), AddrSize(AddrSize), DataSize(DataSize),
  RequireByteEnable(RequireByteEnable), IsReadOnly(IsReadOnly), ReadLatency(ReadLatency),
  EndByteAddr(0) {}

unsigned SIRMemoryBank::getByteAddrWidth() const {
  assert(requireByteEnable() && "Unexpected non-RequiredByteEnable memory bus!");

  // To access the memory bus in byte size, the address width need to increase
  // by log2(DataWidth).
  unsigned ByteAddrWidth = Log2_32_Ceil(getDataWidth() / 8);
  assert(ByteAddrWidth && "Unexpected NULL ByteAddrWidth!");

  return ByteAddrWidth;
}

void SIRMemoryBank::addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes) {
  DEBUG(dbgs() << "Insert the GV [" << GV->getName()
    << "] to offset [" << EndByteAddr << "\n");

  // Hack: I think if GV->getAlignment < (DataSiz / 8), then we can align the
  // EndByteAddr to the (DataSize / 8).
  assert(GV->getAlignment() >= (DataSize / 8) && "Bad GV alignment!");
  assert(EndByteAddr % (DataSize / 8) == 0 && "Bad Current Offset!");

  /// Insert the GV to the offset map, and calculate its offset in the byte address.
  // Roundup the address to align to the GV alignment.
  EndByteAddr = RoundUpToAlignment(EndByteAddr, GV->getAlignment());

  DEBUG(dbgs() << "Roundup EndByteAddr to [" << EndByteAddr
               << "] according to alignment " << GV->getAlignment() << '\n');

  // Insert the GV at the address of EndByteAddr.
  bool inserted = BaseAddrs.insert(std::make_pair(GV, EndByteAddr)).second;
  assert(inserted && "GV had already been added before!");

  DEBUG(dbgs() << "Insert the GV with size [" << SizeInBytes
               << "] and Offset increase to " << (EndByteAddr + SizeInBytes) << "\n");

  // Round up the address again to align to the (DataSize / 8).
  EndByteAddr = RoundUpToAlignment(EndByteAddr + SizeInBytes, DataSize / 8);

  DEBUG(dbgs() << "Roundup to Word address " << EndByteAddr << "\n");
}

unsigned SIRMemoryBank::getOffset(GlobalVariable *GV) const {
  std::map<GlobalVariable *, unsigned>::const_iterator at = BaseAddrs.find(GV);
  assert(at != BaseAddrs.end() && "GV is not inserted to offset map!");

  return at->second;
}

bool SIRMemoryBank::indexGV2OriginalPtrSize(GlobalVariable *GV, unsigned PtrSize) {
  return GVs2PtrSize.insert(std::make_pair(GV, PtrSize)).second;
}

SIRRegister *SIRMemoryBank::getAddr() const {
  return getFanin(0);
}

SIRRegister *SIRMemoryBank::getWData() const {
  return getFanin(1);
}

SIRRegister *SIRMemoryBank::getRData() const {
  return getFanout(0);
}

SIRRegister *SIRMemoryBank::getEnable() const {
  return getFanin(2);
}

SIRRegister *SIRMemoryBank::getWriteEn() const {
  return getFanin(3);
}

SIRRegister *SIRMemoryBank::getByteEn() const {
  return getFanin(4);
}

std::string SIRMemoryBank::getAddrName() const {
  return "mem" + utostr(Idx) + "addr";
}

std::string SIRMemoryBank::getRDataName() const {
  return "mem" + utostr(Idx) + "rdata";
}

std::string SIRMemoryBank::getWDataName() const {
  return "mem" + utostr(Idx) + "wdata";
}

std::string SIRMemoryBank::getEnableName() const {
  return "mem" + utostr(Idx) + "en";
}

std::string SIRMemoryBank::getWriteEnName() const {
  return "mem" + utostr(Idx) + "wen";
}

std::string SIRMemoryBank::getByteEnName() const {
  return "mem" + utostr(Idx) + "byteen";
}

std::string SIRMemoryBank::getArrayName() const {
  return "mem" + utostr(Idx) + "ram";
}

void SIRMemoryBank::printDecl(raw_ostream &OS) const {
  typedef std::map<GlobalVariable*, unsigned>::const_iterator iterator;
  for (iterator I = const_baseaddrs_begin(), E = const_baseaddrs_end();
    I != E; ++I) {
      unsigned PtrSize = lookupPtrSize(I->first);
      OS << "reg" << BitRange(PtrSize, 0, false);
      OS << " " << Mangle(I->first->getName());
      // Print the initial offset.
      OS << " = " << buildLiteralUnsigned(I->second, PtrSize)
         << ";\n";
  }

  getAddr()->printDecl(OS.indent(2));
  getRData()->printDecl(OS.indent(2));
  getWData()->printDecl(OS.indent(2));
  getEnable()->printDecl(OS.indent(2));
  getWriteEn()->printDecl(OS.indent(2));

  if (requireByteEnable())
    getByteEn()->printDecl(OS.indent(2));
}

void SIRMemoryBank::printVirtualPortDecl(raw_ostream &OS) const {
  getEnable()->printVirtualPortDecl(OS, false);
  getWriteEn()->printVirtualPortDecl(OS, false);
  getAddr()->printVirtualPortDecl(OS, false);
  getRData()->printVirtualPortDecl(OS, true);
  getWData()->printVirtualPortDecl(OS, false);

  if (requireByteEnable())
    getByteEn()->printVirtualPortDecl(OS, false);
}

void SIRSeqOp::print(raw_ostream &OS) const {
  OS << "Assign the Src Value " << "[" <<Src->getName()
     << "]" << " to register " << "[" << DstReg->getName()
     << "]" << " in Slot #" << S->getSlotNum()
     << " Scheduled to " << S->getStepInLocalBB()
     << " in local BB"<< "\n";
}

void SIRSeqOp::dump() const {
  print(dbgs());
  dbgs() << "\n";
}

bool SIRSlot::hasNextSlot(SIRSlot *NextSlot) {
  for (const_succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I)
    if (NextSlot == EdgePtr(*I))
      return true;

  return false;
}

void SIRSlot::addSuccSlot(SIRSlot *NextSlot, EdgeType T, Value *Cnd) {
  assert(T <= 3 && "Unexpected distance!");
  // Do not add the same successor slot twice.
  if (hasNextSlot(NextSlot)) return;

  // Connect the slots.
  NextSlot->PredSlots.push_back(EdgePtr(this, T, Cnd));
  NextSlots.push_back(EdgePtr(NextSlot, T, Cnd));
}

void SIRSlot::unlinkSucc(SIRSlot *S) {
  for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
    if (S != *I)
      continue;

    // Remove the PredEdge in DstSlot.
    pred_iterator at = std::find(S->PredSlots.begin(), S->PredSlots.end(), this);
    S->PredSlots.erase(at);

    // Remove the SuccEdge in SrcSlot.
    NextSlots.erase(I);
    return;
  }

  llvm_unreachable("Not the sucessor of this slot!");
}

void SIRSlot::unlinkSuccs() {
  for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
    SIRSlot *SuccSlot = *I;

    // Find the this slot in the PredSlot of the successor and erase it.
    pred_iterator at
      = std::find(SuccSlot->PredSlots.begin(), SuccSlot->PredSlots.end(), this);
    SuccSlot->PredSlots.erase(at);
  }

  NextSlots.clear();
}

void SIRSlot::resetNum(unsigned Num) {
  // Reset the SlotNum;
  SlotNum = Num;

  // Also reset the name of SlotReg.
  SlotReg->setRegName("Slot" + utostr_32(Num) + "r");
}

void SIRSlot::print(raw_ostream &OS) const {
  OS << "Slot #" << SlotNum << " Scheduled to " << Step
     << " in local BB Guarding by "
     << getGuardValue()->getName() << "\n";
}

void SIRSlot::dump() const {
  print(dbgs());
  dbgs() << "\n";
}

bool SIR::gcImpl() {
  // Remove all the instructions that is not be used anymore.
  Function *F = getFunction();

  // Visit all BasicBlock in Function to delete all useless instruction.
  typedef Function::iterator iterator;
  for (iterator BBI = F->begin(), BBE = F->end(); BBI != BBE; ++BBI) {
    BasicBlock *BB = BBI;
    typedef BasicBlock::iterator iterator;
    for (iterator InstI = BB->begin(), InstE = BB->end(); InstI != InstE; ++InstI) {
      Instruction *Inst = InstI;

      if (Inst->use_empty() && !isa<ReturnInst>(Inst)) {
        if (hasKeepVal(Inst))
          continue;

        // If the Inst is a reg_assign instruction, then it is a SeqVal.
        // We also need to delete the corresponding register.
        if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst))
          if (II->getIntrinsicID() == Intrinsic::shang_reg_assign) {
            SIRRegister *Reg = lookupSIRReg(II);

            // Ignore the MemoryBank register and Output register of Module.
            if (Reg->isFUInOut() || Reg->isOutPort())
              continue;

            bool hasReg = false;
            for (register_iterator RI = registers_begin(), RE = registers_end(); RI != RE; ++RI) {
              SIRRegister *reg = RI;

              if (reg == Reg)
                hasReg = true;
            }

            if (hasReg)
              Registers.remove(Reg);
          }

        Inst->eraseFromParent();
        return true;
      }
    }
  }

  return false;
}

bool SIR::gc() {
  bool changed = false;

  // Iteratively release the dead objects.
  while (gcImpl())
    changed = true;

  return changed;
}

void SIR::print(raw_ostream &OS) {
  // Print all information about SIR.
  OS << "======================Note=====================\n" << "All INFO of SIR will be printed by default!\n";
  OS << "If you want to view only part of it, please select following dump methods:\n";
  OS << "(1) IRInfo: dumpIR(); (2) BB2SlotInfo: dumpBB2Slot();\n";
  OS << "(3) Reg2SlotInfo: dumpReg2Slot()\n";
  OS << "===============================================\n";

  //dumpIR(OS);
  dumpBB2Slot(OS);
  //dumpReg2Slot(OS);
  //dumpSeqOp2Slot(OS);

  OS << "======================Note=====================\n" << "All INFO of SIR will be printed by default!\n";
  OS << "If you want to view only part of it, please select following dump methods:\n";
  OS << "(1) IRInfo: dumpIR(); (2) BB2SlotInfo: dumpBB2Slot();\n";
  OS << "(3) Reg2SlotInfo: dumpReg2Slot()\n";
  OS << "===============================================\n";
}

void SIR::dump(raw_ostream &OS) {
  print(OS);
  OS << "\n";
}

void SIR::dumpIR(raw_ostream &OS) {
  Function *F = this->getFunction();

  F->print(OS);
}

void SIR::dumpBB2Slot(raw_ostream &OS) {
  Function *F = this->getFunction();

  OS << "\n";

  // Print the entry slot.
  OS << "Entry Slot#0\n";

  typedef Function::iterator iterator;
  for (iterator I = F->begin(), E = F->end(); I != E; I++) {
    BasicBlock *BB = I;

    // Get the corresponding landing slot and latest slot.
    SIRSlot *LandingSlot = getLandingSlot(BB);
    SIRSlot *LatestSlot = getLatestSlot(BB);

    OS << "BasicBlock [" << BB->getName() << "] with landing slot "
      << "Slot#" << LandingSlot->getSlotNum() << " and latest slot "
      << "Slot#" << LatestSlot->getSlotNum() << "\n";
  }

  OS << "\n";
}

void SIR::dumpReg2Slot(raw_ostream &OS) {
  OS << "\n";

  for (register_iterator I = registers_begin(), E = registers_end(); I != E; I++) {
    SIRRegister *Reg = I;

    // Get the corresponding slot.
    SIRSlot *Slot = lookupSIRSlot(Reg);

    OS << "Register [" << Reg->getName() << "] in slot "
      << "Slot#" << Slot->getSlotNum() << "\n";
  }

  OS << "\n";
}

void SIR::dumpSeqOp2Slot(raw_ostream &OS) {
  OS << "\n";

  typedef seqop_iterator iterator;
  for (iterator I = seqop_begin(), E = seqop_end(); I != E; I++) {
    SIRSeqOp *SeqOp = I;

    // Get the corresponding slot.
    SIRSlot *Slot = SeqOp->getSlot();

    OS << "Assign Value [" << SeqOp->getSrc()->getName() << "] to register ["
       << SeqOp->getDst()->getName() << "] in slot Slot#" << Slot->getSlotNum()
       << "\n";
  }
}