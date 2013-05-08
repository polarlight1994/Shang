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

#include "TimingNetlist.h"

#include "shang/Passes.h"
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

    return Filename;
  }

  ~TempDir() {
    //if (!Dirname.isEmpty()) Dirname.eraseFromDisk(true);
  }
};

struct ExternalTimingAnalysis : public VASTModulePass {
  VASTModule *VM;
  TimingNetlist *TNL;
  typedef TimingNetlist::PathTy PathTy;
  typedef TimingNetlist::SrcDelayInfo SrcDelayInfo;

  // Write the wrapper of the netlist.
  void writeNetlist(raw_ostream &O) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, const sys::Path &NetlistPath,
                          const sys::Path &ExtractScript) const;

  void extractTimingForPair(raw_ostream &O, const VASTSeqValue *Dst,
                            const VASTValue *Thu,
                            const VASTSeqValue *Src) const;

  void extractTimingToDst(raw_ostream &O, const VASTSeqValue *Dst,
                          const VASTValue *Thu, const SrcDelayInfo &Paths) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O,
                                   const sys::Path &ResultPath) const;
  // Read the JSON file written by the timing extraction script.
  bool readTimingAnalysisResult(const sys::Path &ResultPath);
  bool readPathDelay(yaml::MappingNode *N);

  static char ID;

  ExternalTimingAnalysis() : VASTModulePass(ID), VM(0), TNL(0) {
    initializeExternalTimingAnalysisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredTransitiveID(ControlLogicSynthesisID);
    AU.addRequiredTransitive<TimingNetlist>();
    AU.addRequiredTransitiveID(DatapathNamerID);
    AU.setPreservesAll();
  }

  bool runOnVASTModule(VASTModule &VM);
};
}

char ExternalTimingAnalysis::ID = 0;
char &llvm::ExternalTimingAnalysisID = ExternalTimingAnalysis::ID;

INITIALIZE_PASS_BEGIN(ExternalTimingAnalysis,
                      "vast-ext-timing-analysis",
                      "Perfrom External Timing Analysis",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
INITIALIZE_PASS_END(ExternalTimingAnalysis,
                    "vast-ext-timing-analysis",
                    "Perfrom External Timing Analysis",
                    false, true)

void ExternalTimingAnalysis::writeNetlist(raw_ostream &Out) const {
  // Read the result from the scripting engine.
  const char *GlobalCodePath[] = { "FUs", "CommonTemplate" };
  std::string GlobalCode = getStrValueFromEngine(GlobalCodePath);
  Out << GlobalCode << '\n';

  // Write buffers to output
  VM->printModuleDecl(Out);
  Out << "\n\n";
  // Reg and wire
  Out << "// Reg and wire decl\n";
  VM->printSignalDecl(Out);
  Out << "\n\n";
  // Datapath
  Out << "// Datapath\n";
  VM->printDatapath(Out);

  // Sequential logic of the registers.
  VM->printSubmodules(Out);
  VM->printRegisterBlocks(Out);

  Out << "endmodule\n";
  Out.flush();
}

void ExternalTimingAnalysis::writeProjectScript(raw_ostream &O,
                                                const sys::Path &NetlistPath,
                                                const sys::Path &ExtractScript)
                                                const {
  O << "load_package flow\n"
       "load_package report\n"
    << "project_new  -overwrite " << VM->getName()
    << " -family \"Cyclone IV E\" -part \"EP4CE75F29C6\" \n"
    << "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE EP4CE75F29C6\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << VM->getName() << "\n"
       "set_global_assignment -name SOURCE_FILE \""<< NetlistPath.str() <<"\"\n"
       //"set_global_assignment -name SDC_FILE @SDC_FILE@\n"
       "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
       "export_assignments\n"
       // Start the processes.
       "execute_module -tool map\n"
       "execute_module -tool fit -arg --early_timing_estimate\n";
       // "execute_module -tool fit\n"
       // "execute_module -tool sta -args {--report_script \""
       // << ExtractScript.str() << "\"}\n";
       // "project_close\n";
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
  for (iterator I = Paths.begin(), E = Paths.end(); I != E; ++I) {
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(I->first))
      extractTimingForPair(O, Dst, Thu, SeqVal);
  }
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

bool ExternalTimingAnalysis::runOnVASTModule(VASTModule &M) {
  TNL = &getAnalysis<TimingNetlist>();
  VM = &M;

  TempDir Dir;
  std::string ErrorInfo;

  // Write the Nestlist and the wrapper.
  sys::Path Netlist = Dir.buildPath(VM->getName(), ".sv");
  if (Netlist.empty()) return false;

  errs() << "Writing '" << Netlist.str() << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);
  
  if (!ErrorInfo.empty())  return exitWithError(Netlist);

  // Write the netlist.
  writeNetlist(NetlistO);
  NetlistO.close();
  errs() << " done. \n";

  // Write the SDC and the delay query script.
  sys::Path TimingExtractTcl = Dir.buildPath(VM->getName(), "_extract.tcl");
  if (TimingExtractTcl.empty()) return false;

  errs() << "Writing '" << TimingExtractTcl.str() << "'... ";

  raw_fd_ostream TimingExtractTclO(TimingExtractTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(TimingExtractTcl);

  sys::Path TimingExtractResult = Dir.buildPath(VM->getName(), "_result.json");
  if (TimingExtractResult.empty()) return false;

  writeTimingExtractionScript(TimingExtractTclO, TimingExtractResult);
  TimingExtractTclO.close();
  errs() << " done. \n";

  // Write the project script.
  sys::Path PrjTcl = Dir.buildPath(VM->getName(), ".tcl");
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

  return false;
}
