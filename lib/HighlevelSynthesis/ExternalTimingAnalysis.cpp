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
#include "shang/VASTMemoryPort.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileSystem.h"
#define DEBUG_TYPE "external-timing-analysis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumQueriesWritten, "Number of path delay queries written");
STATISTIC(NumQueriesRead, "Number of path delay queries read");

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

struct ExternalTimingAnalysis : TimingEstimatorBase {
  VASTModule &VM;
  SpecificBumpPtrAllocator<float> Allocator;
  std::vector<float*> DelayRefs;
  typedef std::map<VASTValue*, float*> SrcInfo;
  typedef std::map<VASTNode*, SrcInfo> PathInfo;
  PathInfo DelayMatrix;

  // The delay from the selector wire and the selector enable.
  DenseMap<VASTSelector*, float*> SelectorDelay;
  DenseMap<unsigned, float*> BRAMOutputDelay;

  float getSelDelay(VASTSelector *S, VASTValue *FI) const {
    float *ptr = SelectorDelay.lookup(S);
    assert(ptr && "Selector delay not allocated!");

    // Also accumulate the interconnect delay from fanin.
    PathInfo::const_iterator I = DelayMatrix.find(S);
    assert(I != DelayMatrix.end() && "Fanin delay not available!");
    const SrcInfo &FIDelays = I->second;
    SrcInfo::const_iterator J = FIDelays.find(FI);
    // Only return the sel delay if interconnect delay is not available.
    if (J == FIDelays.end()) return *ptr;

    assert(J->second && "Delay record not allocated?");
    return *ptr + *J->second;
  }

  float getFUOutputDelay(VASTSelector *Sel) const {
    assert(Sel->isFUOutput() && "Bad selector type!");

    // We only extract the FU output delay of BRAm for now.
    VASTMemoryBus *Bus = dyn_cast<VASTMemoryBus>(Sel->getParent());
    if (!Bus) return 0.0f;

    float *ptr = BRAMOutputDelay.lookup(Bus->getNumber());
    assert(ptr && "BRAM input delay not allocated!");
    return *ptr;
  }

  typedef TimingNetlist::PathTy PathTy;
  typedef TimingNetlist::SrcDelayInfo SrcDelayInfo;

  // Write the wrapper of the netlist.
  void writeNetlist(raw_ostream &O) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, StringRef NetlistPath,
                          StringRef ExtractScript) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingExtractionScript(raw_ostream &O, StringRef ResultPath);
  void extractTimingForSelector(raw_ostream &O, VASTSelector *Sel);
  void extractSelectorDelay(raw_ostream &O, VASTSelector *Sel);
  void extractInterConnectDelay(raw_ostream &O, VASTSelector *Sel,
                                VASTValue *From);
  void extractFUOutputDelay(raw_ostream &O, VASTSelector *Sel);

  void buildPathInfoForCone(raw_ostream &O, VASTValue *Root);
  void propagateSrcInfo(raw_ostream &O, VASTExpr *V);

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

  SrcDelayInfo &getOrCreateSrcDelayInfo(VASTValue *Src) {
    return PathDelay[Src];
  }

  ExternalTimingAnalysis(VASTModule &VM, TimingNetlist::PathDelayInfo &PathInfo)
    : TimingEstimatorBase(PathInfo, TimingNetlist::ZeroDelay), VM(VM) {}

  bool analysisWithSynthesisTool();

  // Update the arrival time information of a VASTNode.
  void operator()(VASTNode *N) {
    VASTExpr *Expr = dyn_cast<VASTExpr>(N);

    if (Expr == 0) return;

    // Update the timing netlist according to the delay matrix.
    const SrcInfo &Srcs = DelayMatrix[Expr];
    // No need to build the source arrival times set if there is no source.
    if (Srcs.empty()) return;

    SrcDelayInfo &CurInfo = getOrCreateSrcDelayInfo(Expr);

    typedef SrcInfo::const_iterator iterator;
    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      float delay = *I->second;
      updateDelay(CurInfo, SrcEntryTy(I->first, delay_type(delay)));
    }

    // Also accumulate the delay from the operands.
    typedef VASTExpr::op_iterator op_iterator;
    for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      VASTValue *Op = VASTValPtr(*I).get();
      const SrcDelayInfo *OpSrcs = getPathTo(Op);

      if (OpSrcs == 0) continue;

      // Forward the arrival time information from the operands.
      // This make sure the arrival time from any register to current node are
      // no smaller than the arrival time from the same register.
      // Equivalent to accumulateDelayFrom(Op, Expr);
      for (src_iterator SI = OpSrcs->begin(), SE = OpSrcs->end(); SI != SE; ++SI)
        updateDelay(CurInfo, *SI);
    }

    assert(!CurInfo.empty() && "Unexpected empty arrival times set!");
  }
};
}

bool TimingNetlist::performExternalAnalysis(VASTModule &VM) {
  ExternalTimingAnalysis ETA(VM, PathInfo);

  // Run the synthesis tool to get the arrival time estimation.
  if (!ETA.analysisWithSynthesisTool()) return false;

  // Update the timing netlist.
  std::set<VASTExpr*> Visited;

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;

    if (Sel->isFUOutput()) {
      annotateDelay(Sel, ETA.getFUOutputDelay(Sel));
      continue;
    }

    typedef VASTSelector::iterator fanin_iterator;
    for (fanin_iterator SI = Sel->begin(), SE = Sel->end(); SI != SE; ++SI) {
      VASTLatch U = *SI;
      VASTValue *FI = VASTValPtr(U).get();

      // Visit the cone rooted on the fanin.
      if (VASTExpr *Expr = dyn_cast<VASTExpr>(FI))
        Expr->visitConeTopOrder(Visited, ETA);
      buildTimingPath(FI, Sel, delay_type(ETA.getSelDelay(Sel, FI)));

      VASTValue *Cnd = VASTValPtr(U.getGuard()).get();
      // Visit the cone rooted on the guarding condition.
      if (VASTExpr *Expr = dyn_cast<VASTExpr>(Cnd))
        Expr->visitConeTopOrder(Visited, ETA);
      buildTimingPath(Cnd, Sel, delay_type(ETA.getSelDelay(Sel, Cnd)));

      if (VASTValue *SlotActive = U.getSlotActive().get()) {
        // Visit the cone rooted on the ready signal.
        if (VASTExpr *Expr = dyn_cast<VASTExpr>(SlotActive))
          Expr->visitConeTopOrder(Visited, ETA);
        buildTimingPath(SlotActive, Sel,
                        delay_type(ETA.getSelDelay(Sel, SlotActive)));
      }
    }

    if (Sel->isSelectorSynthesized()) {
      typedef VASTSelector::ann_iterator ann_iterator;
      for (ann_iterator I = Sel->ann_begin(), E = Sel->ann_end(); I != E; ++I) {
        VASTValue *V = (*I)->getNode();
        // Visit the cone rooted on the ready signal.
        if (VASTExpr *Expr = dyn_cast<VASTExpr>(V))
          Expr->visitConeTopOrder(Visited, ETA);
        // FIXME: Get the delay from V to Sel!
        buildTimingPath(V, Sel, delay_type(0.0f));
      }
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
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    I->setPrintSelModule();
    I->printSelectorModule(Out);
  }

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

  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    I->setPrintSelModule(false);
}

void ExternalTimingAnalysis::writeProjectScript(raw_ostream &O,
                                                StringRef NetlistPath,
                                                StringRef ExtractScript)
                                                const {
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
  O <<"\"\n"
       //"set_global_assignment -name SDC_FILE @SDC_FILE@\n"
       "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
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
ExternalTimingAnalysis::buildPathInfoForCone(raw_ostream &O, VASTValue *Root) {
  // Do nothing if the cone is visited.
  if (DelayMatrix.count(Root)) return;

  VASTExpr *Expr = dyn_cast<VASTExpr>(Root);

  if (Expr == 0) {
    // Insert the trivial path information.
    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(Root))
      DelayMatrix[V].insert(std::make_pair(V, (float*)0));

    return;
  }

  typedef  VASTOperandList::op_iterator ChildIt;

  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;
  VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // All sources of this node is visited, now propagete the source information.
    if (It ==  Node->op_end()) {
      VisitStack.pop_back();

      propagateSrcInfo(O, Node);

#ifdef XDEBUG
      // Verify if we include all registers which are reachable to this node.
      std::set<VASTSeqValue*> SrcRegs;
      Node->extractSupportingSeqVal(SrcRegs);
      SrcRegs.erase(0);

      SrcInfo &Srcs = DelayMatrix[Node];

      typedef SrcInfo::iterator source_iterator;
      for (source_iterator SI = Srcs.begin(), SE = Srcs.end(); SI != SE; ++SI)
        SrcRegs.erase(dyn_cast_or_null<VASTSeqValue>(SI->first));

      assert(SrcRegs.empty() && "Some source register missed!");
#endif

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(ChildNode)) {
      DelayMatrix[V].insert(std::make_pair(V, (float*)0));
      continue;
    }

    if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(ChildNode)){
      // Mark the ChildNode as visited.
      bool inserted
        = DelayMatrix.insert(std::make_pair(SubExpr, SrcInfo())).second;

      // Do not visit the same node more than once.
      if (!inserted) continue;

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

void ExternalTimingAnalysis::propagateSrcInfo(raw_ostream &O, VASTExpr *V) {
  SrcInfo &PI = DelayMatrix[V];

  typedef VASTOperandList::op_iterator iterator;
  for (iterator I = V->op_begin(), E = V->op_end(); I != E; ++I) {
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
      if (V->hasName()) extractTimingForPath(O, V, Src, Idx);
    }
  }
}

static std::string GetSelectorCollection(VASTSelector *Sel) {
  const std::string Name(Sel->getName());
  return "[get_pins -compatibility_mode -nowarn \"" + Name + "_selector*\"];";
}

void
ExternalTimingAnalysis::extractSelectorDelay(raw_ostream &O, VASTSelector *Sel) {
  // Get the delay from the selector wire of the selector.
  O << "set dst " << GetSTACollection(Sel) << '\n';
  O << "set src " << GetSelectorCollection(Sel) << '\n';
  unsigned Idx = 0;
  SelectorDelay[Sel] = allocateDelayRef(Idx);
  extractTimingForPath(O, Idx);
  DEBUG(O << "post_message -type info \" selector -> "
          << Sel->getSTAObjectName() << " delay: $delay\"\n");
}

void
ExternalTimingAnalysis::extractFUOutputDelay(raw_ostream &O, VASTSelector *Sel) {
  assert(Sel->isFUOutput() && "Bad selector type!");

  if (VASTMemoryBus *Bus = dyn_cast<VASTMemoryBus>(Sel->getParent())) {
    O << "set dst [get_pins -compatibility_mode -nowarn "
      << Bus->getArrayName() << "*dataout*]\n"
      << "set src [get_keepers -nowarn " << Bus->getArrayName() << "*]\n";
    unsigned Idx = 0;
    BRAMOutputDelay[Bus->getNumber()] = allocateDelayRef(Idx);
    extractTimingForPath(O, Idx);
    DEBUG(O << "post_message -type info \" "
            << Sel->getSTAObjectName() << " ->  mem_fanout delay: $delay\"\n");
  }
}

void
ExternalTimingAnalysis::extractInterConnectDelay(raw_ostream &O,
                                                 VASTSelector *Sel,
                                                 VASTValue *From) {
  if (!isa<VASTExpr>(From) && !isa<VASTSeqValue>(From)) return;

  DelayMatrix[Sel][From] = SelectorDelay[Sel];
  return;
  // No need to calculate the interconnect delay more than once.
  if (P) return;

  O << "set src " << GetSTACollection(From) << "\n"
       "set delay 0.0\n"
       "foreach_in_collection node $src {\n"
       "  set edges [get_node_info $node -fanout_edges]\n"
       "  foreach edge $edges {\n"
       "    set dst_node [get_edge_info -dst $edge]\n"
       "    set dst_name [get_node_info -name $dst_node]\n"
       "    if { [string match " << Sel->getName() <<  "_selector* $dst_name] } {\n"
       "      set rr_delay [get_edge_info $edge -max -delay -rr]\n"
       "      if { $rr_delay > $delay } { set delay $rr_delay }\n"
       "      set rf_delay [get_edge_info $edge -max -delay -rf]\n"
       "      if { $rf_delay > $delay } { set delay $rf_delay }\n"
       "      set fr_delay [get_edge_info $edge -max -delay -fr]\n"
       "      if { $fr_delay > $delay } { set delay $fr_delay }\n"
       "      set ff_delay [get_edge_info $edge -max -delay -ff]\n"
       "      if { $ff_delay > $delay } { set delay $ff_delay }\n"
       "    }\n"
       "  }\n"
       "}\n";
  // Allocate the delay record.
  unsigned RefIdx = 0;
  P = allocateDelayRef(RefIdx);
  O << "puts $JSONFile \"" << RefIdx << " $delay\"\n";

  DEBUG(O << "post_message -type info \" "
          << Sel->getSTAObjectName() << " ->  " << From->getSTAObjectName()
          << " delay: $delay\"\n");
}

void ExternalTimingAnalysis::extractTimingForSelector(raw_ostream &O,
                                                      VASTSelector *Sel) {
  if (Sel->isFUOutput()) {
    extractFUOutputDelay(O, Sel);
    return;
  }

  extractSelectorDelay(O, Sel);

  typedef VASTSelector::iterator fanin_iterator;
  for (fanin_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;
    VASTValue *FI = VASTValPtr(U).get();
    // Visit the cone rooted on the fanin.
    buildPathInfoForCone(O, FI);
    extractInterConnectDelay(O, Sel, FI);

    VASTValue *Cnd = VASTValPtr(U.getGuard()).get();
    // Visit the cone rooted on the guarding condition.
    buildPathInfoForCone(O, Cnd);
    extractInterConnectDelay(O, Sel, Cnd);

    if (VASTValue *SlotActive = U.getSlotActive().get()) {
      buildPathInfoForCone(O, SlotActive);
      extractInterConnectDelay(O, Sel, SlotActive);
    }
  }

  // Also extract the arrival time for the synthesized selector.
  if (Sel->isSelectorSynthesized()) {
    buildPathInfoForCone(O, Sel->getFanin().get());
    extractInterConnectDelay(O, Sel, Sel->getFanin().get());
    buildPathInfoForCone(O, Sel->getGuard().get());
    extractInterConnectDelay(O, Sel, Sel->getGuard().get());
  }
}

void
ExternalTimingAnalysis::writeTimingExtractionScript(raw_ostream &O,
                                                    StringRef ResultPath) {
  // Print the critical path in the datapath to debug the TimingNetlist.
  O << "create_clock -name \"clk\" -period 1ns [get_ports {clk}]\n"
       "derive_pll_clocks -create_base_clocks\n"
       "derive_clock_uncertainty\n"
       "report_timing -from_clock { clk } -to_clock { clk }"
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

  // Write the project script.
  SmallString<256> PrjTcl(OutputDir);
  sys::path::append(PrjTcl, VM.getName() + ".tcl");

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
  //args.push_back("--64bit");
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
