FUs.CommonTemplate =[=[

module shang_addc#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  input wire c,
  output wire[C_WIDTH-1:0] d
);
	assign d = a + b + c;
endmodule

module shang_mult#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
	assign c = a * b;
endmodule

module shang_shl#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
	assign c = a << b;
endmodule

module shang_sra#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
	assign c = $signed(a) >> b;
endmodule

module shang_srl#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire[C_WIDTH-1:0] c
);
	assign c = a >> b;
endmodule

module shang_sgt#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
	assign c = ($signed(a) >  $signed(b)) ? 1'b1 : 1'b0;
endmodule

module shang_sge#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
	assign c = ($signed(a) >=  $signed(b)) ? 1'b1 : 1'b0;
endmodule

module shang_ugt#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
	assign c = (a > b)  ? 1'b1 : 1'b0;
endmodule


module shang_uge#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  output wire c
);
	assign c = (a >= b)  ? 1'b1 : 1'b0;
endmodule
]=]

FUs.MemoryBus = { ReadLatency = 2, StartInterval=1, AddressWidth=ptr_size, DataWidth=64 }
