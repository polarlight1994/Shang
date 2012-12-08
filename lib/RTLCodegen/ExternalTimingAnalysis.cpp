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

void TimingNetlist::createDelayEntry(unsigned DstReg, unsigned SrcReg) {
  assert(DstReg && "Unexpected NO_REGISTER!");

  SrcInfoTy &SrcInfo = PathInfo[DstReg];

  SrcInfo[SrcReg] = 0;
}

void TimingNetlist::computeDelayFromSrc(unsigned DstReg, unsigned SrcReg) {
  // Forward the Src terminator of the path from SrcReg.
  PathInfoTy::iterator at = PathInfo.find(SrcReg);

  // If SrcReg is a terminator of a path, create a path from SrcReg to DstReg.
  if (at == PathInfo.end()) {
    createDelayEntry(DstReg, SrcReg);
    return;
  }

  // Otherwise forward the source nodes reachable to SrcReg to DstReg.
  set_union(PathInfo[DstReg], at->second);
}

void TimingNetlist::createAnchor(unsigned DstReg) {
  createDelayEntry(DstReg, 0);
}

void TimingNetlist::addInstrToDatapath(MachineInstr *MI) {
  unsigned DefReg = 0;

  bool IsDatapath = VInstrInfo::isDatapath(MI->getOpcode());

  // Can use add the MachineInstr to the datapath?
  if (IsDatapath)
    buildDatapath(MI);

  // Ignore the PHIs.
  if (!IsDatapath && MI->isPHI()) return;

  // Otherwise export the values used by this MachineInstr.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i){
    const MachineOperand &MO = MI->getOperand(i);

    if (!MO.isReg() || MO.getReg() == 0) continue;

    if (MO.isDef()) {
      assert(DefReg == 0 && "Unexpected multi-defines!");
      DefReg = MO.getReg();
      continue;
    }

    // Only care about a use register.
    unsigned UseReg = MO.getReg();

    // Try to export the value.
    if (IsDatapath)
      computeDelayFromSrc(DefReg, UseReg);
    else
      exportValue(UseReg);
  }

  // Create anchor for the datapath result, so it is connected to something.
  if (IsDatapath) createAnchor(DefReg);
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

void
TimingNetlist::writeNetlistWrapper(raw_ostream &O, const Twine &Name) const {
  // FIXME: Use the luascript template?
  O << "module " << Name << "wapper(\n";
  O.indent(2) << "input wire clk,\n";
  O.indent(2) << "input wire read_netlist,\n";
  O.indent(2) << "input wire scan_chain_in,\n";
  O.indent(2) << "output reg scan_chain_out);\n";

  // Declare the registers for the scan chain.
  for (FaninIterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    const VASTMachineOperand *MO = I->second;
    O.indent(4) << "reg";
    unsigned Bitwidth = MO->getBitWidth();

    if (Bitwidth > 1) O << "[" << (Bitwidth - 1) << ":0]";

    O << ' ' << MO->getName() << ";\n";
  }

  for (FanoutIterator I = fanout_begin(), E = fanout_end(); I != E; ++I) {
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
  for (FaninIterator I = fanin_begin(), E = fanin_end(); I != E; ++I)
    LastReg = printScanChainLogic<false>(O, I->second, LastReg, 6);

  for (FanoutIterator I = fanout_begin(), E = fanout_end(); I != E; ++I)
    LastReg = printScanChainLogic<true>(O, I->second, LastReg, 6);

  // Write the output.
  O.indent(6) << "scan_chain_out <= ";

  if (LastReg) LastReg->printAsOperand(O, 1, 0, false);
  else         "scan_chain_in";

  O << ";\n";

  // Close the timing block.
  O.indent(4) << "end\n";

  O.indent(4) << '\n';

  // Instantiate the netlist.
  O.indent(4) << Name << ' ' << Name << "_inst(\n";

  for (FaninIterator I = fanin_begin(), E = fanin_end(); I != E; ++I)
    O.indent(6) << '.' << I->second->getName()
                << '(' << I->second->getName() << "),\n";

  for (FanoutIterator I = fanout_begin(), E = fanout_end(); I != E; ++I)
    O.indent(6) << '.' << I->second->getName()
                << '(' << I->second->getName() << "w),\n";

  O.indent(6) << ".dummy_" << Name << "_output(0));\n\n";

  O << "endmodule\n";
}

void TimingNetlist::writeProjectScript(raw_ostream &O, const Twine &Name,
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
       << TimingExtractionScript.str() << "\"}\n";
       //"project_close\n";
}

static void setTerminatorCollection(raw_ostream & O, const VASTValue *V,
                                    const char *CollectionName) {
  O << "set " << CollectionName << " [";
  if (const VASTExpr *E = dyn_cast<VASTExpr>(V))
    O << "get_nets \"*" << E->getTempName() << "*\"]\n";
  else
    O << "get_keepers \"*" << cast<VASTNamedValue>(V)->getName() << "*\"]\n";
}

VASTValPtr TimingNetlist::getSrcPtr(unsigned Reg) const {
  // Lookup the value from input of the netlist.
  return (*this)->lookupExpr(Reg);
}

VASTValPtr TimingNetlist::getDstPtr(unsigned Reg) const {
  // Lookup the value from the output list of the netlist first.
  if (VASTValPtr V = lookupFanout(Reg))
    return V;

  return getSrcPtr(Reg);
}

void TimingNetlist::extractTimingForPair(raw_ostream &O, unsigned DstReg,
                                         unsigned SrcReg) const {
  VASTValue *Dst = dyn_cast<VASTWire>(getDstPtr(DstReg).get());
  VASTValue *Src = dyn_cast<VASTMachineOperand>(getSrcPtr(SrcReg).get());

  if (!(Dst && Src)) return;

  extractTimingForPair(O, Dst, DstReg, Src, SrcReg);
}

void TimingNetlist::extractTimingForPair(raw_ostream &O,
                                         const VASTValue *Dst, unsigned DstReg,
                                         const VASTValue *Src, unsigned SrcReg)
                                         const {
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
    "    post_message -type info \"" << SrcReg << " -> " << DstReg
    << " delay: $delay\"\n"
    "  }\n"
    "} else {\n"
    "    post_message -type warning \"" << SrcReg << " -> " << DstReg
    << " path not found!\""
    "}\n"
    "puts $JSONFile \"\\{\\\"from\\\":" << SrcReg << ",\\\"to\\\":" << DstReg
    << ",\\\"delay\\\":$delay\\},\"\n";
}

void
TimingNetlist::extractTimingToDst(raw_ostream &O, unsigned DstReg,
                                  const SrcInfoTy &SrcInfo) const{
  typedef SrcInfoTy::const_iterator iterator;

  for (iterator I = SrcInfo.begin(), E = SrcInfo.end(); I != E; ++I) {
    unsigned SrcReg = I->first;
    if (SrcReg) {
      extractTimingForPair(O, DstReg, SrcReg);
      continue;
    }
  }
}

void
TimingNetlist::writeTimingExtractionScript(raw_ostream &O, const Twine &Name,
                                           const sys::Path &ResultPath) const {
  // Open the file and start the array.
  O << "set JSONFile [open \"" << ResultPath.str() <<"\" w+]\n"
       "puts $JSONFile \"\\[\"\n";

  typedef PathInfoTy::const_iterator iterator;
  for (iterator I = PathInfo.begin(), E = PathInfo.end(); I != E; ++I)
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

static unsigned readPathTerimator(KeyValueNode *N, StringRef KeyName) {
  assert(cast<ScalarNode>(N->getKey())->getRawValue() == KeyName
         && "Bad Key name!");

  unsigned Reg = 0;

  ScalarNode *Pin = cast<ScalarNode>(N->getValue());

  if (Pin->getRawValue().getAsInteger<unsigned>(10, Reg))
    return unsigned(-1);

  return Reg;
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

bool TimingNetlist::readPathDelay(MappingNode *N) {
  typedef MappingNode::iterator iterator;
  iterator CurPtr = N->begin();

  // Read the value of from, to and delay from the record.
  KeyValueNode *From = readAndAdvance(CurPtr);
  KeyValueNode *To = readAndAdvance(CurPtr);
  KeyValueNode *Delay = readAndAdvance(CurPtr);

  unsigned FromReg = readPathTerimator(From, "\"from\""),
           ToReg = readPathTerimator(To, "\"to\"");
  double PathDelay = readDelay(Delay);

  dbgs() << "From: " << FromReg << " To: " << ToReg << " delay: "
         << PathDelay << '\n';

  if (PathDelay == -1.0) return false;

  PathInfo[ToReg][FromReg] = PathDelay / VFUs::Period;

  return true;
}

bool TimingNetlist::readTimingAnalysisResult(const sys::Path &ResultPath) {
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

bool TimingNetlist::runExternalTimingAnalysis(const Twine &Name) {
  std::string ErrorInfo;

  // Write the Nestlist and the wrapper.
  sys::Path Netlist = buildPath(Name, ".v");
  if (Netlist.empty()) return false;

  errs() << "Writing '" << Netlist.str() << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);
  
  if (!ErrorInfo.empty())  return exitWithError(Netlist);

  // Write the netlist.
  writeVerilog(NetlistO, Name);
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

  // Clean up.
  //Netlist.eraseFromDisk();
  //TimingExtractTcl.eraseFromDisk();
  //PrjTcl.eraseFromDisk();

  errs() << " done. \n";

  if (!readTimingAnalysisResult(TimingExtractResult))
    return false;

  return true;
}
