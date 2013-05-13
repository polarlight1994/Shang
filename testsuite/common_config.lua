FUs.MemoryBus = { ReadLatency = 2, StartInterval=1, AddressWidth=ptr_size, DataWidth=64 }

-- Please note that the template of the block RAM is provided in <TargetPlatform>Common.lua
FUs.BRam = { StepsToWait =1, StartInterval=1, DataWidth = 64, InitFileDir = test_binary_root }

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

module shang_sel#(parameter A_WIDTH = 0, B_WIDTH = 0, C_WIDTH = 0, D_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  input wire[B_WIDTH-1:0] b,
  input wire c,
  output wire[D_WIDTH-1:0] d
);
	assign d = c ? a : b;
endmodule

module shang_reduction#(parameter A_WIDTH = 0, B_WIDTH = 0) (
  input wire[A_WIDTH-1:0] a,
  output wire b
);
	assign b = &a;
endmodule

module shang_selector#(parameter INPUTS = 4, WIDTH = 2)(
  input wire[INPUTS * WIDTH - 1 : 0]  inputs,
  input wire[INPUTS - 1 : 0]          sels,
  output wire               enable,
  output reg[WIDTH - 1 : 0] sel_output);

  assign enable = |sels;

  wire[WIDTH - 1 : 0]  expanded_inputs [INPUTS];
  genvar i;
  generate for(i = 0; i < INPUTS; i = i + 1) begin : EXPAND
    assign expanded_inputs[i] = inputs[(WIDTH * (i + 1) - 1):((WIDTH * i))];
  end
  endgenerate

  integer j;
  always @(*) begin
    sel_output = {WIDTH{1'b0}};
    for(j = 0; j < INPUTS; j = j + 1)
      sel_output = sel_output | ({WIDTH{sels[j]}} & expanded_inputs[j]);
  end

endmodule
]=]
