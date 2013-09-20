FUs.CommonTemplate =[=[
module shang_addc#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  input wire c,
  output wire[C_WIDTH-1:0] d
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
  (*keep*) wire[A_WIDTH-1:0] kc = c;
	assign d = ka + kb + kc;
endmodule

module shang_mult#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = ka * kb;
endmodule

module shang_shl#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = ka << kb;
endmodule

module shang_sra#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = $signed(ka) >>> kb;
endmodule

module shang_srl#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = ka >> kb;
endmodule

module shang_sgt#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = ($signed(ka) >  $signed(kb)) ? 1'b1 : 1'b0;
endmodule

module shang_sge#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = ($signed(ka) >=  $signed(kb)) ? 1'b1 : 1'b0;
endmodule

module shang_ugt#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = (ka > kb)  ? 1'b1 : 1'b0;
endmodule

module shang_uge#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
  (*keep*) wire[A_WIDTH-1:0] ka = a;
  (*keep*) wire[A_WIDTH-1:0] kb = b;
	assign c = (ka >= kb)  ? 1'b1 : 1'b0;
endmodule

module shang_rand#(parameter WIDTH = 0) (
  input wire[WIDTH-1:0] a,
  output wire b
);
  (*keep*) wire[WIDTH-1:0] ka = a;
	assign b = &ka;
endmodule

module shang_rxor#(parameter WIDTH = 0) (
  input wire[WIDTH-1:0] a,
  output wire b
);
  (*keep*) wire[WIDTH-1:0] ka = a;
	assign b = ^ka;
endmodule
]=]

FUs.MemoryBus = { ReadLatency = 2, StartInterval=1, AddressWidth=ptr_size, DataWidth=64 }
