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
#include "TimingEstimator.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/VASTSubModules.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/PathV1.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/MemoryBuffer.h"
#define DEBUG_TYPE "external-timing-analysis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumQueriesWritten, "Number of path delay queries written");
STATISTIC(NumQueriesRead, "Number of path delay queries read");

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

// The parser to read the path delay records generated by the extraction script.
class PathRefParser {
  const char *CurPtr;
  const char *End;
  char CurChar;
  StringRef CurTok;

  StringRef getTok() {
    const char *TokStart = CurPtr;

    while (isspace(CurChar)) {
      ++CurPtr;
      CurChar = *CurPtr;
      TokStart = CurPtr;
      if (CurPtr == End) return StringRef();
    }

    while (isdigit(CurChar) || CurChar == '.') {
      ++CurPtr;
      CurChar = *CurPtr;
      if (CurPtr == End) return StringRef();
    }

    return StringRef(TokStart, CurPtr - TokStart);
  }

  StringRef eatTok() {
    StringRef Tok = CurTok;
    CurTok = getTok();
    return Tok;
  }

public:
  PathRefParser(const char *Start, const char *End)
    : CurPtr(Start), End(End), CurChar(*Start) {
    // Initialize the first token.
    eatTok();
  }

  bool finish() { return CurPtr == End; }

  unsigned getPathRef() {
    StringRef Idx = eatTok();
    DEBUG(dbgs() << "Idx: " << Idx << '\n');
    return strtoul(Idx.data(), 0, 10);
  }

  float getPathDelay() {
    StringRef Delay = eatTok();
    DEBUG(dbgs() << "Delay: " << Delay << '\n');
    return strtod(Delay.data(), 0);
  }
};

struct ExternalTimingAnalysis : TimingEstimatorBase {
  VASTModule &VM;
  SpecificBumpPtrAllocator<float> Allocator;
  std::vector<float*> DelayRefs;
  typedef std::map<VASTValue*, float*> SrcInfo;
  typedef std::map<VASTNode*, SrcInfo> PathInfo;
  PathInfo DelayMatrix;

  typedef TimingNetlist::PathTy PathTy;
  typedef TimingNetlist::SrcDelayInfo SrcDelayInfo;

  // Write the wrapper of the netlist.
  void writeNetlist(raw_ostream &O) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, const sys::Path &NetlistPath,
                          const sys::Path &ExtractScript) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O, const sys::Path &ResultPath);
  void extractTimingForSelector(raw_ostream &O, VASTSelector *Sel);
  void extractPathDelay(raw_ostream &O, VASTSelector *Sel, VASTValue *FI);

  void buildPathInfoForCone(raw_ostream &O, VASTValue *Root);
  void propagateSrcInfo(raw_ostream &O, VASTValue *V);

  void setDelay(unsigned Idx, float Delay) const {
    *DelayRefs[Idx] = Delay;
  }

  float *allocateDelayRef(unsigned &Idx) {
    Idx = DelayRefs.size();
    float *P = Allocator.Allocate();
    // Don't forget to initialize the content!
    *P = 0.0f;
    DelayRefs.push_back(P);
    ++NumQueriesWritten;
    return P;
  }

  // Read the JSON file written by the timing extraction script.
  bool readTimingAnalysisResult(const sys::Path &ResultPath);

  SrcDelayInfo &getOrCreateSrcDelayInfo(VASTValue *Src) {
    return PathDelay[Src];
  }

  ExternalTimingAnalysis(VASTModule &VM, TimingNetlist::PathDelayInfo &PathInfo)
    : TimingEstimatorBase(PathInfo, TimingEstimatorBase::ZeroDelay), VM(VM) {}

  bool analysisWithSynthesisTool();

  // Update the arrival time information of a VASTNode.
  void operator()(VASTNode *N) {
    VASTExpr *Expr = dyn_cast<VASTExpr>(N);

    if (Expr == 0) return;

    // Update the timing netlist according to the delay matrix.
    SrcInfo &Srcs = DelayMatrix[Expr];
    SrcDelayInfo &CurInfo = getOrCreateSrcDelayInfo(Expr);

    typedef SrcInfo::iterator iterator;
    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      float delay = *I->second;
      updateDelay(CurInfo, SrcEntryTy(I->first, delay_type(delay, delay)));
    }

    // Also accumlate the delay from the operands.
    typedef VASTExpr::op_iterator op_iterator;
    for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      VASTValPtr Op = *I;
      accumulateDelayFrom(Expr, Op.get());
    }
  }
};

struct ExternalTimingNetlist : public TimingNetlist {
  static char ID;

  ExternalTimingNetlist() : TimingNetlist(ID) {
    initializeExternalTimingNetlistPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    // Perform the control logic synthesis because we need to write the netlist.
    AU.addRequiredID(ControlLogicSynthesisID);
    TimingNetlist::getAnalysisUsage(AU);
  }

  bool runOnVASTModule(VASTModule &VM);
};
}

char ExternalTimingNetlist::ID = 0;
char &llvm::ExternalTimingNetlistID = ExternalTimingNetlist::ID;

INITIALIZE_PASS_BEGIN(ExternalTimingNetlist, "shang-external-timing-netlist",
                      "Preform Timing Estimation on the RTL Netlist"
                      " with the synthesis tool",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(ExternalTimingNetlist, "shang-external-timing-netlist",
                    "Preform Timing Estimation on the RTL Netlist"
                    " with the synthesis tool",
                    false, true)

bool ExternalTimingNetlist::runOnVASTModule(VASTModule &VM) {
  ExternalTimingAnalysis ETA(VM, PathInfo);

  // Run the synthesis tool to get the arrival time estimation, fall back to
  // the internal estimator if the external tool fail.
  if (!ETA.analysisWithSynthesisTool())
    return TimingNetlist::runOnVASTModule(VM);

  // Update the timing netlist.
  std::set<VASTOperandList*> Visited;

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;
    typedef VASTSelector::iterator fanin_iterator;
    for (fanin_iterator SI = Sel->begin(), SE = Sel->end(); SI != SE; ++SI) {
      VASTLatch U = *SI;
      VASTValue *FI = VASTValPtr(U).get();
      // Visit the cone rooted on the fanin.
      VASTOperandList::visitTopOrder(FI, Visited, ETA);
      buildTimingPathTo(FI, Sel, TNLDelay());

      VASTValue *Cnd = VASTValPtr(U.getPred()).get();
      // Visit the cone rooted on the guarding condition.
      VASTOperandList::visitTopOrder(Cnd, Visited, ETA);
      buildTimingPathTo(Cnd, Sel, TNLDelay());

      if (VASTValue *SlotActive = U.getSlotActive().get()) {
        // Visit the cone rooted on the ready signal.
        VASTOperandList::visitTopOrder(SlotActive, Visited, ETA);
        buildTimingPathTo(SlotActive, Sel, TNLDelay());
      }
    }
  }

  return false;
}

void ExternalTimingAnalysis::writeNetlist(raw_ostream &Out) const {
  // Name all expressions before writting the netlist.
  typedef DatapathContainer::expr_iterator iterator;
  for (iterator I = VM->expr_begin(), E = VM->expr_end(); I != E; ++I)
    I->nameExpr();

  // Read the result from the scripting engine.
  const char *GlobalCodePath[] = { "FUs", "CommonTemplate" };
  std::string GlobalCode = getStrValueFromEngine(GlobalCodePath);
  Out << GlobalCode << '\n';

  // Write buffers to output
  VM.printModuleDecl(Out);
  Out << "\n\n";
  // Reg and wire
  Out << "// Reg and wire decl\n";
  VM.printSignalDecl(Out);
  Out << "\n\n";
  // Datapath
  Out << "// Datapath\n";
  VM.printDatapath(Out);

  // Sequential logic of the registers.
  VM.printSubmodules(Out);
  VM.printRegisterBlocks(Out);

  Out << "endmodule\n";
  Out.flush();
}

void ExternalTimingAnalysis::writeProjectScript(raw_ostream &O,
                                                const sys::Path &NetlistPath,
                                                const sys::Path &ExtractScript)
                                                const {
  const char *LUAPath[] = { "TimingAnalysis", "Device" };
  const std::string &DeviceName = getStrValueFromEngine(LUAPath);

  O << "load_package flow\n"
       "load_package report\n"
    << "project_new  -overwrite " << VM.getName() << " \n"
//       "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE " << DeviceName << "\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << VM.getName() << "\n"
       "set_global_assignment -name SOURCE_FILE \""<< NetlistPath.str() <<"\"\n"
       //"set_global_assignment -name SDC_FILE @SDC_FILE@\n"
       "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
       "set_global_assignment -name TIMEQUEST_REPORT_SCRIPT_INCLUDE_DEFAULT_ANALYSIS OFF\n"
       "export_assignments\n"
       // Start the processes.
       "execute_module -tool map\n"
       "execute_module -tool fit -arg --early_timing_estimate\n"
       "execute_module -tool sta -args {--report_script \""
        << ExtractScript.str() << "\"}\n"
       "project_close\n";
}

void
ExternalTimingAnalysis::buildPathInfoForCone(raw_ostream &O, VASTValue *Root) {
  // Do nothing if the cone is visited.
  if (DelayMatrix.count(Root)) return;

  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(Root);

  if (L == 0) {
    // Insert the trivial path information.
    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(Root))
      DelayMatrix[V].insert(std::make_pair(V, (float*)0));

    return;
  }

  typedef  VASTOperandList::op_iterator ChildIt;

  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;
  VisitStack.push_back(std::make_pair(Root, L->op_begin()));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // All sources of this node is visited, now propagete the source information.
    if (It ==  VASTOperandList::GetDatapathOperandList(Node)->op_end()) {
      VisitStack.pop_back();

      propagateSrcInfo(O, Node);
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(ChildNode)) {
      DelayMatrix[V].insert(std::make_pair(V, (float*)0));
      continue;
    }

    if (VASTOperandList *L = VASTOperandList::GetDatapathOperandList(ChildNode)){
      // Mark the ChildNode as visited.
      bool inserted
        = DelayMatrix.insert(std::make_pair(ChildNode, SrcInfo())).second;

      // Do not visit the same node more than once.
      if (!inserted) continue;

      VisitStack.push_back(std::make_pair(ChildNode, L->op_begin()));
    }
  }
}

static std::string GetObjectName(const VASTSelector *Sel) {
  std::string Name;
  raw_string_ostream OS(Name);

  if (const VASTBlockRAM *RAM = dyn_cast<VASTBlockRAM>(Sel->getParent())) {
    OS << " *"
      // BlockRam name with prefix
      << getFUDesc<VFUBRAM>()->Prefix
      << VFUBRAM::getArrayName(RAM->getBlockRAMNum()) << "* *"
      // Or simply the name of the output register.
      << VFUBRAM::getArrayName(RAM->getBlockRAMNum())
      << "* ";
  } else
    OS << " *" << Sel->getName() << "* ";

  return OS.str();
}

static std::string GetObjectName(const VASTValue *V) {
  std::string Name;
  raw_string_ostream OS(Name);
  if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V)) {
    if (const VASTSeqValue *SV = dyn_cast<VASTSeqValue>(NV))
      return GetObjectName(SV->getSelector());

    // The block RAM should be printed as Prefix + ArrayName in the script.
    if (const char *N = NV->getName()) {
      OS << " *" << N << "* ";
      return OS.str();
    }
  } else if (const VASTExpr *E = dyn_cast<VASTExpr>(V)) {
    std::string Name = E->getSubModName();
    if (!Name.empty()) {
      OS << " *" << Name << "|* ";
      return OS.str();
    } else if (E->hasName()) {
      OS << " *" << E->getTempName() << "* ";
      return OS.str();
    }
  }

  return "";
}

static std::string GetCollection(const VASTSelector *Sel) {
  std::string Name;
  raw_string_ostream OS(Name);
  OS << "[get_keepers -nowarn \"" << GetObjectName(Sel) << "\"]";
  return OS.str();
}

static std::string GetCollection(const VASTValue *V) {
  if (const VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V))
    return GetCollection(SV->getSelector());

  if (isa<VASTExpr>(V)) {
    std::string Name;
    raw_string_ostream OS(Name);
    OS << "[get_cells -nowarn \"" << GetObjectName(V) << "\"]";
    return OS.str();
  }

  llvm_unreachable("Bad node type!");
  return "";
}

template<typename T0, typename T1, typename T2>
static
void extractTimingForPath(raw_ostream &O, T0 *Dst, T1 *Thu, T2 *Src,
                          unsigned RefIdx, bool ExtractMinDelay = false) {
  O << "set src " << GetCollection(Src) << '\n';
  O << "set dst " << GetCollection(Dst) << '\n';
  if (Thu) O << "set thu " << GetCollection(Thu) << '\n';
  O << "if {[get_collection_size $src] && [get_collection_size $dst]} {\n";
  if (Thu) O << "if {[get_collection_size $thu]} {\n";
  // Use get_path instead of get_timing_path to get the longest delay paths
  // between arbitrary points in the netlist.
  // See "get_path -help" for more information.
  O << "set paths [get_path -from $src -to $dst ";
  if (Thu) O << " -through $thu";
  // Sometimes we may need to extract the minimal delay.
  if (ExtractMinDelay) O << " -min_path -npath 1 ";
  else                 O << " -nworst 1 ";
  O << " -pairs_only]\n"
    // Only extract the delay from source to destination when these node are
    // not optimized.
    "if {[get_collection_size $paths]} {\n"
    "  foreach_in_collection path $paths {\n"
    "    set delay [get_path_info $path -data_delay]\n"
    "    post_message -type info \"" << GetObjectName(Src);
  if (Thu) O << " -> " << GetObjectName(Thu);
  O << " -> " << GetObjectName(Dst) << " delay: $delay\"\n"
    "puts $JSONFile \"" << RefIdx << " $delay\"\n" <<
    "  }\n"
    "}\n"; // Path Size
  if (Thu) O << "}\n"; // Thu Size
  O << "}\n"; // Src and Dst Size
}

void ExternalTimingAnalysis::propagateSrcInfo(raw_ostream &O, VASTValue *V) {
  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(V);
  assert(L && "Bad Value!");

  SrcInfo &PI = DelayMatrix[V];

  typedef VASTOperandList::op_iterator iterator;
  for (iterator I = L->op_begin(), E = L->op_end(); I != E; ++I) {
    VASTValPtr Op = *I;
    PathInfo::iterator at = DelayMatrix.find(Op.get());
    // TODO: Assert Op is the leaf of the cone.
    if (at == DelayMatrix.end()) continue;

    // Propagate the source information from the operand.
    SrcInfo &Srcs = at->second;
    typedef SrcInfo::iterator source_iterator;
    for (source_iterator SI = Srcs.begin(), SE = Srcs.end(); SI != SE; ++SI) {
      VASTValue *Src = SI->first;
      // Is the delay record allocated?
      float *&P = PI[Src];
      if (P) continue;

      // Otherwise allocate the record now.
      unsigned Idx = 0;
      P = allocateDelayRef(Idx);
      // Generate the corresponding delay extraction script.
      extractTimingForPath(O, V, (VASTValue*)0, Src, Idx);
    }
  }
}

void ExternalTimingAnalysis::extractPathDelay(raw_ostream &O, VASTSelector *Sel,
                                              VASTValue *FI) {
  // TODO: Extract the mux delay from FI to Sel.
}

void ExternalTimingAnalysis::extractTimingForSelector(raw_ostream &O,
                                                      VASTSelector *Sel) {
  typedef VASTSelector::iterator fanin_iterator;
  for (fanin_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;
    VASTValue *FI = VASTValPtr(U).get();
    // Visit the cone rooted on the fanin.
    buildPathInfoForCone(O, FI);
    extractPathDelay(O, Sel, FI);

    VASTValue *Cnd = VASTValPtr(U.getPred()).get();
    // Visit the cone rooted on the guarding condition.
    buildPathInfoForCone(O, Cnd);
    extractPathDelay(O, Sel, Cnd);

    if (VASTValue *SlotActive = U.getSlotActive().get()) {
      buildPathInfoForCone(O, SlotActive);
      extractPathDelay(O, Sel, SlotActive);
    }
  }
}

void
ExternalTimingAnalysis::writeTimingExtractionScript(raw_ostream &O,
                                                    const sys::Path &ResultPath)
{
  // Print the critical path in the datapath to debug the TimingNetlist.
  O << "create_clock -name \"clk\" -period 1ns [get_ports {clk}]\n"
       "derive_pll_clocks -create_base_clocks\n"
       "derive_clock_uncertainty\n"
       "report_timing -from_clock { clk } -to_clock { clk }"
         " -setup -npaths 1 -detail full_path -stdout\n"
  // Open the file and start the array.
       "set JSONFile [open \"" << ResultPath.str() <<"\" w+]\n";

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    extractTimingForSelector(O, I);

  // Close the array and the file object.
  O << "close $JSONFile\n";
}

static bool exitWithError(const sys::Path &FileName) {
  errs() << "error opening file '" << FileName.str() << "' for writing!\n";
  return false;
}

bool ExternalTimingAnalysis::readTimingAnalysisResult(const sys::Path &ResultPath) {
  // Read the timing analysis results.
  OwningPtr<MemoryBuffer> File;
  if (error_code ec = MemoryBuffer::getFile(ResultPath.c_str(), File)) {
    errs() <<  "Could not open input file: " <<  ec.message() << '\n';
    return false;
  }

  // Parse the file generated by quartus.
  PathRefParser Parser(File->getBufferStart(), File->getBufferEnd());

  while (!Parser.finish()) {
    unsigned idx = Parser.getPathRef();
    float delay = Parser.getPathDelay();
    setDelay(idx, delay);
    ++NumQueriesRead;
  }

  return true;
}

bool ExternalTimingAnalysis::analysisWithSynthesisTool() {
  TempDir Dir;
  std::string ErrorInfo;

  // Write the Nestlist and the wrapper.
  sys::Path Netlist = Dir.buildPath(VM.getName(), ".sv");
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

  const char *LUAPath[] = { "TimingAnalysis", "ExternalTool" };
  sys::Path quartus(getStrValueFromEngine(LUAPath));
  std::vector<const char*> args;

  args.push_back(quartus.c_str());
  args.push_back("--64bit");
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
