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

#include "Dataflow.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/VASTSubModules.h"
#include "shang/VASTMemoryPort.h"
#include "shang/STGDistances.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "external-timing-analysis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumQueriesWritten, "Number of path delay queries written");
STATISTIC(NumQueriesRead, "Number of path delay queries read");
static cl::opt<bool> Use64BitQuartus("vast-use-64bit-quartus",
  cl::desc("Use 64bit quartus to perform timing analysis"),
#ifdef _MSC_VER
  cl::init(false));
#else
  cl::init(true));
#endif

namespace {
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

struct ExternalTimingAnalysis {
  VASTModule &VM;
  SpecificBumpPtrAllocator<float> Allocator;
  std::vector<float*> DelayRefs;
  typedef std::map<VASTValue*, float*> SrcInfo;
  typedef std::map<VASTNode*, SrcInfo> PathInfo;
  PathInfo DelayMatrix;
  typedef std::map<VASTSelector*, float*> SelDelayInfo;
  SelDelayInfo SelectorDelay;

  // Write the wrapper of the netlist.
  void writeNetlist(raw_ostream &O) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, StringRef NetlistPath,
                          StringRef SDCPath, StringRef ExtractScript) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O, StringRef ResultPath);
  void extractTimingForSelector(raw_ostream &O, VASTSelector *Sel);
  void extractSelectorDelay(raw_ostream &O, VASTSelector *Sel);

  typedef std::set<VASTSeqValue*> LeafSet;
  void buildPathInfoForCone(raw_ostream &O, VASTValue *Root, LeafSet &Leaves);

  static bool contains(const SrcInfo &Srcs, const LeafSet &Leaves) {
    typedef LeafSet::iterator iterator;
    for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I)
      if (Srcs.count(*I))
        return false;

    return true;
  }

  void propagateSrcInfo(raw_ostream &O, VASTExpr *V, LeafSet &Leaves);

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
  bool readTimingAnalysisResult(StringRef ResultPath);

  explicit ExternalTimingAnalysis(VASTModule &VM) : VM(VM) {}

  bool analysisWithSynthesisTool();

  void getPathDelay(VASTSelector *Sel, VASTValue *FI,
                    std::map<VASTSeqValue*, float> &Srcs,
                    std::set<VASTExpr*> &Visited);

    // Update the arrival time information of a VASTNode.
  void operator()(VASTNode *N) {
    VASTExpr *Expr = dyn_cast<VASTExpr>(N);

    if (Expr == 0) return;

    PathInfo::iterator I = DelayMatrix.find(Expr);

    // No need to build the source arrival times set if there is no source.
    if (I == DelayMatrix.end()) return;

    // Update the timing netlist according to the delay matrix.
    SrcInfo &Srcs = I->second;

    typedef SrcInfo::const_iterator iterator;

    // Also accumulate the delay from the operands.
    typedef VASTExpr::op_iterator op_iterator;
    for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      VASTValue *Op = VASTValPtr(*I).get();

      PathInfo::const_iterator J = DelayMatrix.find(Op);

      if (J == DelayMatrix.end()) continue;

      const SrcInfo &OpSrcs = J->second;

      // Forward the arrival time information from the operands.
      // This make sure the arrival time from any register to current node are
      // no smaller than the arrival time from the same register.
      // Equivalent to accumulateDelayFrom(Op, Expr);
      for (iterator SI = OpSrcs.begin(), SE = OpSrcs.end(); SI != SE; ++SI) {
        SrcInfo::iterator K = Srcs.find(SI->first);
        if (K == Srcs.end())
          continue;

        *K->second = std::max(*K->second, *SI->second);
      }
    }
  }
};
}

void ExternalTimingAnalysis::getPathDelay(VASTSelector *Sel, VASTValue *FI,
                                          std::map<VASTSeqValue*, float> &Srcs,
                                          std::set<VASTExpr*> &Visited) {
  LeafSet Leaves;
  FI->extractSupportingSeqVal(Leaves);
  if (Leaves.empty())
    return;

  PathInfo::const_iterator I = DelayMatrix.find(Sel);
  assert(I != DelayMatrix.end() && "Fanin delay not available!");
  const SrcInfo &FIDelays = I->second;

  bool AnyPathMissed = false;

  typedef LeafSet::iterator iterator;
  typedef SrcInfo::const_iterator src_iterator;
  for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
    VASTSeqValue *Leaf = *I;
    src_iterator J = FIDelays.find(Leaf);

    // If there is more than one paths between Leaf and selector, the delay
    // is not directly available.
    if (J == FIDelays.end()) {
      AnyPathMissed |= true;
      continue;
    }

    // Otherwise Update the delay.
    float &OldDelay = Srcs[Leaf];
    OldDelay = std::max(OldDelay, *J->second);
  }

  if (!AnyPathMissed)
    return;

  SelDelayInfo::const_iterator J = SelectorDelay.find(Sel);
  assert(J != SelectorDelay.end() && "Delay of selector not found!");

  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(FI)) {
    // DIRTY HACK: If FI is simply a VASTSeqValue, we need to get the
    // interconnect delay between SV and Sel. But TimeQuest will always return
    // the longest path delay. For now we simply return the delay from the
    // slot register as the interconnect delay.
    float &OldDelay = Srcs[SV];
    OldDelay = std::max(OldDelay, *J->second);
    return;
  }

  cast<VASTExpr>(FI)->visitConeTopOrder(Visited, *this);
  PathInfo::const_iterator K = DelayMatrix.find(FI);
  assert(K != DelayMatrix.end() && "Fanin delay not available!");

  const SrcInfo &LeavesDelays = K->second;
  for (src_iterator I = LeavesDelays.begin(), E = LeavesDelays.end();
       I != E; ++I) {
    VASTSeqValue *Leaf = cast<VASTSeqValue>(I->first);

    float &OldDelay = Srcs[Leaf];
    OldDelay = std::max(OldDelay, *I->second + *J->second);
  }
}

bool DataflowAnnotation::externalDelayAnnotation(VASTModule &VM) {
  ExternalTimingAnalysis ETA(VM);

  // Run the synthesis tool to get the arrival time estimation.
  if (!ETA.analysisWithSynthesisTool()) return false;

  std::set<VASTExpr*> Visited;
  typedef VASTModule::seqop_iterator iterator;
  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;

    Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());

    // Nothing to do if Op does not have an underlying instruction.
    if (!Inst)
      continue;

    bool IsLaunch = false;
    if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op))
      IsLaunch = SeqInst->isLaunch();

    std::map<VASTSeqValue*, float> Srcs;

    VASTValPtr Cnd = Op->getGuard();

    for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
      VASTLatch L = Op->getSrc(i);
      VASTSelector *Sel = L.getSelector();
      if (Sel->isTrivialFannin(L))
        continue;

      VASTValPtr FI = L;

      // Extract the delay from the fan-in and the guarding condition.
      ETA.getPathDelay(Sel, FI.get(), Srcs, Visited);
      ETA.getPathDelay(Sel, Cnd.get(), Srcs, Visited);
    }

    typedef std::map<VASTSeqValue*, float>::iterator src_iterator;
    for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *Src = I->first;
      float delay = I->second;
      annotateDelay(DataflowInst(Inst, IsLaunch), Op->getSlot(), Src, delay);
    }
  }

  // External timing analysis successfully completed.
  return true;
}

void ExternalTimingAnalysis::writeNetlist(raw_ostream &Out) const {
  // Read the result from the scripting engine.
  const char *FUTemplatePath[] = { "FUs", "CommonTemplate" };
  std::string FUTemplate = getStrValueFromEngine(FUTemplatePath);
  Out << FUTemplate << '\n';

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    I->printSelectorModule(Out);

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

void
ExternalTimingAnalysis::writeProjectScript(raw_ostream &O,
                                           StringRef NetlistPath,
                                           StringRef SDCPath,
                                           StringRef ExtractScript) const {
  const char *LUAPath[] = { "TimingAnalysis", "Device" };
  const std::string &DeviceName = getStrValueFromEngine(LUAPath);

  O << "load_package flow\n"
       "load_package report\n"
    << "project_new  -overwrite " << VM.getName() << " \n"
//       "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE " << DeviceName << "\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << VM.getName() << "\n"
       "set_global_assignment -name SOURCE_FILE \"";
  O.write_escaped(NetlistPath);
  O << "\"\n"
       "set_global_assignment -name SDC_FILE \"";
  O.write_escaped(SDCPath);
  O << "\"\n"
       "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
       "set_global_assignment -name SYNTH_TIMING_DRIVEN_SYNTHESIS OFF\n"
       "set_global_assignment -name TIMEQUEST_REPORT_SCRIPT_INCLUDE_DEFAULT_ANALYSIS OFF\n"
       "export_assignments\n"
       // Start the processes.
       "execute_module -tool map\n"
       "execute_module -tool fit -arg --early_timing_estimate\n"
       "execute_module -tool sta -args {--report_script \"";
  O.write_escaped(ExtractScript);
  O << "\"}\n"
       "project_close\n";
}

void
ExternalTimingAnalysis::buildPathInfoForCone(raw_ostream &O, VASTValue *Root,
                                             LeafSet &Leaves) {

  VASTExpr *Expr = dyn_cast<VASTExpr>(Root);

  if (Expr == 0)
    return;

  // Do nothing if the cone is visited.
  PathInfo::iterator I = DelayMatrix.find(Root);
  if (I != DelayMatrix.end() && contains(I->second, Leaves))
    return;

  typedef  VASTOperandList::op_iterator ChildIt;

  std::set<VASTExpr*> Visited;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;
  VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // All sources of this node is visited, now propagete the source information.
    if (It ==  Node->op_end()) {
      VisitStack.pop_back();

      propagateSrcInfo(O, Node, Leaves);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(ChildNode)){
      // Mark the ChildNode as visited, and do not visit the same node more than
      // once.
      if (!Visited.insert(SubExpr).second)
        continue;

      VisitStack.push_back(std::make_pair(SubExpr, SubExpr->op_begin()));
    }
  }
}

static std::string GetSTACollection(const VASTSelector *Sel) {
  const std::string &Name = Sel->getSTAObjectName();
  assert(!Name.empty() && "Unexpected anonymous selector!");
  return "[get_keepers -nowarn \"" + Name +  "\"]";
}

static std::string GetSTACollection(const VASTValue *V) {
  if (const VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V))
    return GetSTACollection(SV->getSelector());

  if (isa<VASTExpr>(V)) {
    const std::string &Name = V->getSTAObjectName();
    if (!Name.empty())
      return "[get_pins -compatibility_mode -nowarn \"" + Name + "\"]";
  }

  llvm_unreachable("Bad node type!");
  return "";
}

static
void extractTimingForPath(raw_ostream &O, unsigned RefIdx) {
  O << "set delay \"No-path\"\n";
  O << "if {[get_collection_size $src] && [get_collection_size $dst]} {\n";
  // Use get_path instead of get_timing_path to get the longest delay paths
  // between arbitrary points in the netlist.
  // See "get_path -help" for more information.
  O << "  set paths [get_path -from $src -to $dst -nworst 1 -pairs_only]\n"
    // Only extract the delay from source to destination when these node are
    // not optimized.
       "  if {[get_collection_size $paths]} {\n"
       "    foreach_in_collection path $paths {\n"
       "      set delay [get_path_info $path -data_delay]\n"
       "      puts $JSONFile \"" << RefIdx << " $delay\"\n" <<
       "    }\n"
       "  }\n" // Path Size
       "}\n"; // Src and Dst Size
}

template<typename T0, typename T1>
static
void extractTimingForPath(raw_ostream &O, T0 *Dst, T1 *Src, unsigned RefIdx) {
  O << "set src " << GetSTACollection(Src) << '\n';
  O << "set dst " << GetSTACollection(Dst) << '\n';
  // if (Thu) O << "set thu " << GetSTACollection(Thu) << '\n';
  extractTimingForPath(O, RefIdx);
  DEBUG(O << "post_message -type info \"" << Src->getSTAObjectName()
    << " -> " << Dst->getSTAObjectName() << " delay: $delay\"\n");
}

void ExternalTimingAnalysis::propagateSrcInfo(raw_ostream &O, VASTExpr *V,
                                              LeafSet &Leaves) {
  SrcInfo &PI = DelayMatrix[V];

  typedef VASTOperandList::op_iterator iterator;
  for (iterator I = V->op_begin(), E = V->op_end(); I != E; ++I) {
    VASTValue *Op = (*I).getAsLValue<VASTValue>();

    if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(Op)) {
      if (!Leaves.count(SV))
        continue;

      float *&P = PI[SV];
      if (P) continue;

      unsigned Idx = 0;
      P = allocateDelayRef(Idx);
      // Generate the corresponding delay extraction script.
      if (V->hasName()) extractTimingForPath(O, V, SV, Idx);
      continue;
    }

    PathInfo::iterator at = DelayMatrix.find(Op);
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
      if (V->hasName()) extractTimingForPath(O, V, Src, Idx);
    }
  }

  // Erase the empty entry.
  if (PI.empty())
    DelayMatrix.erase(V);
}

static std::string GetSelectorCollection(VASTSelector *Sel) {
  const std::string Name(Sel->getName());
  return "[get_pins -compatibility_mode -nowarn \"" + Name + "_selector*\"];";
}

void ExternalTimingAnalysis::extractSelectorDelay(raw_ostream &O,
                                                  VASTSelector *Sel) {
  // Get the delay from the selector wire of the selector.
  O << "set dst " << GetSTACollection(Sel) << '\n';
  O << "set src " << GetSelectorCollection(Sel) << '\n';
  unsigned Idx = 0;
  SelectorDelay[Sel] = allocateDelayRef(Idx);
  extractTimingForPath(O, Idx);
  O << "post_message -type info \" selector -> "
    << Sel->getSTAObjectName() << " delay: $delay\"\n";
}

void ExternalTimingAnalysis::extractTimingForSelector(raw_ostream &O,
                                                      VASTSelector *Sel) {
  std::set<VASTValue*> Fanins;
  LeafSet AllLeaves, IntersectLeaves, CurLeaves;
  typedef LeafSet::iterator leaf_iterator;
  typedef VASTSelector::iterator fanin_iterator;

  for (fanin_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U))
      continue;

    // Collect all leaves for current fanin.
    CurLeaves.clear();
    U->extractSupportingSeqVal(CurLeaves);
    U.getGuard()->extractSupportingSeqVal(CurLeaves);

    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *Leaf = *LI;
      if (!AllLeaves.insert(Leaf).second)
        IntersectLeaves.insert(Leaf);
    }

    // Directly add the slot active to all leaves set.
    if (VASTValue *SlotActive = U.getSlotActive().get())
      SlotActive->extractSupportingSeqVal(AllLeaves);
  }

  // Extract end-to-end delay.
  for (leaf_iterator I = AllLeaves.begin(), E = AllLeaves.end(); I != E; ++I) {
    VASTSeqValue *Src = *I;

    if (IntersectLeaves.count(Src) && !Src->isSlot() && !Src->isFUOutput())
      continue;

    unsigned Idx = 0;
    // Directly use the register-to-register delay.
    DelayMatrix[Sel][Src] = allocateDelayRef(Idx);
    extractTimingForPath(O, Sel, Src, Idx);
  }

  extractSelectorDelay(O, Sel);

  // Extract path delay in details for leaves that reachable to different fanins
  if (IntersectLeaves.empty())
    return;

  for (fanin_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U))
      continue;

    buildPathInfoForCone(O, VASTValPtr(U).get(), IntersectLeaves);
    buildPathInfoForCone(O, VASTValPtr(U.getGuard()).get(), IntersectLeaves);
  }
}

void
ExternalTimingAnalysis::writeTimingExtractionScript(raw_ostream &O,
                                                    StringRef ResultPath) {
  // Print the critical path in the datapath to debug the TimingNetlist.
  O << "report_timing -from_clock { clk } -to_clock { clk }"
         " -setup -npaths 1 -detail full_path -stdout\n"
  // Open the file and start the array.
       "set JSONFile [open \"";
  O.write_escaped(ResultPath);
  O << "\" w+]\n";

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    extractTimingForSelector(O, I);

  // Close the array and the file object.
  O << "close $JSONFile\n";
}

template<typename T>
static bool exitWithError(const T &FileName) {
  errs() << "error opening file '" << FileName.str() << "' for writing!\n";
  return false;
}

bool ExternalTimingAnalysis::readTimingAnalysisResult(StringRef ResultPath) {
  // Read the timing analysis results.
  OwningPtr<MemoryBuffer> File;
  if (error_code ec = MemoryBuffer::getFile(ResultPath, File)) {
    errs() <<  "Could not open input file: " <<  ec.message() << '\n';
    return false;
  }

  // Parse the file generated by quartus.
  PathRefParser Parser(File->getBufferStart(), File->getBufferEnd());

  while (!Parser.finish()) {
    unsigned idx = Parser.getPathRef();
    float delay = Parser.getPathDelay() / VFUs::Period;
    setDelay(idx, delay);
    ++NumQueriesRead;
  }

  return true;
}

bool ExternalTimingAnalysis::analysisWithSynthesisTool() {
  SmallString<256> OutputDir
    = sys::path::parent_path(getStrValueFromEngine("RTLOutput"));
  sys::path::append(OutputDir, "TimingNetlist");
  bool Existed;
  sys::fs::create_directories(StringRef(OutputDir), Existed);
  (void)Existed;

  std::string ErrorInfo;

  // Write the SDC and the delay query script.

  SmallString<256> TimingExtractTcl(OutputDir);
  sys::path::append(TimingExtractTcl, VM.getName() + "_extract.tcl");

  errs() << "Writing '" << TimingExtractTcl.str() << "'... ";

  raw_fd_ostream TimingExtractTclO(TimingExtractTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(TimingExtractTcl);

  SmallString<256> TimingExtractResult(OutputDir);
  sys::path::append(TimingExtractResult, VM.getName() + "_result.json");

  writeTimingExtractionScript(TimingExtractTclO, TimingExtractResult);
  TimingExtractTclO.close();
  errs() << " done. \n";

  // Write the Nestlist and the wrapper.
  SmallString<256> Netlist(OutputDir);
  sys::path::append(Netlist, VM.getName() + ".sv");

  errs() << "Writing '" << Netlist << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(Netlist);

  // Write the netlist.
  writeNetlist(NetlistO);
  NetlistO.close();
  errs() << " done. \n";

  // Write the SDC for the project.
  SmallString<256> SDC(OutputDir);
  sys::path::append(SDC, VM.getName() + ".sdc");
  errs() << "Writing '" << SDC << "'... ";

  raw_fd_ostream SDCO(SDC.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(SDC);

  SDCO << "create_clock -name \"clk\" -period " << format("%.2fns", VFUs::Period)
       << " [get_ports {clk}]\n"
          "derive_pll_clocks -create_base_clocks\n"
          "derive_clock_uncertainty\n";
  SDCO.close();
  errs() << " done. \n";

  // Write the project script.
  SmallString<256> PrjTcl(OutputDir);
  sys::path::append(PrjTcl, VM.getName() + ".tcl");

  errs() << "Writing '" << PrjTcl.str() << "'... ";

  raw_fd_ostream PrjTclO(PrjTcl.c_str(), ErrorInfo);

  if (!ErrorInfo.empty())  return exitWithError(PrjTcl);

  writeProjectScript(PrjTclO, Netlist, SDC, TimingExtractTcl);
  PrjTclO.close();
  errs() << " done. \n";

  const char *LUAPath[] = { "TimingAnalysis", "ExternalTool" };
  sys::Path quartus(getStrValueFromEngine(LUAPath));
  std::vector<const char*> args;

  args.push_back(quartus.c_str());
  if (Use64BitQuartus)
    args.push_back("--64bit");
  args.push_back("-t");
  args.push_back(PrjTcl.c_str());
  args.push_back(0);

  sys::Path Empty;
  const sys::Path *Redirects[] = { &Empty, &Empty, &Empty };
  errs() << "Running '" << quartus.str() << " ' program... ";
  if (sys::Program::ExecuteAndWait(quartus, &args[0], 0, Redirects, 0, 0,
                                   &ErrorInfo)) {
    errs() << "Error: " << ErrorInfo <<'\n';
    return false;
  }

  errs() << " done. \n";

  if (!readTimingAnalysisResult(TimingExtractResult))
    return false;

  return true;
}
