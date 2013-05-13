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

module shang_selector#(parameter integer INPUTS = 8, string SELSTR = "100101100100101100", integer SELS = 16, integer WIDTH = 8)(
  input wire[INPUTS * WIDTH - 1 : 0]  inputs,
  input wire[SELS - 1 : 0]            sel_cnds,
  input wire[SELS - 1 : 0]            sel_ens,
  output reg[WIDTH - 1 : 0] sel_output,
  output wire               en);

  wire[WIDTH - 1 : 0]  expanded_inputs [0:INPUTS - 1];
  genvar i;
  generate for(i = 0; i < INPUTS; i = i + 1) begin : EXPAND_INPUTS
    assign expanded_inputs[i] = inputs[(WIDTH * (i + 1) - 1):((WIDTH * i))];
  end
  endgenerate

  reg[INPUTS - 1:0] expanded_sels;
  
  integer j, k;
  always @(*) begin
    expanded_sels = {INPUTS{1'b0}};
    k = 0;
    for(j = 0; j < SELS; j = j + 1) begin
      expanded_sels[k] = expanded_sels[k] | (sel_cnds[j] & sel_ens[j]);
      if (SELSTR[SELS - j - 1] == 49 /*'1'*/) k = k + 1;
    end
  end

  integer l;
  always @(*) begin
    sel_output = {WIDTH{1'b0}};
    for(l = 0; l < INPUTS; l = l + 1) 
      sel_output = sel_output | ({WIDTH{expanded_sels[l]}} & expanded_inputs[l]);
  end

  assign en = |expanded_sels;

endmodule
]=]

FUs.MemoryBus = { ReadLatency = 2, StartInterval=1, AddressWidth=ptr_size, DataWidth=64 }

-- Please note that the template of the block RAM is provided in <TargetPlatform>Common.lua
FUs.BRam = { StepsToWait =1, StartInterval=1, DataWidth = 64, InitFileDir = test_binary_root }
