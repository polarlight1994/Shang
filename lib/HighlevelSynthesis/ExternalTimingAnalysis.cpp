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
#include "shang/VASTModule.h"

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

static sys::Path buildPath(const Twine &Name, const Twine &Ext) {
  std::string ErrMsg;
  // FIXME: Delete the Temporary Directory
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

static const VASTNamedValue *printScanChainLogic(raw_ostream &O,
                                                 const VASTSeqValue *V,
                                                 const VASTValue *LastV,
                                                 unsigned Indent) {
  unsigned Bitwidth = V->getBitWidth();
  bool MultiBits = Bitwidth > 1;

  O.indent(Indent) << "if (read_netlist) \n";
  // For each fanin, print the datapath, selected by the slot register.
  O.indent(Indent + 2) << V->getName() << " <= " << V->getName() << "w;\n";
  //
  O.indent(Indent) << "else begin\n";
  // Increase the indent in the esle block.
  Indent += 2;

  // Active the scan chain shifting.
  O.indent(Indent) << "if (shift) begin\n";

  Indent += 2;
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

  Indent -= 2;
  O.indent(Indent) << "end\n";

  // Finish the else block if necessary.
  O.indent(Indent - 2) << "end\n";

  O << '\n';

  return V;
}

void ExternalTimingAnalysis::writeNetlist(raw_ostream &O) const {
  // FIXME: Use the luascript template?
  O << "module " << VM.getName() << "wapper(\n";
  O.indent(2) << "input wire clk,\n";
  O.indent(2) << "input wire read_netlist,\n";
  O.indent(2) << "input wire shift,\n";
  O.indent(2) << "input wire scan_chain_in,\n";
  O.indent(2) << "output reg scan_chain_out);\n";

  // Declare the registers for the scan chain.
  typedef VASTModule::seqval_iterator iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SeqVal = I;

    O.indent(4) << "reg";
    unsigned Bitwidth = SeqVal->getBitWidth();

    if (Bitwidth > 1) O << "[" << (Bitwidth - 1) << ":0]";

    O << ' ' << SeqVal->getName() << ";\n";
  }

  O.indent(4) << "// Scan chain timing\n";

  // Build the scan chain.
  const VASTValue *LastReg = 0;
  O.indent(4) << "always @(posedge clk) begin\n";
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SeqVal = I;

    LastReg = printScanChainLogic(O, SeqVal, LastReg, 6);
  }

  // Write the output.
  O.indent(6) << "scan_chain_out <= ";

  if (LastReg) LastReg->printAsOperand(O, 1, 0, false);
  else         O << "scan_chain_in";

  O << ";\n";

  // Close the timing block.
  O.indent(4) << "end\n";

  O.indent(4) << '\n';

  O << "endmodule\n";
}

void ExternalTimingAnalysis::writeProjectScript(raw_ostream &O,
                                                const sys::Path &NetlistPath,
                                                const sys::Path &ExtractScript)
                                                const {
  O << "load_package flow\n"
       "load_package report\n"
    << "project_new " << VM.getName() << " -overwrite\n"
    << "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE EP4CE75F29C6\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << VM.getName() << "wapper\n"
       "set_global_assignment -name SOURCE_FILE \""<< NetlistPath.str() <<"\"\n"
       //"set_global_assignment -name SDC_FILE @SDC_FILE@\n"
       "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
       "export_assignments\n"
       // Start the processes.
       "execute_module -tool map\n"
       "execute_module -tool fit\n"
       "execute_module -tool sta -args {--report_script \""
       << ExtractScript.str() << "\"}\n"
       "project_close\n";
}

static void setTerminatorCollection(raw_ostream & O, const VASTNamedValue *V,
                                    const char *CollectionName) {
  O << "set " << CollectionName << " [" << "get_keepers \"*" << V->getName()
    << "*\"]\n";
}

static std::string getName(const VASTValue *V) {
  if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V))
    return NV->getName();

  return cast<VASTExpr>(V)->getTempName();
}

void ExternalTimingAnalysis::extractTimingForPair(raw_ostream &O,
                                                  const VASTSeqValue *Dst,
                                                  const VASTSeqValue *Src)
                                                  const {
  // Get the source and destination nodes.
  setTerminatorCollection(O, Dst, "dst");
  setTerminatorCollection(O, Src, "src");
  
  const VASTValue *PathDst = Dst;
  assert(PathDst && "Bad Assignment!");

  O <<
    "set paths [get_timing_paths -from $src -to $dst -setup -npath 1 -detail path_only]\n"
    "set delay -1\n"
    // Only extract the delay from source to destination when these node are
    // not optimized.
    "if {[get_collection_size $src] && [get_collection_size $dst] && [get_collection_size $paths]} {\n"
    "  foreach_in_collection path $paths {\n"
    "    set delay [get_path_info $path -data_delay]\n"
    "    post_message -type info \"" << intptr_t(Src) << ' '<< getName(Src)
    << " -> " << intptr_t(PathDst) << ' '<< getName(PathDst) << ' '
    << getName(Dst) << " delay: $delay\"\n"
    "  }\n"
    "} else {\n"
    "    post_message -type info \"" << intptr_t(Src)<< ' ' << getName(Src)
    << " -> " << intptr_t(PathDst) << ' ' << getName(PathDst) << ' '
    << getName(Dst) << " path not found!\"\n"
    "}\n"
    "puts $JSONFile \"\\{\\\"from\\\":" << intptr_t(Src) << ",\\\"to\\\":"
    <<  intptr_t(PathDst) << ",\\\"delay\\\":$delay\\},\"\n";
}

void ExternalTimingAnalysis::extractTimingToDst(raw_ostream &O,
                                                const VASTSeqValue *Dst) const{
  //typedef SrcInfoTy::const_iterator it;
  //VASTValue *PathDst = Dst->getDriver().get();

  //for (it I = TNL.src_begin(PathDst), E = TNL.src_end(PathDst); I != E; ++I)    
  //  extractTimingForPair(O, Dst, I->first);
}

void ExternalTimingAnalysis::writeTimingExtractionScript(raw_ostream &O,
                                                         const sys::Path &ResultPath)
                                                         const {
  // Print the critical path in the datapath to debug the TimingNetlist.
  O << "report_timing -from_clock { clk } -to_clock { clk } -setup -npaths 1"
       " -detail full_path -stdout\n"
  // Open the file and start the array.
       "set JSONFile [open \"" << ResultPath.str() <<"\" w+]\n"
       "puts $JSONFile \"\\[\"\n";

  //for (FanoutIterator I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I)
  //    extractTimingToDst(O, I->second);


  // Close the array and the file object.
  O << "puts $JSONFile \"\\{\\\"from\\\":0,\\\"to\\\":0,\\\"delay\\\":0\\}\"\n"
       "puts $JSONFile \"\\]\"\n"
       "close $JSONFile\n";
}

static bool exitWithError(const sys::Path &FileName) {
  errs() << "error opening file '" << FileName.str() << "' for writing!\n";
  return false;
}

static VASTSeqValue *readPathDst(KeyValueNode *N) {
  assert(cast<ScalarNode>(N->getKey())->getRawValue() == "\"to\""
         && "Bad Key name!");

  intptr_t Ptr = 0;

  ScalarNode *Pin = cast<ScalarNode>(N->getValue());

  if (Pin->getRawValue().getAsInteger<intptr_t>(10, Ptr))
    return 0;

  return (VASTSeqValue*)Ptr;
}

static VASTSeqValue *readPathSrc(KeyValueNode *N) {
  assert(cast<ScalarNode>(N->getKey())->getRawValue() == "\"from\""
         && "Bad Key name!");

  intptr_t Ptr = 0;

  ScalarNode *Pin = cast<ScalarNode>(N->getValue());

  if (Pin->getRawValue().getAsInteger<intptr_t>(10, Ptr))
    return 0;

  return (VASTSeqValue*)Ptr;
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

static void dumpNetlistTree(raw_ostream &O, VASTValue *Dst)  {
  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;
  std::set<VASTValue*> Visited;

  VisitStack.push_back(std::make_pair(Dst, VASTValue::dp_dep_begin(Dst)));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == VASTValue::dp_dep_end(Node)) {
      VisitStack.pop_back();

      if (VASTExpr *E = dyn_cast<VASTExpr>(Node)) {
        E->unnameExpr();
        std::string Name = E->getTempName();
        O.indent(2) << "wire ";

        unsigned Bitwidth = E->getBitWidth();
        if (Bitwidth > 1) O << "[" << (Bitwidth - 1) << ":0]";
        O << ' ' << Name << " = ";
        E->printAsOperand(O, false);
        O << ";\n";

        // Assign the name to the expression.
        E->nameExpr();
      } else if (VASTWire *W = dyn_cast<VASTWire>(Node))
        W->printAssignment(O.indent(2));

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->getAsLValue<VASTValue>();
    ++VisitStack.back().second;

    if (!Visited.insert(ChildNode).second)  continue;

    if (!isa<VASTWire>(ChildNode) && !isa<VASTExpr>(ChildNode)) continue;

    VisitStack.push_back(std::make_pair(ChildNode, VASTValue::dp_dep_begin(ChildNode)));
  }
}

bool ExternalTimingAnalysis::readPathDelay(MappingNode *N) {
  typedef MappingNode::iterator iterator;
  iterator CurPtr = N->begin();

  // Read the value of from, to and delay from the record.
  KeyValueNode *From = readAndAdvance(CurPtr);
  KeyValueNode *To = readAndAdvance(CurPtr);
  KeyValueNode *Delay = readAndAdvance(CurPtr);

  VASTSeqValue *Src = readPathSrc(From);
  VASTSeqValue *Dst = readPathDst(To);

  // Ignore the the trivial entry.
  if (!Src && !Dst) return true;

  double PathDelay = readDelay(Delay);

  dbgs() << "From: " << getName(Src) << " To: " << getName(Dst) << " delay: "
         << PathDelay << '\n';

  if (PathDelay == -1.0) {
    dumpNetlistTree(dbgs(), Dst);
    return false;
  }

  // Annotate the delay to the timing netlist.
  //TNL.annotateDelay(Src, Dst, PathDelay);

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

bool ExternalTimingAnalysis::runExternalTimingAnalysis() {
  std::string ErrorInfo;

  // Write the Nestlist and the wrapper.
  sys::Path Netlist = buildPath(VM.getName(), ".v");
  if (Netlist.empty()) return false;

  errs() << "Writing '" << Netlist.str() << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);
  
  if (!ErrorInfo.empty())  return exitWithError(Netlist);

  // Write the netlist.
  writeNetlist(NetlistO);
  NetlistO.close();
  errs() << " done. \n";

  // Write the SDC and the delay query script.
  sys::Path TimingExtractTcl = buildPath(VM.getName(), "_extract.tcl");
  if (TimingExtractTcl.empty()) return false;

  errs() << "Writing '" << TimingExtractTcl.str() << "'... ";

  raw_fd_ostream TimingExtractTclO(TimingExtractTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(TimingExtractTcl);

  sys::Path TimingExtractResult = buildPath(VM.getName(), "_result.json");
  if (TimingExtractResult.empty()) return false;

  writeTimingExtractionScript(TimingExtractTclO, TimingExtractResult);
  TimingExtractTclO.close();
  errs() << " done. \n";

  // Write the project script.
  sys::Path PrjTcl = buildPath(VM.getName(), ".tcl");
  if (PrjTcl.empty()) return false;

  errs() << "Writing '" << PrjTcl.str() << "'... ";

  raw_fd_ostream PrjTclO(PrjTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(PrjTcl);

  writeProjectScript(PrjTclO, Netlist, TimingExtractTcl);
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