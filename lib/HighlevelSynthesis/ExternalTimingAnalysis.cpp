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
class FileLexer {
  const char *CurPtr;
  const char *End;
  char CurChar;
  StringRef CurTok;

  StringRef getTok() {
    const char *TokStart = CurPtr;

    while (!isdigit(CurChar) && CurChar != '.') {
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
  FileLexer(const char *Start, const char *End)
    : CurPtr(Start), End(End), CurChar(*Start) {
    // Initialize the first token.
    eatTok();
  }

  bool finish() { return CurPtr == End; }

  StringRef getWord() {
    return eatTok();
  }

  unsigned getInteger() {
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
  Dataflow *DF;
  SpecificBumpPtrAllocator<float> Allocator;
  std::vector<float*> DelayRefs;
  typedef std::map<VASTValue*, float*> SrcInfo;
  typedef std::map<VASTNode*, SrcInfo> PathInfo;
  PathInfo DelayMatrix;

  // The annotated expressions that are read by specificed VASTSeqOp
  typedef std::map<VASTSeqOp*, std::set<VASTExpr*> > AnnotoatedFaninsMap;
  AnnotoatedFaninsMap AnnotoatedFanins;

  // Write the wrapper of the netlist.
  void writeNetlist(raw_ostream &O) const;

  // Write the project file to perform the timing analysis.
  void writeProjectScript(raw_ostream &O, StringRef NetlistPath,
                          StringRef SDCPath, StringRef ExtractScript,
                          StringRef RegionPlacementFile) const;

  // Write the script to extract the timing analysis results from quartus.
  void writeTimingScript(raw_ostream &O, raw_ostream &TimingSDCO,
                         StringRef ResultPath);
  void extractTimingForSelector(raw_ostream &TclO, raw_ostream &TimingSDCO,
                                VASTSelector *Sel);

  typedef std::set<VASTSeqValue*> LeafSet;

  static bool contains(const SrcInfo &Srcs, const LeafSet &Leaves) {
    typedef LeafSet::iterator iterator;
    for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I)
      if (Srcs.count(*I))
        return false;

    return true;
  }

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

  ExternalTimingAnalysis(VASTModule &VM, Dataflow *DF);

  bool analysisWithSynthesisTool();

  bool readRegionPlacement(StringRef RegionPlacementPath);

  void getPathDelay(const VASTLatch &L, LeafSet &Leaves,
                    std::map<VASTSeqValue*, float> &Srcs);
};
}

void ExternalTimingAnalysis::getPathDelay(const VASTLatch &L, LeafSet &Leaves,
                                          std::map<VASTSeqValue*, float> &Srcs) {
  if (Leaves.empty())
    return;

  VASTSelector *Sel = L.getSelector();

  PathInfo::const_iterator I = DelayMatrix.find(Sel);
  assert(I != DelayMatrix.end() && "Fanin delay not available!");
  const SrcInfo &FIDelays = I->second;

  SmallVector<VASTSeqValue*, 4> MissedLeaves;

  typedef LeafSet::iterator iterator;
  typedef SrcInfo::const_iterator src_iterator;
  for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
    VASTSeqValue *Leaf = *I;
    src_iterator J = FIDelays.find(Leaf);

    // If there is more than one paths between Leaf and selector, the delay
    // is not directly available.
    if (J == FIDelays.end()) {
      MissedLeaves.push_back(Leaf);
      continue;
    }

    // Otherwise Update the delay.
    float &OldDelay = Srcs[Leaf];
    OldDelay = std::max(OldDelay, *J->second);
  }

  if (MissedLeaves.empty())
    return;

  // Get the delay through annotated nodes.
  VASTSeqOp *Op = L.Op;
  AnnotoatedFaninsMap::const_iterator J = AnnotoatedFanins.find(Op);
  assert(J != AnnotoatedFanins.end() && "Annotation node not available!");
  const std::set<VASTExpr*> &ThuNodes = J->second;

  typedef std::set<VASTExpr*>::const_iterator thu_iterator;
  for (thu_iterator I = ThuNodes.begin(), E = ThuNodes.end(); I != E; ++I) {
    VASTExpr *Expr = *I;

    PathInfo::const_iterator SelI = DelayMatrix.find(Sel);
    assert(SelI != DelayMatrix.end() && "Fanin delay not available!");
    const SrcInfo &Delays = SelI->second;
    SrcInfo::const_iterator SelThuI = Delays.find(Expr);
    // Sometimes the thu nodes are connected to other selectors that are
    // modified in current slot.
    if (SelThuI == Delays.end())
      continue;

    // Get the delay from leaves to expr, and then build the full path delay by
    // add delay from leaf to expr, delay from expr to the selector together.
    PathInfo::const_iterator SrcI = DelayMatrix.find(Expr);
    assert(SrcI != DelayMatrix.end() && "Fanin delay not available!");
    const SrcInfo &LeavesDelays = SrcI->second;

    typedef SmallVector<VASTSeqValue*, 4>::iterator leaf_iterator;
    for (leaf_iterator I = MissedLeaves.begin(), E = MissedLeaves.end();
         I != E; ++I) {
      VASTSeqValue *Leaf = *I;
      src_iterator ThuSrcI = LeavesDelays.find(Leaf);
      if (ThuSrcI == LeavesDelays.end())
        continue;

      float &OldDelay = Srcs[Leaf];
      OldDelay = std::max(OldDelay, *ThuSrcI->second + *SelThuI->second);
    }
  }

#ifndef NDEBUG
  for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I)
    assert(Srcs.count(*I) && "Source delay missed!");
#endif
}

bool DataflowAnnotation::externalDelayAnnotation(VASTModule &VM) {
  ExternalTimingAnalysis ETA(VM, DF);

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

    std::map<VASTSeqValue*, float> Srcs;
    ExternalTimingAnalysis::LeafSet Leaves, CndLeaves;
    Op->getGuard()->extractSupportingSeqVal(CndLeaves);

    for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
      VASTLatch L = Op->getSrc(i);
      VASTSelector *Sel = L.getSelector();
      if (Sel->isTrivialFannin(L))
        continue;

      VASTValPtr FI = L;
      Leaves.clear();
      FI->extractSupportingSeqVal(Leaves);
      set_union(Leaves, CndLeaves);

      // Extract the delay from the fan-in and the guarding condition.
      ETA.getPathDelay(L, Leaves, Srcs);
    }

    typedef std::map<VASTSeqValue*, float>::iterator src_iterator;
    for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *Src = I->first;
      float delay = I->second;
      annotateDelay(Op, Op->getSlot(), Src, delay);
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
                                           StringRef ExtractScript,
                                           StringRef RegionPlacementFile) const {
  const char *LUAPath[] = { "TimingAnalysis", "Device" };
  const std::string &DeviceName = getStrValueFromEngine(LUAPath);

  O << "load_package flow\n"
       "load_package report\n"
       "load_package incremental_compilation\n"
    << "project_new  -overwrite " << VM.getName() << " \n"
//       "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE " << DeviceName << "\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << VM.getName() << "\n"
       "set_global_assignment -name SOURCE_FILE \"";
  O.write_escaped(NetlistPath);
  O << "\"\n"
       "set_global_assignment -name SDC_FILE \"";
  O.write_escaped(SDCPath);
  O << "\"\n";

  O << "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
       //"set_global_assignment -name SYNTH_TIMING_DRIVEN_SYNTHESIS OFF\n"
       "set_global_assignment -name TIMEQUEST_REPORT_SCRIPT_INCLUDE_DEFAULT_ANALYSIS OFF\n"
       "export_assignments\n"
       // Start the processes.
       "execute_module -tool map\n"
       "set_logiclock -enabled true ";
  if (!VM.hasBoundingBoxConstraint())
    O << "-auto_size true -floating true ";
  else
    O << "-origin X" << VM.getBBX() << "_Y" << VM.getBBY()
      << " -width " << VM.getBBWidth() << " -height " << VM.getBBHeight()
      << " -auto_size false -floating false ";
  O << "-region main\n"
       "set_logiclock_contents -region main -to " << VM.getName() << "\n"
       "export_assignments\n"
       "execute_module -tool fit -arg --early_timing_estimate\n"
       "execute_module -tool sta -args {--report_script \"";
  O.write_escaped(ExtractScript);
  O << "\"}\n";

  // Get the regional placement if we do not have the bounding box constraint.
  if (!VM.hasBoundingBoxConstraint()) {
    O << "load_report\n"
         "set panel {Fitter||Resource Section||LogicLock Region Resource Usage}\n"
         "set id [get_report_panel_id $panel]\n"
         "set origin [get_report_panel_data -col 2 -row_name {    -- Origin} -id $id]\n"
         "set height [get_report_panel_data -col 2 -row_name {    -- Height} -id $id]\n"
         "set width [get_report_panel_data -col 2 -row_name {    -- Width} -id $id]\n"
         "post_message -type info $origin\n"
         "post_message -type info $height\n"
         "post_message -type info $width\n"
         "set RegionPlacementFile [open \"";
    O.write_escaped(RegionPlacementFile);
    O << "\" w+]\n"
         "puts $RegionPlacementFile \"$origin,$width,$height \"\n";
    // Close the array and the file object.
    O << "close $RegionPlacementFile\n";
         "unload_report\n";
  }
  O << "project_close\n";
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

static void
GenerateTimingConstraints(raw_ostream &O, VASTSelector *Src, VASTSelector *Dst,
                          VASTValue *Thu, unsigned Cycles) {
  O << "set src " << GetSTACollection(Src) << '\n';
  O << "set dst " << GetSTACollection(Dst) << '\n';
  if (Thu)
    O << "set thu " << GetSTACollection(Thu) << '\n';

  O << "set_multicycle_path -from $src ";
  if (Thu)
    O << "-through $thu ";
  O << "-to $dst -setup -end " << Cycles << '\n';
}

void ExternalTimingAnalysis::extractTimingForSelector(raw_ostream &TclO,
                                                      raw_ostream &TimingSDCO,
                                                      VASTSelector *Sel) {
  // Build the intersected fanins and the esitmated delays.
  std::map<DataflowValue, float> EstimatedDelays;
  LeafSet AllLeaves, IntersectLeaves, CurLeaves;
  typedef LeafSet::iterator leaf_iterator;
  typedef VASTSelector::iterator fanin_iterator;
  DenseMap<VASTSlot*, VASTSeqOp*> SlotMap;

  for (fanin_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U))
      continue;

    VASTSeqOp *&Op = SlotMap[U.getSlot()];
    assert(Op == 0 && "Expected 1-to-1 mapping from VASTSlot to VASTSeqOp!");
    Op = U.Op;

    // Collect all leaves for current fanin.
    CurLeaves.clear();
    U->extractSupportingSeqVal(CurLeaves);
    U.getGuard()->extractSupportingSeqVal(CurLeaves);
    // Also extract the arrival time from the slot register.
    // CurLeaves.insert(U.getSlot()->getValue());

    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *Leaf = *LI;
      if (!AllLeaves.insert(Leaf).second && !Leaf->isSlot() && !Leaf->isFUOutput())
        IntersectLeaves.insert(Leaf);

      // Get the delay from last generation.
      float delay = DF->getDelay(Leaf, U.Op, U.getSlot());

      if (delay == 0.0f)
        continue;

      float &OldDelay = EstimatedDelays[Leaf];
      OldDelay = std::max(OldDelay, delay);
    }

    // Directly add the slot active to all leaves set.
    if (VASTValue *SlotActive = U.getSlotActive().get())
      SlotActive->extractSupportingSeqVal(AllLeaves);
  }

  // Extract end-to-end delay.
  for (leaf_iterator I = AllLeaves.begin(), E = AllLeaves.end(); I != E; ++I) {
    VASTSeqValue *Src = *I;

    if (IntersectLeaves.count(Src))
      continue;

    unsigned Idx = 0;
    // Directly use the register-to-register delay.
    DelayMatrix[Sel][Src] = allocateDelayRef(Idx);
    extractTimingForPath(TclO, Sel, Src, Idx);

    // Generate the timing constraints
    std::map<DataflowValue, float>::iterator J = EstimatedDelays.find(Src);
    if (J != EstimatedDelays.end()) {
      // Use a slightly tighter constraint to tweak the timing.
      unsigned Cycles = std::max<float>(ceil(J->second - 0.5f), 1.0f);
      if (Cycles > 1)
        GenerateTimingConstraints(TimingSDCO, Src->getSelector(), Sel, 0, Cycles);
    }
  }

  // Extract path delay in details for leaves that reachable to different fanins
  if (IntersectLeaves.empty())
    return;

  // Build the fanin map, extract the src -> thu -> dst delay.
  typedef VASTSelector::ann_iterator ann_iterator;
  for (ann_iterator I = Sel->ann_begin(), E = Sel->ann_end(); I != E; ++I) {
    ArrayRef<VASTSlot*> Slots(I->second);
    // Get the thu node of the path.
    VASTExpr *Expr = dyn_cast<VASTExpr>(I->first);

    if (Expr == 0)
      continue;

    assert(Expr->isTimingBarrier() && "Unexpected VASTExpr type!");

    CurLeaves.clear();
    Expr->extractSupportingSeqVal(CurLeaves);
    set_intersect(CurLeaves, IntersectLeaves);
    // Ignore the thu node if it is not reachable from the intersect leaves.
    if (CurLeaves.empty())
      continue;

    for (unsigned i = 0; i < Slots.size(); ++i) {
      VASTSeqOp *Op = SlotMap.lookup(Slots[i]);
      // For guarding conditions, we are 
      if (Op == 0)
        continue;
      
      for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
           LI != LE; ++LI) {
        VASTSeqValue *Leaf = *LI;
        unsigned Idx = 0;
        // Get the register to thu delay.
        float *&P = DelayMatrix[Expr][Leaf];
        if (P)
          continue;

        P = allocateDelayRef(Idx);
        extractTimingForPath(TclO, Expr, Leaf, Idx);

        // Also generate the constraint.
        float delay = DF->getDelay(Leaf, Op, Op->getSlot());
        unsigned Cycles = std::max<float>(ceil(delay - 0.5f), 1.0f);
        if (Cycles > 1)
          GenerateTimingConstraints(TimingSDCO, Leaf->getSelector(), Sel, Expr, Cycles);
      }

      // Get the thu to register delay.
      unsigned Idx = 0;
      DelayMatrix[Sel][Expr] = allocateDelayRef(Idx);
      extractTimingForPath(TclO, Sel, Expr, Idx);

      AnnotoatedFanins[Op].insert(Expr);
    }
  }
}

void
ExternalTimingAnalysis::writeTimingScript(raw_ostream &TclO,
                                          raw_ostream &TimingSDCO,
                                          StringRef ResultPath) {
  TimingSDCO << "create_clock -name \"clk\" -period "
             << format("%.2fns", VFUs::Period)
             << " [get_ports {clk}]\n"
                "derive_pll_clocks -create_base_clocks\n"
                "derive_clock_uncertainty\n";

  // Print the critical path in the datapath to debug the TimingNetlist.
  TclO << "report_timing -from_clock { clk } -to_clock { clk }"
         " -setup -npaths 1 -detail full_path -stdout\n"
  // Open the file and start the array.
       "set JSONFile [open \"";
  TclO.write_escaped(ResultPath);
  TclO << "\" w+]\n";

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    extractTimingForSelector(TclO, TimingSDCO, I);

  // Close the array and the file object.
  TclO << "close $JSONFile\n";

  // Set the correct hold timing.
  TimingSDCO << "set_multicycle_path -from [get_clocks {clk}] "
                "-to [get_clocks {clk}] -hold -end 0\n";
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
  FileLexer Parser(File->getBufferStart(), File->getBufferEnd());

  while (!Parser.finish()) {
    unsigned idx = Parser.getInteger();
    float delay = Parser.getPathDelay() / VFUs::Period;
    setDelay(idx, delay);
    ++NumQueriesRead;
  }

  return true;
}

ExternalTimingAnalysis::ExternalTimingAnalysis(VASTModule &VM, Dataflow *DF)
  : VM(VM), DF(DF) {}

bool ExternalTimingAnalysis::analysisWithSynthesisTool() {
  SmallString<256> OutputDir
    = sys::path::parent_path(getStrValueFromEngine("RTLOutput"));
  sys::path::append(OutputDir, "TimingNetlist");
  bool Existed;
  sys::fs::create_directories(StringRef(OutputDir), Existed);
  (void)Existed;

  std::string ErrorInfo;

  SmallString<256> TimingExtractResult(OutputDir);
  sys::path::append(TimingExtractResult, VM.getName() + "_result.json");

  // Write the SDC and the delay query script.
  SmallString<256> TimingExtractTcl(OutputDir);
  sys::path::append(TimingExtractTcl, VM.getName() + "_extract.tcl");
  errs() << "Writing '" << TimingExtractTcl.str() << "'... ";
  raw_fd_ostream TimingTclO(TimingExtractTcl.c_str(), ErrorInfo);
  if (!ErrorInfo.empty())  return exitWithError(TimingExtractTcl);
  // Write the SDC for the project.
  SmallString<256> TimingSDC(OutputDir);
  sys::path::append(TimingSDC, VM.getName() + ".sdc");
  errs() << "Writing '" << TimingSDC << "'... ";
  raw_fd_ostream TimingSDCO(TimingSDC.c_str(), ErrorInfo);
  if (!ErrorInfo.empty())  return exitWithError(TimingSDC);
  writeTimingScript(TimingTclO, TimingSDCO, TimingExtractResult);
  TimingTclO.close();
  TimingSDCO.close();
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

  SmallString<256> RegionPlacement(OutputDir);
  sys::path::append(RegionPlacement, VM.getName() + ".rgp");

  // Write the project script.
  SmallString<256> PrjTcl(OutputDir);
  sys::path::append(PrjTcl, VM.getName() + ".tcl");
  errs() << "Writing '" << PrjTcl.str() << "'... ";
  raw_fd_ostream PrjTclO(PrjTcl.c_str(), ErrorInfo);
  if (!ErrorInfo.empty())  return exitWithError(PrjTcl);
  writeProjectScript(PrjTclO, Netlist, TimingSDC, TimingExtractTcl,
                     RegionPlacement);
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

  if (!VM.hasBoundingBoxConstraint())
    return readRegionPlacement(RegionPlacement);

  return true;
}

bool ExternalTimingAnalysis::readRegionPlacement(StringRef RegionPlacementPath){
  OwningPtr<MemoryBuffer> File;
  if (error_code ec = MemoryBuffer::getFile(RegionPlacementPath, File)) {
    errs() <<  "Could not open input file: " <<  ec.message() << '\n';
    return false;
  }

  FileLexer Lexer(File->getBufferStart(), File->getBufferEnd());
  assert(!Lexer.finish() && "Bad placement line!");
  unsigned BBX = Lexer.getInteger();
  assert(!Lexer.finish() && "Bad placement line!");
  unsigned BBY = Lexer.getInteger();
  assert(!Lexer.finish() && "Bad placement line!");
  unsigned BBWidth = Lexer.getInteger();
  assert(!Lexer.finish() && "Bad placement line!");
  unsigned BBHeight = Lexer.getInteger();

  VM.setBoundingBoxConstraint(BBX, BBY, BBWidth, BBHeight);

  return true;
}
