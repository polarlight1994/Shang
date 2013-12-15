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

#include "vast/Passes.h"
#include "vast/Dataflow.h"
#include "vast/VASTModule.h"
#include "vast/VASTSubModules.h"
#include "vast/VASTMemoryBank.h"
#include "vast/STGDistances.h"
#include "vast/LuaI.h"

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
#include "llvm/Support/Timer.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "external-timing-analysis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumConstraintsWritten, "Number of multi-cycle constraints written");
STATISTIC(NumConesMerged, "Number of combinational cones merged for multi-cycle"
                          " constraints generation");
STATISTIC(NumQueriesWritten, "Number of path delay queries written");
STATISTIC(NumQueriesRead, "Number of path delay queries read");
static cl::opt<bool> Use64BitQuartus("vast-use-64bit-quartus",
  cl::desc("Use 64bit quartus to perform timing analysis"),
#ifdef _MSC_VER
  cl::init(false));
#else
  cl::init(true));
#endif

static cl::opt<bool>
EnablePAR("vast-external-enable-place-and-route",
  cl::desc("Enable place and route in external tool"),
  cl::init(true));

static cl::opt<bool>
EnableFastPAR("vast-external-enable-fast-place-and-route",
  cl::desc("Enable fast place and route in external tool"),
  cl::init(true));

static cl::opt<bool>
EnableTimingConstraint("vast-external-enable-timing-constraint",
  cl::desc("Use timing constraints to tweak the data path in external tool"),
  cl::init(true));

static cl::opt<unsigned>
ExternalToolTimeOut("vast-external-tool-time-out",
  cl::desc("Use set the timeout of external tool"),
  cl::init(3600 * 4));


static cl::opt<float>
ExpectedICDelayRatio("vast-external-tool-expected-ic-delay-ratio",
  cl::desc("Use set the interconnect delay ratio for the sdc constraints filter"),
  cl::init(0.0f));

//===----------------------------------------------------------------------===//
template<typename T>
static void reportFileOpenFailure(const T &FileName) {
  report_fatal_error(Twine("error opening file '") + FileName
                     + Twine("' for writing!\n"));
}

//===----------------------------------------------------------------------===//
namespace {
// The parser to read the path delay records generated by the extraction script.
class FileLexer {
  const char *CurPtr;
  const char *End;
  char CurChar;
  StringRef CurTok;

  StringRef getTok() {
    const char *TokStart = CurPtr;

    while (!isdigit(CurChar) && CurChar != '.' && CurChar != '-') {
      ++CurPtr;
      CurChar = *CurPtr;
      TokStart = CurPtr;
      if (CurPtr == End) return StringRef();
    }

    while (isdigit(CurChar) || CurChar == '.' || CurChar == '-') {
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

  float getFloat() {
    StringRef Delay = eatTok();
    DEBUG(dbgs() << "Delay: " << Delay << '\n');
    return strtod(Delay.data(), 0);
  }
};

struct ExternalTimingAnalysis {
  SmallString<256> OutputDir;
  VASTModule &VM;
  Dataflow *DF;
  STGDistances *Distances;

  // Data structure that explicitly hold the total delay and cell delay of a
  // datapath. Based on total delay and cell delay we can calculate the
  // corresponding wire delay.
  struct delay_type {
    float total_delay;
    float cell_delay;
    delay_type() : total_delay(0.0f), cell_delay(0.0f) {}

    bool operator < (const delay_type &RHS) const {
      return total_delay < RHS.total_delay;
    }
  };

  SpecificBumpPtrAllocator<delay_type> Allocator;

  std::vector<float*> DelayRefs;
  typedef std::map<VASTSeqValue*, delay_type*> SrcInfo;
  // The end-to-end (source register to destinate register) arrival time.
  typedef std::map<VASTSelector*, SrcInfo> SimpleArrivalInfo;
  SimpleArrivalInfo SimpleArrivals;

  // The (source register through combinational node to destinate register)
  // node arrival time
  typedef std::map<VASTExpr*, SrcInfo> HalfArrivialInfo;
  typedef std::map<VASTSelector*, HalfArrivialInfo> ComplexArrivialInfo;
  ComplexArrivialInfo ComplexArrivials;

  // The annotated expressions that are read by specificed VASTSeqOp
  typedef std::map<VASTSeqOp*, std::set<VASTExpr*> > AnnotoatedFaninsMap;
  AnnotoatedFaninsMap AnnotoatedFanins;

  // Test if a slot register can ever be set.
  std::map<BasicBlock*, std::vector<float*> > BBReachability;

  void extractSlotReachability(raw_ostream &TclO, VASTSelector *Sel,
                               delay_type *DelayRef);

  bool isBasicBlockUnreachable(ArrayRef<float*> SlotsStatus) const {
    for (unsigned i = 0; i < SlotsStatus.size(); ++i)
      if (*SlotsStatus[i] == 0.0f)
        return false;

    return true;
  }

  bool isBasicBlockUnreachable(BasicBlock *BB) const {
    typedef std::map<BasicBlock*, std::vector<float*> >::const_iterator iterator;
    iterator I = BBReachability.find(BB);

    // We do not know if the BB is reachable, it is safe to assume the BB is
    // reachable.
    if (I == BBReachability.end())
      return false;

    return isBasicBlockUnreachable(I->second);
  }

  std::string getNetlistPath() const {
    return getTmpFilePath(VM.getName() + ".sv");
  }

  std::string getSDCPath() const {
    return getTmpFilePath(VM.getName() + ".sdc");
  }

  std::string getResultPath() const {
    return getTmpFilePath(VM.getName() + ".json");
  }

  std::string getPlacementPath() const {
    return getTmpFilePath(VM.getName() + ".rgp");
  }

  std::string getDriverScriptPath() const {
    return getTmpFilePath(VM.getName() + ".tcl");
  }

  std::string getTmpFilePath(const Twine &FileName) const {
    std::string ErrorInfo;
    SmallString<256> FilePath(OutputDir);
    sys::path::append(FilePath, FileName);

    return FilePath.str();
  }

  raw_fd_ostream *createTmpFile(const std::string &FilePath) const {
    std::string ErrorInfo;

    errs() << "Writing '" << FilePath << "'...\n";
    raw_fd_ostream *NetlistO = new raw_fd_ostream(FilePath.c_str(), ErrorInfo);

    if (LLVM_UNLIKELY(!ErrorInfo.empty())) {
      reportFileOpenFailure(FilePath);
      return 0;
    }

    return NetlistO;
  }

  // Write the wrapper of the netlist.
  void writeNetlist() const;

  // Write the project file to perform the timing analysis.
  void writeMapDesignScript(raw_ostream &O) const;
  void writeFitDesignScript(raw_ostream &O, bool DontFit, bool EnableETA) const;
  void writeTimingAnalysisDriver(raw_ostream &O, bool PostMapOnly);
  void writeReadPlacementScript(raw_ostream &O) const;

  // Write the script to extract the timing analysis results from quartus.
  void buildDelayMatrix();
  void buildDelayMatrixForSelector(raw_ostream &TimingSDCO,
                                   VASTSelector *Sel);
  void writeDelayMatrixExtractionScript(raw_ostream &TclO, bool PAR,
                                        bool ZeroICDelay);
  void extractTimingForPath(raw_ostream &O, VASTSelector *Dst, VASTSeqValue *Src,
                            VASTExpr *Thu, delay_type *DelayRef, bool IsCellDelay);
  unsigned calculateCycles(VASTSeqValue *Src, VASTSeqOp *Op);

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

  delay_type *allocateDelayRef() {
    delay_type *P = new (Allocator.Allocate()) delay_type();
    // Don't forget to initialize the content!
    ++NumQueriesWritten;
    return P;
  }

  // Read the JSON file written by the timing extraction script.
  bool readTimingAnalysisResult();

  ExternalTimingAnalysis(VASTModule &VM, Dataflow *DF, STGDistances *Distences);

  bool analysisWithSynthesisTool();

  bool readRegionPlacement();

  void getPathDelay(const VASTLatch &L, VASTValPtr V,
                    std::map<VASTSeqValue*, delay_type> &Srcs);
};
}

void
ExternalTimingAnalysis::getPathDelay(const VASTLatch &L, VASTValPtr V,
                                     std::map<VASTSeqValue*, delay_type> &Srcs) {
  // Simply add the zero delay record if the fanin itself is a register.
  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V.get())) {
    if (!SV->isSlot() && !SV->isFUOutput()) {
      // Please note that this insertion may fail (V already existed), but it
      // does not hurt because here we only want to ensure the record exist.
      Srcs.insert(std::make_pair(SV, delay_type()));
      return;
    }
  }

  LeafSet Leaves;
  V->extractSupportingSeqVal(Leaves);
  if (Leaves.empty())
    return;

  VASTSelector *Sel = L.getSelector();

  SimpleArrivalInfo::const_iterator I = SimpleArrivals.find(Sel);
  assert(I != SimpleArrivals.end() && "End-to-End arrivial time not available!");
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
    delay_type &OldDelay = Srcs[Leaf];
    OldDelay = std::max(OldDelay, *J->second);
  }

  if (MissedLeaves.empty())
    return;

  // Get the delay through annotated nodes.
  VASTSeqOp *Op = L.Op;
  AnnotoatedFaninsMap::const_iterator J = AnnotoatedFanins.find(Op);
  assert(J != AnnotoatedFanins.end() && "Annotation node not available!");
  const std::set<VASTExpr*> &ThuNodes = J->second;

  ComplexArrivialInfo::const_iterator K = ComplexArrivials.find(Sel);
  assert(K != ComplexArrivials.end() && "Src-Thu-Dst arrivial information not found!");
  const HalfArrivialInfo &STArrivials = K->second;

  typedef std::set<VASTExpr*>::const_iterator thu_iterator;
  for (thu_iterator I = ThuNodes.begin(), E = ThuNodes.end(); I != E; ++I) {
    VASTExpr *Expr = *I;

    HalfArrivialInfo::const_iterator ThuI = STArrivials.find(Expr);
    // Sometimes the thu nodes are connected to other selectors that are
    // modified in current slot.
    if (ThuI == STArrivials.end())
      continue;

    const SrcInfo &Delays = ThuI->second;

    typedef SmallVector<VASTSeqValue*, 4>::iterator leaf_iterator;
    for (leaf_iterator I = MissedLeaves.begin(), E = MissedLeaves.end();
         I != E; ++I) {
      VASTSeqValue *Leaf = *I;
      src_iterator ThuSrcI = Delays.find(Leaf);
      if (ThuSrcI == Delays.end())
        continue;

      delay_type &OldDelay = Srcs[Leaf];
      OldDelay = std::max(OldDelay, *ThuSrcI->second);
    }
  }

#ifndef NDEBUG
  for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I)
    assert(Srcs.count(*I) && "Source delay missed!");
#endif
}

bool DataflowAnnotation::externalDelayAnnotation(VASTModule &VM) {
  ExternalTimingAnalysis ETA(VM, DF, Distances);

  // Run the synthesis tool to get the arrival time estimation.
  if (!ETA.analysisWithSynthesisTool()) return false;

  typedef VASTModule::seqop_iterator iterator;
  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;

    Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());

    // Nothing to do if Op does not have an underlying instruction.
    if (!Inst)
      continue;

    typedef std::map<VASTSeqValue*, ExternalTimingAnalysis::delay_type>
            ArrivalInfo;
    std::map<VASTSeqValue*, ExternalTimingAnalysis::delay_type> Srcs;
    VASTValPtr Guard = Op->getGuard();
    VASTSeqValue *SlotValue = Op->getSlot()->getValue();

    for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
      VASTLatch L = Op->getSrc(i);
      VASTSelector *Sel = L.getSelector();
      if (Sel->isTrivialFannin(L))
        continue;

      // Extract the delay from the fan-in and the guarding condition.
      VASTValPtr FI = L;
      ETA.getPathDelay(L, SlotValue, Srcs);
      ETA.getPathDelay(L, Guard, Srcs);
      ETA.getPathDelay(L, FI, Srcs);
    }

    typedef ArrivalInfo::iterator src_iterator;
    for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *Src = I->first;
      float delay = I->second.total_delay,
            ic_delay = delay - I->second.cell_delay;

      if (Src->isSlot()) {
        if (Src->getLLVMValue() == 0)
          continue;
      }

      annotateDelay(Op, Op->getSlot(), Src, delay, ic_delay);
    }
  }

  // Annotate the unreachable blocks.
  typedef Function::iterator bb_iterator;
  Function &F = VM.getLLVMFunction();
  for (bb_iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    if (ETA.isBasicBlockUnreachable(BB))
      DF->addUnreachableBlocks(BB);
  }

  // External timing analysis successfully completed.
  return true;
}

void ExternalTimingAnalysis::writeNetlist() const {
  OwningPtr<raw_fd_ostream> Out(createTmpFile(getNetlistPath()));

  // Read the result from the scripting engine.
  const char *FUTemplatePath[] = { "FUs", "CommonTemplate" };
  std::string FUTemplate = LuaI::GetString(FUTemplatePath);
  *Out << FUTemplate << '\n';

  // Write buffers to output
  VM.printModuleDecl(*Out);
  *Out << "\n\n";
  // Reg and wire
  *Out << "// Reg and wire decl\n";
  VM.printSignalDecl(*Out);
  *Out << "\n\n";
  // Datapath
  *Out << "// Datapath\n";
  VM.printDatapath(*Out);

  // Sequential logic of the registers.
  VM.printSubmodules(*Out);
  VM.printRegisterBlocks(*Out);

  *Out << "endmodule\n";
  (*Out).flush();
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
void ExtractTimingForPath(raw_ostream &O, unsigned RefIdx, bool HasThu) {
  O << "set delay \"No-path\"\n";
  O << "if {[get_collection_size $src] && [get_collection_size $dst]";
  if (HasThu)
    O << "&& [get_collection_size $thu]";
  O << "} {\n";
  // Use get_path instead of get_timing_path to get the longest delay paths
  // between arbitrary points in the netlist.
  // See "get_path -help" for more information.
  O << "  set paths [get_timing_paths  -from $src ";
  if (HasThu)
    O << "-through $thu";
  O << " -to $dst -nworst 1 -pairs_only]\n"
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

void
ExternalTimingAnalysis::extractTimingForPath(raw_ostream &O, VASTSelector *Dst,
                                             VASTSeqValue *Src, VASTExpr *Thu,
                                             delay_type *Delay,
                                             bool IsCellDelay) {
  O << "set src " << GetSTACollection(Src) << '\n';
  O << "set dst " << GetSTACollection(Dst) << '\n';
  if (Thu)
    O << "set thu " << GetSTACollection(Thu) << '\n';
  // if (Thu) O << "set thu " << GetSTACollection(Thu) << '\n';

  unsigned RefIndex = DelayRefs.size();
  DelayRefs.push_back(IsCellDelay ? & Delay->cell_delay : &Delay->total_delay);

  ExtractTimingForPath(O, RefIndex, Thu != 0);
  DEBUG(O << "post_message -type info \"" << Src->getSTAObjectName()
          << " -> " << Dst->getSTAObjectName() << " delay: $delay\"\n");
}

namespace {
struct ConstraintHelper {
  raw_ostream &O;
  VASTSelector *Sel;
  typedef std::set<VASTSeqValue*> LeafSet;
  typedef std::map<VASTExpr*, LeafSet> ConeSet;
  std::map<unsigned, ConeSet> CyclesMatrix;

  typedef std::map<VASTSeqValue*, unsigned> LeafCyclesDistribution;
  typedef std::map<VASTExpr*, LeafCyclesDistribution> ConeCyclesDistribution;
  ConeCyclesDistribution ConeCyclesDistributionMatrix;

  ConstraintHelper(raw_ostream &O, VASTSelector *Sel)
    : O(O), Sel(Sel) {}

  // Add source node and the number of avalaible cycles
  void addSource(VASTSeqValue *Leaf, VASTExpr *Thu, unsigned Cycles) {
    Cycles = std::max(Cycles, 1u);

    CyclesMatrix[Cycles][Thu].insert(Leaf);
    ConeCyclesDistributionMatrix[Thu][Leaf] = Cycles;
  }

  void generateConstraints() const {
    if (!EnableTimingConstraint)
      return;

    typedef std::map<unsigned, ConeSet>::const_iterator iterator;
    typedef ConeSet::const_iterator cone_iterator;
    std::vector<VASTExpr*> Roots, CurCones;

    for (iterator I = CyclesMatrix.begin(), E = CyclesMatrix.end(); I != E; ++I)
    {
      unsigned NumCycles = I->first;
      // No need to generate the single cycle constraints
      if (NumCycles <= 1)
        continue;

      const ConeSet &Cones = I->second;
      for (cone_iterator J = Cones.begin(), E = Cones.end(); J != E; ++J)
        Roots.push_back(J->first);

      while (!Roots.empty()) {
        VASTExpr *E = Roots.back();
        Roots.pop_back();

        // The current root may be merged.
        if (E == 0)
          continue;

        CurCones.push_back(E);
        for (unsigned i = 0, e = Roots.size(); i < e; ++i) {
          VASTExpr *&Root = Roots[i];

          // The current root may be merged.
          if (!(Root && isLegalToGroup(CurCones, Root, Cones, NumCycles)))
            continue;

          CurCones.push_back(Root);
          // Set the current array element to 0, so that we will not visit the
          // same root more than once.
          Root = 0;

          ++NumConesMerged;
        }

        generateConstraints(CurCones, NumCycles, Cones);
        CurCones.clear();
      }
    }
  }

  bool isLegalToGroup(ArrayRef<VASTExpr*> CurCones, VASTExpr *NewRoot,
                      const ConeSet &ConeSets, unsigned NumCycles) const {
    ConeSet::const_iterator I = ConeSets.find(NewRoot);
    assert(I != ConeSets.end() && "Cannot find leaves of current cone!");
    const LeafSet &LeavesOfNewRoot = I->second;

    // Do not generate the constraints that have more number of cycles than
    // the actual available cycles.
    for (unsigned i = 0; i < CurCones.size(); ++i) {
      VASTExpr *CurRoot = CurCones[i];

      if (haveSmallerCyclesConstraints(CurRoot, LeavesOfNewRoot, NumCycles))
        return false;

      ConeSet::const_iterator J = ConeSets.find(CurRoot);
      assert(J != ConeSets.end() && "Cannot find leaves of current cone!");
      const LeafSet &LeavesOfCurRoot = J->second;

      if (haveSmallerCyclesConstraints(NewRoot, LeavesOfCurRoot, NumCycles))
        return false;
    }

    return true;
  }

  bool haveSmallerCyclesConstraints(VASTExpr *Root, const LeafSet &Leaves,
                                    unsigned CurrentCycles) const {
    typedef LeafSet::const_iterator iterator;
    for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I)
      if (haveSmallerCyclesConstraints(Root, *I, CurrentCycles))
        return true;

    return false;
  }

  bool haveSmallerCyclesConstraints(VASTExpr *Root, VASTSeqValue *Leaf,
                                    unsigned CurrentCycles) const {
    typedef ConeCyclesDistribution::const_iterator iterator;
    iterator I = ConeCyclesDistributionMatrix.find(Root);
    assert(I != ConeCyclesDistributionMatrix.end()
           && "Cycles information is missed for current root!");

    const LeafCyclesDistribution &LeafCycles = I->second;
    LeafCyclesDistribution::const_iterator J = LeafCycles.find(Leaf);

    // If leaf is not reachable from current root, there is no different cycles
    // constraints.
    if (J == LeafCycles.end())
      return false;

    return J->second < CurrentCycles;
  }

  void generateConstraints(ArrayRef<VASTExpr*> CurCones, unsigned NumCycles,
                           const ConeSet &ConeSets) const {
    assert(NumCycles > 1 && "Unexpected number of cycles!");

    // Start the new constraint.
    O << "set_multicycle_path"
         " -to [get_keepers -nowarn {" << Sel->getSTAObjectName() <<  "}]";

    // Generate the through filter
    if (CurCones.size() > 1 || CurCones[0] != 0) {
      O << " -through [get_pins -compatibility_mode -nowarn {";

      for (unsigned i = 0; i < CurCones.size(); ++i)
        if (CurCones[i] != 0)
          O << ' ' << CurCones[i]->getSTAObjectName();

      O  << "}] ";
    }

    // Generate the from filter
    O << " -from [get_keepers -nowarn {";

    LeafSet Leaves;
    for (unsigned i = 0; i < CurCones.size(); ++i) {
      ConeSet::const_iterator I = ConeSets.find(CurCones[i]);
      assert(I != ConeSets.end() && "Cannot find leaves of current cone!");
      const LeafSet &CurLeaves = I->second;
      Leaves.insert(CurLeaves.begin(), CurLeaves.end());
    }

    assert(!Leaves.empty() && "Unexpected empty leaf set!");

    typedef LeafSet::const_iterator iterator;
    for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I)
      O << ' ' << (*I)->getSTAObjectName();

    O  << "}] ";

    O << " -setup -end " << NumCycles << '\n';
      ++NumConstraintsWritten;
  }
};
}

unsigned
ExternalTimingAnalysis::calculateCycles(VASTSeqValue *Src, VASTSeqOp *Op) {
  Dataflow::delay_type delay = DF->getDelay(Src, Op, Op->getSlot());

  float TotalDelay = delay.expected();
  float CellDelay = TotalDelay - delay.expected_ic_delay();
  // Calculate the expected delay according to the cell delay and the expected
  // % of interconnect delay.
  float ExpectedDelay = CellDelay / (1.0 - ExpectedICDelayRatio);

  return std::ceil(std::min(TotalDelay, ExpectedDelay));
}

void ExternalTimingAnalysis::extractSlotReachability(raw_ostream &TclO,
                                                     VASTSelector *Sel,
                                                     delay_type *DelayRef) {
  VASTSeqValue *V = Sel->getSSAValue();
  assert(V && "Slot enable value not defined?");
  BasicBlock *BB = dyn_cast_or_null<BasicBlock>(V->getLLVMValue());

  if (BB == 0)
    return;

  unsigned RefIndex = DelayRefs.size();
  DelayRefs.push_back(&DelayRef->total_delay);
  BBReachability[BB].push_back(&DelayRef->total_delay);

  TclO << "set slot " << GetSTACollection(Sel) << "\n"
          "if {[get_collection_size $slot] == 0} {\n"
          "  puts $JSONFile \"" << RefIndex << " 1.0\"\n"
          "}\n";
}

void ExternalTimingAnalysis::buildDelayMatrixForSelector(raw_ostream &TimingSDCO,
                                                         VASTSelector *Sel) {
  // Build the intersected fanins
  LeafSet AllLeaves, IntersectLeaves, CurLeaves;
  typedef LeafSet::iterator leaf_iterator;
  typedef VASTSelector::iterator fanin_iterator;
  DenseMap<VASTSlot*, VASTSeqOp*> SlotMap;
  DenseMap<VASTSeqValue*, unsigned> CycleConstraints;
  ConstraintHelper CH(TimingSDCO, Sel);

  for (fanin_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U))
      continue;

    VASTSeqOp *&Op = SlotMap[U.getSlot()];
    assert(Op == 0 && "Expected 1-to-1 mapping from VASTSlot to VASTSeqOp!");
    Op = U.Op;

    // Collect all leaves for current fanin.
    CurLeaves.clear();
    VASTValPtr FI = U;
    // No need to extract the leaves from the same node twice.
    FI->extractSupportingSeqVal(CurLeaves);
    U.getGuard()->extractSupportingSeqVal(CurLeaves);
    // Also extract the arrival time from the slot register.
    CurLeaves.insert(U.getSlot()->getValue());

    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *Leaf = *LI;

      if (!AllLeaves.insert(Leaf).second &&
          !Leaf->isSlot() && !Leaf->isFUOutput())
        IntersectLeaves.insert(Leaf);

      if (Leaf->isSlot() || Leaf->isFUOutput())
        continue;

      CycleConstraints[Leaf] = calculateCycles(Leaf, Op);
    }

    // Directly add the slot active to all leaves set.
    if (VASTValue *SlotActive = U.getSlotActive().get())
      SlotActive->extractSupportingSeqVal(AllLeaves);
  }

  for (leaf_iterator I = AllLeaves.begin(), E = AllLeaves.end(); I != E; ++I) {
    VASTSeqValue *Src = *I;

    if (IntersectLeaves.count(Src))
      continue;

    // Directly use the register-to-register delay.
    SimpleArrivals[Sel][Src] = allocateDelayRef();

    if (Src->isSlot() || Src->isFUOutput())
      continue;

    CH.addSource(Src, 0, CycleConstraints.lookup(Src));
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
        // Get the register to thu delay.
        delay_type *&P = ComplexArrivials[Sel][Expr][Leaf];
        if (P)
          continue;

        P = allocateDelayRef();
        CH.addSource(Leaf, Expr, calculateCycles(Leaf, Op));
      }

      AnnotoatedFanins[Op].insert(Expr);
    }
  }

  CH.generateConstraints();
}

void
ExternalTimingAnalysis::buildDelayMatrix() {
  OwningPtr<raw_fd_ostream> TimingSDCO(createTmpFile(getSDCPath()));

  *TimingSDCO << "create_clock -name \"clk\" -period "
              << format("%.2fns", VFUs::Period)
              << " [get_ports {clk}]\n"
                 "derive_pll_clocks -create_base_clocks\n"
                 "derive_clock_uncertainty\n";

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    buildDelayMatrixForSelector(*TimingSDCO, I);

  // Set the correct hold timing.
  *TimingSDCO << "set_multicycle_path -from [get_clocks {clk}] "
                 "-to [get_clocks {clk}] -hold -end 0\n";
}

bool ExternalTimingAnalysis::readTimingAnalysisResult() {
  // Read the timing analysis results.
  OwningPtr<MemoryBuffer> File;
  if (error_code ec = MemoryBuffer::getFile(getResultPath(), File)) {
    errs() <<  "Could not open input file: " <<  ec.message() << '\n';
    return false;
  }

  // Parse the file generated by quartus.
  FileLexer Parser(File->getBufferStart(), File->getBufferEnd());

  while (!Parser.finish()) {
    unsigned idx = Parser.getInteger();
    float delay = Parser.getFloat() / VFUs::Period;
    setDelay(idx, delay);
    ++NumQueriesRead;
  }

  return true;
}

ExternalTimingAnalysis::ExternalTimingAnalysis(VASTModule &VM, Dataflow *DF,
                                               STGDistances *Distences)
  : OutputDir(sys::path::parent_path(LuaI::GetString("RTLOutput"))),
    VM(VM), DF(DF), Distances(Distences) {
  sys::path::append(OutputDir, "TimingNetlist");
  bool Existed;
  sys::fs::create_directories(StringRef(OutputDir), Existed);
  (void)Existed;
}

void
ExternalTimingAnalysis::writeMapDesignScript(raw_ostream &O) const {
  const char *LUAPath[] = { "TimingAnalysis", "Device" };
  const std::string &DeviceName = LuaI::GetString(LUAPath);

  O << "load_package flow\n"
       "load_package report\n"
    << "project_new  -overwrite " << VM.getName() << " \n"
//       "set_global_assignment -name FAMILY \"Cyclone IV E\"\n"
       "set_global_assignment -name DEVICE " << DeviceName << "\n"
       "set_global_assignment -name TOP_LEVEL_ENTITY " << VM.getName() << "\n"
       "set_global_assignment -name SOURCE_FILE \"";
  O.write_escaped(getNetlistPath());
  O << "\"\n"
       "set_global_assignment -name SDC_FILE \"";
  O.write_escaped(getSDCPath());
  O << "\"\n";

  O << "set_global_assignment -name NUM_PARALLEL_PROCESSORS 1\n"
       "set_global_assignment -name HDL_MESSAGE_LEVEL LEVEL1\n"
       "set_global_assignment -name REMOVE_REDUNDANT_LOGIC_CELLS ON\n"
       "set_global_assignment -name SYNTH_MESSAGE_LEVEL LOW\n"
       //"set_global_assignment -name SYNTH_TIMING_DRIVEN_SYNTHESIS OFF\n"
       "export_assignments\n"
       // Start the processes.
       "execute_module -tool map\n";
}

void
ExternalTimingAnalysis::writeFitDesignScript(raw_ostream &O, bool PAR,
                                             bool EnableETA) const {
  O << "load_package incremental_compilation\n"
       "set_logiclock -enabled true ";
  if (!VM.hasBoundingBoxConstraint())
    O << "-auto_size true -floating true ";
  else
    O << "-origin X" << VM.getBBX() << "_Y" << VM.getBBY()
      << " -width " << VM.getBBWidth() << " -height " << VM.getBBHeight()
      << " -auto_size false -floating false ";
  O << "-region main\n"
       "set_logiclock_contents -region main -to " << VM.getName() << "\n"
       "set_global_assignment -name TIMEQUEST_REPORT_SCRIPT_INCLUDE_DEFAULT_ANALYSIS OFF\n"
       "export_assignments\n";

  if (!PAR)
    return;

  O << "execute_module -tool fit";
  if (EnableETA)
    O << " -arg --early_timing_estimate";
  O << '\n';
}

void ExternalTimingAnalysis::writeDelayMatrixExtractionScript(raw_ostream &O,
                                                              bool PAR,
                                                              bool ZeroICDelay) {
  // Create the timing netlist before we perform any analysis.
  O << "create_timing_netlist";
  if (!PAR) O << " -post_map";
  if (ZeroICDelay) O << " -zero_ic_delays";
  O << "\n"
    "read_sdc {" << getSDCPath() << "}\n"
    "update_timing_netlist\n";

  // Print the critical path in the datapath to debug the TimingNetlist.
  O << "report_timing -from_clock { clk } -to_clock { clk }"
       " -setup -npaths 1 -detail full_path -stdout\n";

  typedef SimpleArrivalInfo::const_iterator simple_path_iterator;
  typedef SrcInfo::const_iterator src_iterator;
  for (simple_path_iterator I = SimpleArrivals.begin(),
       E = SimpleArrivals.end(); I != E; ++I) {
    VASTSelector *Sel = I->first;

    const SrcInfo &Srcs = I->second;
    for (src_iterator J = Srcs.begin(), E = Srcs.end(); J != E; ++J) {
      VASTSeqValue *Leaf = J->first;
      extractTimingForPath(O, Sel, Leaf, 0, J->second, ZeroICDelay);
    }
  }

  typedef ComplexArrivialInfo::const_iterator complex_path_iterator;
  typedef HalfArrivialInfo::const_iterator half_path_iterator;
  for (complex_path_iterator I = ComplexArrivials.begin(),
       E = ComplexArrivials.end(); I != E; ++I) {
    VASTSelector *Sel = I->first;
    const HalfArrivialInfo &HalfArrivals = I->second;

    for (half_path_iterator J = HalfArrivals.begin(), E = HalfArrivals.end();
         J != E; ++J) {
      VASTExpr *Thu = J->first;
      const SrcInfo &Srcs = J->second;
      for (src_iterator K = Srcs.begin(), E = Srcs.end(); K != E; ++K) {
        VASTSeqValue *Leaf = K->first;
        extractTimingForPath(O, Sel, Leaf, Thu, K->second, ZeroICDelay);
      }
    }
  }

  // Extract the basic block reachability information if it is not yet extracted.
  if (BBReachability.empty()) {
    typedef VASTModule::selector_iterator iterator;
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
      VASTSelector *Sel = I;

      if (!Sel->isSlot())
        continue;

      extractSlotReachability(O, Sel, allocateDelayRef());
    }
  }

  O << "delete_timing_netlist\n\n";
}

void
ExternalTimingAnalysis::writeTimingAnalysisDriver(raw_ostream &O, bool PAR) {
  // Perform analysis to extract the delays.
  buildDelayMatrix();
  // Open the file for the extracted datapath arrival time.
  O << "export_assignments\n"
       "set JSONFile [open \"";
  O.write_escaped(getResultPath()) << "\" w+]\n";

  writeDelayMatrixExtractionScript(O, PAR, false);
  writeDelayMatrixExtractionScript(O, PAR, true);

  // Close the file object.
  O << "close $JSONFile\n";
}

void
ExternalTimingAnalysis::writeReadPlacementScript(raw_ostream &O) const {
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
    O.write_escaped(getPlacementPath());
    O << "\" w+]\n"
         "puts $RegionPlacementFile \"$origin,$width,$height \"\n";
    // Close the array and the file object.
    O << "close $RegionPlacementFile\n"
         "unload_report\n";
  }
  O << "project_close\n";
}

bool ExternalTimingAnalysis::analysisWithSynthesisTool() {
  std::string GroupName;
  if (TimePassesIsEnabled)
    GroupName = "External Timing Analysis";

  // Write the Nestlist and the wrapper.
  writeNetlist();

  std::string DriverScriptPath = getDriverScriptPath();
  {
    // Write the project script.
    OwningPtr<raw_fd_ostream> PrjTclO(createTmpFile(DriverScriptPath));
    writeMapDesignScript(*PrjTclO);
    writeFitDesignScript(*PrjTclO, EnablePAR, EnableFastPAR);
    writeTimingAnalysisDriver(*PrjTclO, EnablePAR);
    if (EnablePAR) writeReadPlacementScript(*PrjTclO);
  }

  const char *LUAPath[] = { "TimingAnalysis", "ExternalTool" };

  SmallString<256> quartus(LuaI::GetString(LUAPath));
  std::vector<const char*> args;

  args.push_back(quartus.c_str());
  if (Use64BitQuartus)
    args.push_back("--64bit");
  args.push_back("-t");
  args.push_back(DriverScriptPath.c_str());
  args.push_back(0);

  StringRef Empty;
  const StringRef *Redirects[] = { &Empty, &Empty, &Empty };
  errs() << "Running '" << quartus.str() << " ' program... ";
  {

    NamedRegionTimer T("External Tool Run Time", GroupName, TimePassesIsEnabled);
    std::string ErrorInfo;
    if (LLVM_UNLIKELY(sys::ExecuteAndWait(quartus, &args[0], 0, 0/*Redirects*/,
                                          ExternalToolTimeOut, 0, &ErrorInfo))) {
      errs() << "Error: " << ErrorInfo <<'\n';
      report_fatal_error("External timing analyze fail!\n");
      return false;
    }
  }
  errs() << " done. \n";

  {
    NamedRegionTimer T("Backannotation File IO", GroupName, TimePassesIsEnabled);
    if (LLVM_UNLIKELY(!readTimingAnalysisResult()))
      return false;

    if (EnablePAR && !VM.hasBoundingBoxConstraint())
      return readRegionPlacement();
  }

  return true;
}

bool ExternalTimingAnalysis::readRegionPlacement(){
  OwningPtr<MemoryBuffer> File;
  if (error_code ec = MemoryBuffer::getFile(getPlacementPath(), File)) {
    errs() <<  "Could not open input file: " <<  ec.message() << '\n';
    return false;
  }

  FileLexer Lexer(File->getBufferStart(), File->getBufferEnd());
  assert(!Lexer.finish() && "Bad placement line!");
  int BBX = Lexer.getInteger();
  assert(!Lexer.finish() && "Bad placement line!");
  int BBY = Lexer.getInteger();
  assert(!Lexer.finish() && "Bad placement line!");
  int BBWidth = Lexer.getInteger();
  assert(!Lexer.finish() && "Bad placement line!");
  int BBHeight = Lexer.getInteger();

  // Relax the placement constraints by 10% on both x and y axes.
  int XSlack = ((BBWidth + 9) / 10) * 1,
      YSlack = ((BBHeight + 9) / 10) * 1;

  const int DeviceWidth = 81, DeviceHeight = 67;

  BBX = std::min(DeviceWidth - (BBWidth + 2 * XSlack), BBX - XSlack);
  BBX = std::max(1, BBX);
  BBWidth += 2 * XSlack;
  BBWidth = std::min(DeviceWidth, BBWidth);

  BBY = std::min(DeviceHeight - (BBHeight + 2 * YSlack), BBY - YSlack);
  BBY = std::max(1, BBY);
  BBHeight += 2 * YSlack;
  BBHeight = std::min(DeviceHeight, BBHeight);

  VM.setBoundingBoxConstraint(BBX, BBY, BBWidth, BBHeight);

  return true;
}
