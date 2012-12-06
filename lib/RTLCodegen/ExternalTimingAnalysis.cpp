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
//
//===----------------------------------------------------------------------===//

#include "ExternalTimingAnalysis.h"

#include "llvm\Support\PathV1.h"
#include "llvm\Support\Program.h"

using namespace llvm;

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

void TimingNetlist::addInstrToDatapath(MachineInstr *MI) {
  // Can use add the MachineInstr to the datapath?
  if (buildDatapath(MI)) return;

  // Otherwise export the values used by this MachineInstr.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i){
    const MachineOperand &MO = MI->getOperand(i);

    // Only care about a use register.
    if (!MO.isReg() || MO.isDef() || MO.getReg() == 0)
      continue;

    // Try to export the value.
    exportValue(MO.getReg());
  }
}

template<typename T>
raw_ostream &printAsLHS(raw_ostream &O, T *V, unsigned UB, unsigned LB) {
  O << V->getName() << VASTValue::printBitRange(UB, LB, V->getBitWidth() > 1);

  return O;
}

template<bool OUTPUTPART, typename T>
static T *printScanChainLogic(raw_ostream &O, T *V, const VASTValue *LastV,
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

void TimingNetlist::writeNetlistWrapper(raw_ostream &O, const Twine &Name) {
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
                                       const sys::Path &NetlistPath) {
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
       "execute_module -tool sta\n"
       "project_close\n";
}

void TimingNetlist::runExternalTimingAnalysis(const Twine &Name) {
  std::string ErrorInfo;

  // Write the Nestlist and the wrapper.
  sys::Path Netlist = buildPath(Name, ".v");
  if (Netlist.empty()) return;

  errs() << "Writing '" << Netlist.str() << "'... ";

  raw_fd_ostream NetlistO(Netlist.c_str(), ErrorInfo);

  if (ErrorInfo.empty()) {
    // Write the netlist.
    writeVerilog(NetlistO, Name);

    NetlistO << '\n';

    // Also write the wrapper.
    writeNetlistWrapper(NetlistO, Name);

    NetlistO.close();
    errs() << " done. \n";
  } else {
    errs() << "error opening file '" << Netlist.str() << "' for writing!\n";
    return;
  }

  // TODO: Write the SDC and the delay query script.

  // Write the project script.
  sys::Path PrjTcl = buildPath(Name, ".tcl");
  if (PrjTcl.empty()) return;

  errs() << "Writing '" << PrjTcl.str() << "'... ";

  raw_fd_ostream PrjTclO(PrjTcl.c_str(), ErrorInfo);

  if (ErrorInfo.empty()) {
    writeProjectScript(PrjTclO, Name, Netlist);

    PrjTclO.close();
    errs() << " done. \n";
  } else {
    errs() << "error opening file '" << PrjTcl.str() << "' for writing!\n";
    return;
  }

  sys::Path quartus("C:/altera/12.1/quartus/bin64/quartus.exe");
  std::vector<const char*> args;

  args.push_back(quartus.c_str());
  args.push_back("-t");
  args.push_back(PrjTcl.c_str());
  args.push_back(0);

  errs() << "Running 'quartus' program... ";
  if (sys::Program::ExecuteAndWait(quartus, &args[0], 0, 0, 0, 0, &ErrorInfo)) {
    errs() << "Error: " << ErrorInfo <<'\n';
    return;
  }

  // Clean up.
  Netlist.eraseFromDisk();
  PrjTcl.eraseFromDisk();

  errs() << " done. \n";
}
