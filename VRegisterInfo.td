//===- CTVRegisterInfo.td - CTV Register defs ----------*- tblgen -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//  Declarations that describe the CTV register file
//===----------------------------------------------------------------------===//

class VTMReg<string n> : Register<n> {
  let Namespace = "VTM";
}

//===----------------------------------------------------------------------===//
//  Registers
//===----------------------------------------------------------------------===//
// Result registers of function units.
def IR : VTMReg<"interface">;
def CR : VTMReg<"callee">;
def BR : VTMReg<"bram">;
def MUXR : VTMReg<"mux">;
def R : VTMReg<"r">;
def W : VTMReg<"w">;

def DR    : RegisterClass<"VTM", [i1, i8, i16, i32, i64], 64, (add R)>;
def Wire  : RegisterClass<"VTM", [i64], 64, (add W)>;

// Result register classes of function units.
def RINF    : RegisterClass<"VTM", [i1, i8, i16, i32, i64], 64, (add IR)>;
def RCFN    : RegisterClass<"VTM", [i1, i8, i16, i32, i64], 64, (add CR)>;
def RBRM    : RegisterClass<"VTM", [i1, i8, i16, i32, i64], 64, (add BR)>;

// Prebinded distributed (in time domain) multiplexer.
def RMUX    : RegisterClass<"VTM", [i1, i8, i16, i32, i64], 64, (add MUXR)>;
