//=- ExternalTimingAnalysis.cpp-Timing Analysis By External Tools -*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface to enable timing analysis by external tools.
// The interface first generates the Verilog for the datapath of the design,
// and then start the QuartusII to perform timing analysis on the datapath.
// Once the timing analysis finished, a timing analysis results extraction
// script is run to write the results in JSON format. The JSON file contains an
// array including the delay for all possible input-output pair of the datapath.
// Specifically, each delay record in the array is in the following format:
//   {"from":<src-reg>,"to":<dst-reg>,"delay":<delay-in-nanosecond>}
//
//===----------------------------------------------------------------------===//

#include "ExternalTimingAnalysis.h"

#include "llvm/ADT/SetOperations.h"
#include "llvm/Support/PathV1.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/MemoryBuffer.h"
#define DEBUG_TYPE "external-timing-analysis"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace yaml;

namespace {

class TimingEstimator {
protected:
  typedef double delay_type;
  typedef std::map<VASTValue*, delay_type> SrcDelayInfo;
  typedef SrcDelayInfo::value_type SrcEntryTy;
  typedef std::map<VASTValue*, SrcDelayInfo> PathDelayInfo;
  PathDelayInfo PathDelay;

  SrcDelayInfo *getPathTo(VASTValue *Dst) {
    PathDelayInfo::iterator at = PathDelay.find(Dst);
    return at == PathDelay.end() ? 0 : &at->second;
  }

  const SrcDelayInfo *getPathTo(VASTValue *Dst) const {
    PathDelayInfo::const_iterator at = PathDelay.find(Dst);
    return at == PathDelay.end() ? 0 : &at->second;
  }

  delay_type getDelayFrom(VASTValue *Src, const SrcDelayInfo &SrcInfo) const {
    SrcDelayInfo::const_iterator at = SrcInfo.find(Src);
    return at == SrcInfo.end() ? delay_type(0) : at->second;
  }

  const TimingNetlist &Netlist;
public:
  explicit TimingEstimator(const TimingNetlist &Netlist) : Netlist(Netlist) {}

  void updateDelay(SrcDelayInfo &Info, SrcEntryTy NewValue) {
    delay_type &OldDelay = Info[NewValue.first];
    OldDelay = std::max(OldDelay, NewValue.second);
  }

  // Take DelayAccumulatorTy to accumulate the design.
  // The signature of DelayAccumulatorTy should be:
  // SrcEntryTy DelayAccumulatorTy(VASTValue *Dst, unsign SrcPos,
  //                               SrcEntryTy DelayFromSrc)
  template<typename DelayAccumulatorTy>
  void accumulateDelayThu(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                          SrcDelayInfo &CurInfo, DelayAccumulatorTy F) {
    const SrcDelayInfo *SrcInfo = getPathTo(Thu);
    if (SrcInfo == 0) {
      assert(!isa<VASTExpr>(Thu) && "Not SrcInfo from Src find!");
      updateDelay(CurInfo, F(Dst, ThuPos, SrcEntryTy(Thu, delay_type())));
      return;
    }

    typedef SrcDelayInfo::const_iterator src_iterator;
    for (src_iterator I = SrcInfo->begin(), E = SrcInfo->end(); I != E; ++I)
      updateDelay(CurInfo, F(Dst, ThuPos, *I));

    // FIXME: Also add the delay from Src to Dst.
  }

  void analysisTimingOnTree(VASTWire *W);
  void accumulateExprDelay(VASTExpr *E);

  void buildDatapathDelayMatrix() {
    typedef TimingNetlist::FanoutIterator it;

    for (it I = Netlist.fanout_begin(), E = Netlist.fanout_end(); I != E; ++I)
      analysisTimingOnTree(I->second);
  }

  delay_type getPathDelay(VASTValue *From, VASTValue *To) {
    const SrcDelayInfo *SrcInfo = getPathTo(To);
    assert(SrcInfo && "SrcInfo not available!");
    return getDelayFrom(From, *SrcInfo);
  }

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
};
}

void TimingEstimator::accumulateExprDelay(VASTExpr *Expr) {
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

void TimingEstimator::analysisTimingOnTree(VASTWire *W) {
  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(W, VASTValue::dp_dep_begin(W)));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == VASTValue::dp_dep_end(Node)) {
      VisitStack.pop_back();

      // Accumulate the delay of the current node from all the source.
      if (VASTExpr *E = dyn_cast<VASTExpr>(Node))
        accumulateExprDelay(E);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->getAsLValue<VASTValue>();
    ++VisitStack.back().second;

    // We had already build the delay information to this node.
    if (getPathTo(ChildNode)) continue;

    // Ignore the leaf nodes.
    if (!isa<VASTWire>(ChildNode) && !isa<VASTExpr>(ChildNode)) continue;

    VisitStack.push_back(std::make_pair(ChildNode,
                                        VASTValue::dp_dep_begin(ChildNode)));
  }
}

static sys::Path buildPath(const Twine &Name, const Twine &Ext) {
  std::string ErrMsg;
  sys::Path Filename = sys::Path::GetTemporaryDirectory(&ErrMsg);
  if (Filename.isEmpty()) {
    errs() << "Error: " << ErrMsg << "\n";
    return Filename;
  }

  Filename.appendComponent((Name + Ext).str());
  if (Filename.makeUnique(true, &ErrMsg)) {
    errs() << "Error: " << ErrMsg << "\n";
    return sys::Path();
  }

  return Filename;
}

raw_ostream &printAsLHS(raw_ostream &O, const VASTNamedValue *V,
                        unsigned UB, unsigned LB) {
  O << V->getName() << VASTValue::printBitRange(UB, LB, V->getBitWidth() > 1);

  return O;
}

template<bool OUTPUTPART>
static const VASTNamedValue *printScanChainLogic(raw_ostream &O,
                                                 const VASTNamedValue *V,
                                                 const VASTValue *LastV,
                                                 unsigned Indent) {
  unsigned Bitwidth = V->getBitWidth();
  bool MultiBits = Bitwidth > 1;

  if (OUTPUTPART) {
    O.indent(Indent) << "if (read_netlist) \n";
    O.indent(Indent + 2) << V->getName() << " <= " << V->getName() << "w;\n";
    O.indent(Indent) << "else begin\n";
    // Increase the indent in the esle block.
    Indent += 2;
  }

  // Connect the last register to current register.
  printAsLHS(O.indent(Indent), V, Bitwidth, Bitwidth - 1) << " <= ";

  if (LastV)
    LastV->printAsOperand(O, 1, 0, false);
  else // We are in the beginning of the chain.
    O << "scan_chain_in";

  O << ";\n";

  // Shift the register.
  if (MultiBits) {
    printAsLHS(O.indent(Indent), V, Bitwidth - 1, 0) << " <= ";
    V->printAsOperand(O, Bitwidth, 1, false);    
    O << ";\n";
  }

  // Finish the else block if necessary.
  if (OUTPUTPART) O.indent(Indent - 2) << "end\n";

  O << '\n';

  return V;
}

void ExternalTimingAnalysis::writeNetlistWrapper(raw_ostream &O,
                                                 const Twine &Name) const {
  // FIXME: Use the luascript template?
  O << "module " << Name << "wapper(\n";
  O.indent(2) << "input wire clk,\n";
  O.indent(2) << "input wire read_netlist,\n";
  O.indent(2) << "input wire scan_chain_in,\n";
  O.indent(2) << "output reg scan_chain_out);\n";

  // Declare the registers for the scan chain.
  for (FaninIterator I = TNL.fanin_begin(), E = TNL.fanin_end(); I != E; ++I) {
    const VASTMachineOperand *MO = I->second;
    O.indent(4) << "reg";
    unsigned Bitwidth = MO->getBitWidth();

    if (Bitwidth > 1) O << "[" << (Bitwidth - 1) << ":0]";

    O << ' ' << MO->getName() << ";\n";
  }

  for (FanoutIterator I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I) {
    // The wire connecting the output of the netlist and the scan chain.
    O.indent(4) << "wire";

    unsigned Bitwidth = I->second->getBitWidth();
    if (Bitwidth > 1) O << "[" << (Bitwidth - 1) << ":0]";

    O << ' ' << I->second->getName() << "w;\n";

    // The registers in the scan chain.
    O.indent(4) << "reg";

    if (Bitwidth > 1) O << "[" << (Bitwidth - 1) << ":0]";

    O << ' ' << I->second->getName() << ";\n";
  }

  O.indent(4) << "// Scan chain timing\n";

  // Build the scan chain.
  const VASTValue *LastReg = 0;
  O.indent(4) << "always @(posedge clk) begin\n";
  for (FaninIterator I = TNL.fanin_begin(), E = TNL.fanin_end(); I != E; ++I)
    LastReg = printScanChainLogic<false>(O, I->second, LastReg, 6);

  for (FanoutIterator I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I)
    LastReg = printScanChainLogic<true>(O, I->second, LastReg, 6);

  // Write the output.
  O.indent(6) << "scan_chain_out <= ";

  if (LastReg) LastReg->printAsOperand(O, 1, 0, false);
  else         O << "scan_chain_in";

  O << ";\n";

  // Close the timing block.
  O.indent(4) << "end\n";

  O.indent(4) << '\n';

  // Instantiate the netlist.
  O.indent(4) << Name << ' ' << Name << "_inst(\n";

  for (FaninIterator I = TNL.fanin_begin(), E = TNL.fanin_end(); I != E; ++I)
    O.indent(6) << '.' << I->second->getName()
                << '(' << I->second->getName() << "),\n";

  for (FanoutIterator I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I)
    O.indent(6) << '.' << I->second->getName()
                << '(' << I->second->getName() << "w),\n";

  O.indent(6) << ".dummy_" << Name << "_output(0));\n\n";

  O << "endmodule\n";
}

void
ExternalTimingAnalysis::writeProjectScript(raw_ostream &O, const Twine &Name,
                                           const sys::Path &NetlistPath,
                                           const sys::Path &TimingExtractionScript)
                                           const {
  O << "load_package flow\n"
       "load_package report\n"
    << "project_new " << Name << " -overwrite\n"
    << "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE EP4CE75F29C6\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << Name << "wapper\n"
       "set_global_assignment -name SOURCE_FILE \""<< NetlistPath.str() <<"\"\n"
       //"set_global_assignment -name SDC_FILE @SDC_FILE@\n"
       "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
       "export_assignments\n"
       // Start the processes.
       "execute_module -tool map\n"
       "execute_module -tool fit\n"
       "execute_module -tool sta -args {--report_script \""
       << TimingExtractionScript.str() << "\"}\n"
       "project_close\n";
}

static void setTerminatorCollection(raw_ostream & O, const VASTValue *V,
                                    const char *CollectionName) {
  O << "set " << CollectionName << " [" << "get_keepers \"*"
    << cast<VASTNamedValue>(V)->getName() << "*\"]\n";
}

void ExternalTimingAnalysis::extractTimingForPair(raw_ostream &O, unsigned DstReg,
                                                  const VASTValue *Src) const {
  if (VASTValue *Dst = dyn_cast<VASTWire>(TNL.getDstPtr(DstReg)))
    extractTimingForPair(O, Dst, DstReg, Src);
}

void ExternalTimingAnalysis::extractTimingForPair(raw_ostream &O,
                                                  const VASTValue *Dst,
                                                  unsigned DstReg,
                                                  const VASTValue *Src) const {
  // Get the source and destination nodes.
  setTerminatorCollection(O, Dst, "dst");
  setTerminatorCollection(O, Src, "src");

  O <<
    "set delay -1\n"
    // Only extract the delay from source to destination when these node are
    // not optimized.
    "if {[get_collection_size $src] && [get_collection_size $dst]} {\n"
    "  foreach_in_collection path [get_timing_paths -from $src -to $dst -setup"
    "                              -npath 1 -detail path_only] {\n"
    "    set delay [get_path_info $path -data_delay]\n"
    "    post_message -type info \"" << intptr_t(Src) << " -> " << DstReg
    << " delay: $delay\"\n"
    "  }\n"
    "} else {\n"
    "    post_message -type warning \"" << intptr_t(Src) << " -> " << DstReg
    << " path not found!\""
    "}\n"
    "puts $JSONFile \"\\{\\\"from\\\":" << intptr_t(Src) << ",\\\"to\\\":"
    << DstReg << ",\\\"delay\\\":$delay\\},\"\n";
}

void ExternalTimingAnalysis::extractTimingToDst(raw_ostream &O, unsigned DstReg,
                                                const SrcInfoTy &SrcInfo) const{
  typedef SrcInfoTy::const_iterator iterator;

  for (iterator I = SrcInfo.begin(), E = SrcInfo.end(); I != E; ++I)
    extractTimingForPair(O, DstReg, I->first);
}

void ExternalTimingAnalysis::writeTimingExtractionScript(raw_ostream &O,
                                                         const Twine &Name,
                                                         const sys::Path &ResultPath)
                                                         const {
  // Open the file and start the array.
  O << "set JSONFile [open \"" << ResultPath.str() <<"\" w+]\n"
       "puts $JSONFile \"\\[\"\n";

  typedef TimingNetlist::const_path_iterator iterator;
  for (iterator I = TNL.path_begin(), E = TNL.path_end(); I != E; ++I)
    extractTimingToDst(O, I->first, I->second);


  // Close the array and the file object.
  O << "puts $JSONFile \"\\{\\\"from\\\":0,\\\"to\\\":0,\\\"delay\\\":0\\}\"\n"
       "puts $JSONFile \"\\]\"\n"
       "close $JSONFile";
}

static bool exitWithError(const sys::Path &FileName) {
  errs() << "error opening file '" << FileName.str() << "' for writing!\n";
  return false;
}

static unsigned readPathDst(KeyValueNode *N) {
  assert(cast<ScalarNode>(N->getKey())->getRawValue() == "\"to\""
         && "Bad Key name!");

  unsigned Reg = 0;

  ScalarNode *Pin = cast<ScalarNode>(N->getValue());

  if (Pin->getRawValue().getAsInteger<unsigned>(10, Reg))
    return unsigned(-1);

  return Reg;
}

static VASTMachineOperand *readPathSrc(KeyValueNode *N) {
  assert(cast<ScalarNode>(N->getKey())->getRawValue() == "\"from\""
    && "Bad Key name!");

  intptr_t Ptr = 0;

  ScalarNode *Pin = cast<ScalarNode>(N->getValue());

  if (Pin->getRawValue().getAsInteger<intptr_t>(10, Ptr))
    return 0;

  return (VASTMachineOperand*)Ptr;
}

static double readDelay(KeyValueNode *N) {
  assert(cast<ScalarNode>(N->getKey())->getRawValue() == "\"delay\""
         && "Bad Key name!");

  ScalarNode *Delay = cast<ScalarNode>(N->getValue());
  return strtod(Delay->getRawValue().data(), 0);
}

static KeyValueNode *readAndAdvance(MappingNode::iterator it) {
  // Check if the iterator is dereferencable.
  assert(it != MappingNode::iterator() && "Cannot read map record!");
  KeyValueNode *N = it;
  ++it;
  return N;
}

bool ExternalTimingAnalysis::readPathDelay(MappingNode *N) {
  typedef MappingNode::iterator iterator;
  iterator CurPtr = N->begin();

  // Read the value of from, to and delay from the record.
  KeyValueNode *From = readAndAdvance(CurPtr);
  KeyValueNode *To = readAndAdvance(CurPtr);
  KeyValueNode *Delay = readAndAdvance(CurPtr);

  VASTMachineOperand *FromReg = readPathSrc(From);
  unsigned ToReg = readPathDst(To);
  double PathDelay = readDelay(Delay);

  dbgs() << "From: " << FromReg << " To: " << ToReg << " delay: "
         << PathDelay << '\n';

  if (PathDelay == -1.0) return false;

  // Annotate the delay to the timing netlist.
  TNL.annotateDelay(FromReg, ToReg, PathDelay);

  return true;
}

bool ExternalTimingAnalysis::readTimingAnalysisResult(const sys::Path &ResultPath) {
  // Read the timing analysis results.
  OwningPtr<MemoryBuffer> File;
  if (error_code ec = MemoryBuffer::getFile(ResultPath.c_str(), File)) {
    errs() <<  "Could not open input file: " <<  ec.message() << '\n';
    return false;
  }

  // Parse the JSON generated by quartus.
  SourceMgr sm;
  Stream JSONStream(File->getBuffer(), sm);
  SequenceNode *DelayArray = cast<SequenceNode>(JSONStream.begin()->getRoot());

  // Iterate over the array and get the delay for each register pair.
  typedef SequenceNode::iterator timing_it;
  for (timing_it I = DelayArray->begin(), E = DelayArray->end(); I != E; ++I) {
    MappingNode *N = cast<MappingNode>(I.operator Node *());
    
    // Parse the input-to-output delay record.
    readPathDelay(N);
  }

  return true;
}

bool ExternalTimingAnalysis::runExternalTimingAnalysis(const Twine &Name) {
  std::string ErrorInfo;

  // Write the Nestlist and the wrapper.
  sys::Path Netlist = buildPath(Name, ".v");
  if (Netlist.empty()) return false;

  errs() << "Writing '" << Netlist.str() << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);
  
  if (!ErrorInfo.empty())  return exitWithError(Netlist);

  // Write the netlist.
  TNL.writeVerilog(NetlistO, Name);
  NetlistO << '\n';
  // Also write the wrapper.
  writeNetlistWrapper(NetlistO, Name);
  NetlistO.close();
  errs() << " done. \n";

  // Write the SDC and the delay query script.
  sys::Path TimingExtractTcl = buildPath(Name, "_extract.tcl");
  if (TimingExtractTcl.empty()) return false;

  errs() << "Writing '" << TimingExtractTcl.str() << "'... ";

  raw_fd_ostream TimingExtractTclO(TimingExtractTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(TimingExtractTcl);

  sys::Path TimingExtractResult = buildPath(Name, "_result.json");
  if (TimingExtractResult.empty()) return false;

  writeTimingExtractionScript(TimingExtractTclO, Name, TimingExtractResult);
  TimingExtractTclO.close();
  errs() << " done. \n";

  // Write the project script.
  sys::Path PrjTcl = buildPath(Name, ".tcl");
  if (PrjTcl.empty()) return false;

  errs() << "Writing '" << PrjTcl.str() << "'... ";

  raw_fd_ostream PrjTclO(PrjTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(PrjTcl);

  writeProjectScript(PrjTclO, Name, Netlist, TimingExtractTcl);
  PrjTclO.close();
  errs() << " done. \n";

  const char *LUAPath[] = { "ExternalTool", "Path" };
  sys::Path quartus(getStrValueFromEngine(LUAPath));
  std::vector<const char*> args;

  args.push_back(quartus.c_str());
  args.push_back("-t");
  args.push_back(PrjTcl.c_str());
  args.push_back(0);

  errs() << "Running '" << quartus.str() << " ' program... ";
  if (sys::Program::ExecuteAndWait(quartus, &args[0], 0, 0, 0, 0, &ErrorInfo)) {
    errs() << "Error: " << ErrorInfo <<'\n';
    return false;
  }


  errs() << " done. \n";

  if (!readTimingAnalysisResult(TimingExtractResult))
    return false;

  // Clean up.
  Netlist.eraseFromDisk();
  TimingExtractTcl.eraseFromDisk();
  PrjTcl.eraseFromDisk();
  TimingExtractResult.eraseFromDisk();


  return true;
}

//void ExternalTimingAnalysis::runInternalTimingAnalysis() {
//  TimingEstimator TE(*this);
//
//  TE.buildDatapathDelayMatrix();
//
//  typedef PathInfoTy::iterator dst_iterator;
//  typedef SrcInfoTy::iterator src_iterator;
//  for (dst_iterator I = PathInfo.begin(), E = PathInfo.end(); I != E; ++I) {
//    unsigned DstReg = I->first;
//    SrcInfoTy &SrcInfo = I->second;
//    VASTWire *ExportedWire = lookupFanout(DstReg);
//
//    // Ignore the nodes that in the middle of the datapath.
//    if (ExportedWire == 0) continue;
//
//    VASTValue *Dst = ExportedWire->getAssigningValue().get();
//    
//    for (src_iterator SI = SrcInfo.begin(), SE = SrcInfo.end(); SI != SE; ++SI) {
//      VASTValue *Src = SI->first;
//      double delay = TE.getPathDelay(Src, Dst);
//      double old_delay = SI->second;
//      dbgs() << "DELAY-ESTIMATOR-JSON: { \"ACCURATE\":\"" << old_delay
//             << "\", \"BLACKBOX\":\"" << delay << "} \n";
//
//      SI->second = delay;
//    }
//  }
//}
