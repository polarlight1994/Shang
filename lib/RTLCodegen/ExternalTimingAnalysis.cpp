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

void ExternalTimingAnalysis::writeNetlistWrapper(raw_ostream &O) const {
  // FIXME: Use the luascript template?
  O << "module " << TNL.Name << "wapper(\n";
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
  O.indent(4) << TNL.Name << ' ' << TNL.Name << "_inst(\n";

  for (FaninIterator I = TNL.fanin_begin(), E = TNL.fanin_end(); I != E; ++I)
    O.indent(6) << '.' << I->second->getName()
                << '(' << I->second->getName() << "),\n";

  for (FanoutIterator I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I)
    O.indent(6) << '.' << I->second->getName()
                << '(' << I->second->getName() << "w),\n";

  O.indent(6) << ".dummy_" << TNL.Name << "_output(0));\n\n";

  O << "endmodule\n";
}

void ExternalTimingAnalysis::writeProjectScript(raw_ostream &O,
                                                const sys::Path &NetlistPath,
                                                const sys::Path &ExtractScript)
                                                const {
  O << "load_package flow\n"
       "load_package report\n"
    << "project_new " << TNL.Name << " -overwrite\n"
    << "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE EP4CE75F29C6\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << TNL.Name << "wapper\n"
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

void ExternalTimingAnalysis::extractTimingForPair(raw_ostream &O,
                                                  const VASTWire *Dst,
                                                  const VASTNamedValue *Src)
                                                  const {
  // Get the source and destination nodes.
  setTerminatorCollection(O, Dst, "dst");
  setTerminatorCollection(O, Src, "src");
  
  VASTValue *PathDst = Dst->getAssigningValue().get();
  assert(PathDst && "Bad Assignment!");

  O <<
    "set delay -1\n"
    // Only extract the delay from source to destination when these node are
    // not optimized.
    "if {[get_collection_size $src] && [get_collection_size $dst]} {\n"
    "  foreach_in_collection path [get_timing_paths -from $src -to $dst -setup"
    "                              -npath 1 -detail path_only] {\n"
    "    set delay [get_path_info $path -data_delay]\n"
    "    post_message -type info \"" << intptr_t(Src) << " -> " << intptr_t(PathDst)
    << " delay: $delay\"\n"
    "  }\n"
    "} else {\n"
    "    post_message -type warning \"" << intptr_t(Src) << " -> " << intptr_t(PathDst)
    << " path not found!\""
    "}\n"
    "puts $JSONFile \"\\{\\\"from\\\":" << intptr_t(Src) << ",\\\"to\\\":"
    <<  intptr_t(PathDst) << ",\\\"delay\\\":$delay\\},\"\n";
}

void ExternalTimingAnalysis::extractTimingToDst(raw_ostream &O,
                                                const VASTWire *Dst) const{
  typedef SrcInfoTy::const_iterator it;
  VASTValue *PathDst = Dst->getAssigningValue().get();

  for (it I = TNL.src_begin(PathDst), E = TNL.src_end(PathDst); I != E; ++I)    
    extractTimingForPair(O, Dst, I->first);
}

void ExternalTimingAnalysis::writeTimingExtractionScript(raw_ostream &O,
                                                         const sys::Path &ResultPath)
                                                         const {
  // Open the file and start the array.
  O << "set JSONFile [open \"" << ResultPath.str() <<"\" w+]\n"
       "puts $JSONFile \"\\[\"\n";

  typedef TimingNetlist::const_path_iterator iterator;
  for (FanoutIterator I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I)
      extractTimingToDst(O, I->second);


  // Close the array and the file object.
  O << "puts $JSONFile \"\\{\\\"from\\\":0,\\\"to\\\":0,\\\"delay\\\":0\\}\"\n"
       "puts $JSONFile \"\\]\"\n"
       "close $JSONFile";
}

static bool exitWithError(const sys::Path &FileName) {
  errs() << "error opening file '" << FileName.str() << "' for writing!\n";
  return false;
}

static VASTValue *readPathDst(KeyValueNode *N) {
  assert(cast<ScalarNode>(N->getKey())->getRawValue() == "\"to\""
         && "Bad Key name!");

  intptr_t Ptr = 0;

  ScalarNode *Pin = cast<ScalarNode>(N->getValue());

  if (Pin->getRawValue().getAsInteger<intptr_t>(10, Ptr))
    return 0;

  return (VASTValue*)Ptr;
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

  VASTMachineOperand *Src = readPathSrc(From);
  VASTValue *Dst = readPathDst(To);

  // Ignore the the trivial entry.
  if (!Src && !Dst) return true;

  double PathDelay = readDelay(Delay);

  dbgs() << "From: " << Src << " To: " << Dst << " delay: " << PathDelay << '\n';

  if (PathDelay == -1.0) return false;

  // Annotate the delay to the timing netlist.
  TNL.annotateDelay(Src, Dst, PathDelay);

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
  sys::Path Netlist = buildPath(TNL.Name, ".v");
  if (Netlist.empty()) return false;

  errs() << "Writing '" << Netlist.str() << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);
  
  if (!ErrorInfo.empty())  return exitWithError(Netlist);

  // Write the netlist.
  TNL.writeVerilog(NetlistO, TNL.Name);
  NetlistO << '\n';
  // Also write the wrapper.
  writeNetlistWrapper(NetlistO);
  NetlistO.close();
  errs() << " done. \n";

  // Write the SDC and the delay query script.
  sys::Path TimingExtractTcl = buildPath(TNL.Name, "_extract.tcl");
  if (TimingExtractTcl.empty()) return false;

  errs() << "Writing '" << TimingExtractTcl.str() << "'... ";

  raw_fd_ostream TimingExtractTclO(TimingExtractTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(TimingExtractTcl);

  sys::Path TimingExtractResult = buildPath(TNL.Name, "_result.json");
  if (TimingExtractResult.empty()) return false;

  writeTimingExtractionScript(TimingExtractTclO, TimingExtractResult);
  TimingExtractTclO.close();
  errs() << " done. \n";

  // Write the project script.
  sys::Path PrjTcl = buildPath(TNL.Name, ".tcl");
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
