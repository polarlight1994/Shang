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

namespace {
struct TempDir {
  sys::Path Dirname;

  TempDir() {
    std::string ErrMsg;
    Dirname = sys::Path::GetTemporaryDirectory(&ErrMsg);
    if (Dirname.isEmpty()) errs() << "Error: " << ErrMsg << "\n";
  }

  sys::Path buildPath(const Twine &Name, const Twine &Ext) {
    std::string ErrMsg;
    // FIXME: Delete the Temporary Directory
    sys::Path Filename = Dirname;
    if (Filename.isEmpty()) {
      errs() << "Error: " << ErrMsg << "\n";
      return sys::Path();
    }

    Filename.appendComponent((Name + Ext).str());
    if (Filename.makeUnique(true, &ErrMsg)) {
      errs() << "Error: " << ErrMsg << "\n";
      return sys::Path();
    }

    return Filename;
  }

  ~TempDir() {
    if (!Dirname.isEmpty()) Dirname.eraseFromDisk(true);
  }
};
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

  if (!V->empty()) {
    O.indent(Indent) << "if (read_netlist) begin\n";
    // For each fanin, print the datapath, selected by the slot register.
    O.indent(Indent + 2) << VASTModule::ParallelCaseAttr << " case (1'b1)\n";
    typedef VASTSeqValue::const_iterator fanin_iterator;
    for (fanin_iterator FI = V->begin(), FE = V->end(); FI != FE; ++FI) {
      VASTSeqUse U = *FI;

      O.indent(Indent + 2) << '(' << U.getSlot()->getName() << "): begin ";
      O << V->getName() << " <= ";
      if (U->getASTType() == VASTNode::vastWire)
        // Ignore the wrapper wire and print some random value.
        O << intptr_t(VASTValPtr(U).getOpaqueValue());
        else
        VASTValPtr(U).printAsOperand(O);
      O << "; end\n";
    }

    O.indent(Indent + 2) << "endcase\n";

    O.indent(Indent) << "end\n";
  }

  // Active the scan chain shifting.
  O.indent(Indent) << "if (shift) begin\n";

  // Connect the last register to current register.
  printAsLHS(O.indent(Indent + 2), V, Bitwidth, Bitwidth - 1) << " <= ";

  if (LastV)
    LastV->printAsOperand(O, 1, 0, false);
  else // We are in the beginning of the chain.
    O << "scan_chain_in";

  O << ";\n";

  // Shift the register.
  if (MultiBits) {
    printAsLHS(O.indent(Indent + 2), V, Bitwidth - 1, 0) << " <= ";
    V->printAsOperand(O, Bitwidth, 1, false);    
    O << ";\n";
  }

  O.indent(Indent) << "end\n";

  O << '\n';

  return V;
}

namespace {
  struct DatapathPrinter {
  raw_ostream &OS;

  DatapathPrinter(raw_ostream &OS) : OS(OS) {}

  void operator()(VASTNode *N) const {
    if (VASTWire *W = dyn_cast<VASTWire>(N))  {
      // Declare the wire if necessary.
      OS << "wire ";
      if (W->getBitWidth() > 1)
        OS << "[" << (W->getBitWidth() - 1) << ":0] ";
      OS << W->getName();

      if (VASTValPtr V= W->getDriver()) {
        OS << " = ";
        // Print some random constant for the linked time known value.
        if (isa<VASTLLVMValue>(V.get())) OS << intptr_t(W);
        else                             V.printAsOperand(OS);
      }

      OS << ";\n";
    }

    if (VASTExpr *E = dyn_cast<VASTExpr>(N)) {
      // If the wire has name at the first time we visit it, it is a output
      // pint. Otherwise we should declare it here.
      E->nameExpr();
      OS << "wire ";

      if (E->getBitWidth() > 1)
        OS << "[" << (E->getBitWidth() - 1) << ":0] ";

      OS << E->getTempName() << " = ";

      // Temporary unname the rexpression so that we can print its logic.
      //if (!E->getSubModName().empty()) {
      //  OS << ";\n";
      //  if (E->printFUInstantiation(OS))
      //    return;

      //  // If the FU instantiation is not printed, we need to print the
      //  // assignment.
      //  OS << "assign " << E->getTempName();
      //}

      E->nameExpr(false);
      E->printAsOperand(OS, false);

      OS << ";\n";
      E->nameExpr();
    }
  }
};
}

void ExternalTimingAnalysis::writeNetlist(raw_ostream &O) const {
  typedef VASTModule::seqval_iterator iterator;
  typedef VASTSeqValue::iterator fanin_iterator;

  // FIXME: Use the luascript template?
  O << "module " << VM.getName() << "_wapper(\n";
  O.indent(2) << "input wire scan_clk,\n";
  O.indent(2) << "input wire read_netlist,\n";
  O.indent(2) << "input wire shift,\n";
  O.indent(2) << "input wire scan_chain_in,\n";
  O.indent(2) << "output reg scan_chain_out);\n";


  std::set<VASTExpr*> Fanins;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SVal = I;    
    // Declare the registers for the scan chain.
    O.indent(4) << "reg";
    unsigned Bitwidth = SVal->getBitWidth();

    if (Bitwidth > 1) O << "[" << (Bitwidth - 1) << ":0]";

    O << ' ' << SVal->getName() << ";\n";

    // Collect the wires.
    for (fanin_iterator FI = SVal->begin(), FE = SVal->end(); FI != FE; ++FI)
      if (VASTExpr *E = dyn_cast<VASTExpr>(VASTValPtr(*FI).get())) 
        Fanins.insert(E);
  }

  // Print the combinational logic.
  std::set<VASTOperandList*> Visited;
  typedef std::set<VASTExpr*>::iterator output_iterator;
  for (output_iterator I = Fanins.begin(), E = Fanins.end(); I != E; ++I)
    VASTOperandList::visitTopOrder(*I, Visited, DatapathPrinter(O));

  O.indent(4) << "// Scan chain timing\n";

  // Build the scan chain.
  const VASTValue *LastReg = 0;
  O.indent(4) << "always @(posedge scan_clk) begin\n";
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

  O << '\n';

  O << "endmodule\n\n";
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
       "set_global_assignment -name TOP_LEVEL_ENTITY " << VM.getName() << "_wapper\n"
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
static std::string getName(const VASTValue *V) {
  if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V))
    return NV->getName();

  return cast<VASTExpr>(V)->getTempName();
}

static void setTerminatorCollection(raw_ostream & O, const VASTSeqValue *V,
                                    const char *CollectionName) {
  O << "set " << CollectionName << " [" << "get_keepers \"*" << V->getName()
    << "*\"]\n";
}

void ExternalTimingAnalysis::extractTimingForPair(raw_ostream &O,
                                                  const VASTSeqValue *Dst,
                                                  const VASTValue *Thu,
                                                  const VASTSeqValue *Src)
                                                  const {
  // Get the source and destination nodes.
  setTerminatorCollection(O, Dst, "dst");
  setTerminatorCollection(O, Src, "src");
  
  O << "set " << "thu" << " [" << "get_nets \"*" << getName(Thu) << "*\"]\n";
  
  O <<
    "set paths [get_timing_paths -from $src -to $dst -setup -npath 1 -detail path_only]\n"
    "set delay -1\n"
    // Only extract the delay from source to destination when these node are
    // not optimized.
    "if {[get_collection_size $src] && [get_collection_size $dst] && [get_collection_size $paths]} {\n"
    "  foreach_in_collection path $paths {\n"
    "    set delay [get_path_info $path -data_delay]\n"
    "    post_message -type info \"" << intptr_t(Src) << ' '<< Src->getName()
    << " -> " << intptr_t(Dst) << ' '<< Dst->getName() << ' '
    << getName(Dst) << " delay: $delay\"\n"
    "  }\n"
    "} else {\n"
    "    post_message -type info \"" << intptr_t(Src)<< ' ' << Src->getName()
    << " -> " << intptr_t(Dst) << ' ' << Dst->getName() << ' '
    << getName(Dst) << " path not found!\"\n"
    "}\n"
    "puts $JSONFile \"\\{\\\"from\\\":" << intptr_t(Src) << ",\\\"to\\\":"
    <<  intptr_t(Dst) << ",\\\"delay\\\":$delay\\},\"\n";
}

void ExternalTimingAnalysis::extractTimingToDst(raw_ostream &O,
                                                const VASTSeqValue *Dst,
                                                const VASTValue *Thu,
                                                const SrcDelayInfo &Paths) const
{
  typedef TimingNetlist::src_iterator iterator;
  for (iterator I = Paths.begin(), E = Paths.end(); I != E; ++I)
    extractTimingForPair(O, Dst, Thu, I->first);
}

void ExternalTimingAnalysis::writeTimingExtractionScript(raw_ostream &O,
                                                         const sys::Path &ResultPath)
                                                         const {
  // Print the critical path in the datapath to debug the TimingNetlist.
  O << "report_timing -from_clock { scan_clk } -to_clock { scan_clk }"
       " -setup -npaths 1 -detail full_path -stdout\n"
  // Open the file and start the array.
       "set JSONFile [open \"" << ResultPath.str() <<"\" w+]\n"
       "puts $JSONFile \"\\[\"\n";

  typedef VASTModule::seqval_iterator iterator;
  typedef VASTSeqValue::iterator fanin_iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SVal = I;    

    for (fanin_iterator FI = SVal->begin(), FE = SVal->end(); FI != FE; ++FI) {
      VASTValue *Thu = VASTValPtr(*FI).get();
      if (const SrcDelayInfo *Src = TNL.getSrcInfo(Thu))
        extractTimingToDst(O, SVal, Thu, *Src);
    }
  }

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

  dbgs() << "From: " << Src->getName() << " To: " << Src->getName() << " delay: "
         << PathDelay << '\n';

  if (PathDelay == -1.0) {
    //dumpNetlistTree(dbgs(), Dst);
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
  TempDir Dir;
  std::string ErrorInfo;

  // Write the Nestlist and the wrapper.
  sys::Path Netlist = Dir.buildPath(VM.getName(), ".v");
  if (Netlist.empty()) return false;

  errs() << "Writing '" << Netlist.str() << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);
  
  if (!ErrorInfo.empty())  return exitWithError(Netlist);

  // Write the netlist.
  writeNetlist(NetlistO);
  NetlistO.close();
  errs() << " done. \n";

  // Write the SDC and the delay query script.
  sys::Path TimingExtractTcl = Dir.buildPath(VM.getName(), "_extract.tcl");
  if (TimingExtractTcl.empty()) return false;

  errs() << "Writing '" << TimingExtractTcl.str() << "'... ";

  raw_fd_ostream TimingExtractTclO(TimingExtractTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(TimingExtractTcl);

  sys::Path TimingExtractResult = Dir.buildPath(VM.getName(), "_result.json");
  if (TimingExtractResult.empty()) return false;

  writeTimingExtractionScript(TimingExtractTclO, TimingExtractResult);
  TimingExtractTclO.close();
  errs() << " done. \n";

  // Write the project script.
  sys::Path PrjTcl = Dir.buildPath(VM.getName(), ".tcl");
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

  return true;
}
