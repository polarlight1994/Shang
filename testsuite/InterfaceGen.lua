function getGVBit(Num)
  if Num <= 2 then        return 1
  elseif Num <= 4 then    return 2
  elseif Num <= 8 then    return 3
  elseif Num <= 16 then    return 4
  elseif Num <= 32 then    return 5
  elseif Num <= 64 then    return 6
  elseif Num <= 128 then    return 7
  elseif Num <= 256 then    return 8
  elseif Num <= 512 then    return 9
  elseif Num <= 1024 then    return 10
  elseif Num <= 2048 then    return 11
  elseif Num <= 4096 then    return 12
  elseif Num <= 8192 then    return 13
  elseif Num <= 16384 then    return 14
  else                    return 15
  end
end
InterfaceGen =[=[
#local table_size_tmp = # LineTotal
#local Num64GV = 0
#if table_size_tmp > 0 then Num64GV = LineTotal[table_size_tmp] + 1 end
///////////////////////////////////////////////////////////////////////
//------------------------Experiment module--------------------------//
///////////////////////////////////////////////////////////////////////
module DUT_TOP(
  input wire clk,
  input wire rstN,
  input wire start,
  output reg[7:0] LED7,
  output wire succ,
  output wire fin
);

wire  [31:0]         return_value;
wire                 mem0en;
wire  [3:0]          mem0cmd;
wire  [31:0]         mem0addr;
wire                 mem0rdy;
wire  [7:0]          mem0be;
wire  [63:0]         mem0in;
wire  [63:0]         mem0out;
wire                 start_N =~start;

// The module successfully complete its execution if return_value is 0.
assign succ = ~(|return_value);

$(RTLModuleName) $(RTLModuleName)_inst(
    .clk(clk),
    .rstN(rstN),
    .start(start_N),
    .fin(fin),
    .return_value(return_value),
    .mem0en(mem0en),
    .mem0cmd(mem0cmd),
    .mem0addr(mem0addr),
    .mem0in(mem0in),
    .mem0out(mem0out),
    .mem0be(mem0be),
    .mem0rdy(mem0rdy)
);

Main2Bram i1(
  .rstN(rstN),
  .clk(clk),
  .mem0addr(mem0addr),
  .mem0cmd(mem0cmd),
  .mem0en(mem0en),
  .mem0rdy(mem0rdy),
  .mem0be(mem0be),
  .mem0out(mem0out),
  .mem0in(mem0in)
);

  always@(posedge clk, negedge rstN) begin
    if(!rstN)begin
      LED7 <= 8'b10101010;
    end else begin
      if(fin)begin
        LED7 <= (|return_value) ? 8'b00000000 : 8'b11111111;
      end
    end
  end

endmodule

//-_-------------------------Interface module for Bram-----------------------------_-//
//-_-------------------------Interface module for Bram-----------------------------_-//
//-_-------------------------Interface module for Bram-----------------------------_-//
module Main2Bram(
  //_---------Signal from IP----------------------//
  input wire                   clk,
  input wire                   rstN,
  input wire                   mem0en,
  input wire       [3:0]       mem0cmd,
  input wire        [7:0]        mem0be,
  input wire       [31:0]       mem0addr,
  input wire        [63:0]        mem0out,
  //--------Signal to IP--------------------------//
  output reg                 mem0rdy,
  output wire     [63:0]      mem0in
  );

reg [31:0]       MemAddrPipe0Reg, MemAddrPipe1Reg;
reg [7:0]        MemBePipe0Reg;
reg              WEnPipe0Reg, REnPipe0Reg, REnPipe1Reg;
reg [63:0]       MemWDataPipe0Reg;
wire [63:0]      MemRDataPipe1Wire;
reg [63:0]       MemRDataPipe2Reg;
reg [2:0]        MemAddrPipe2Reg;

// Stage 1: registering all the input for writes
always@(posedge clk,negedge rstN)begin
  if(!rstN)begin
    MemAddrPipe0Reg <= 0;
    MemBePipe0Reg <= 0;
    WEnPipe0Reg <= 0;
    MemWDataPipe0Reg <=0;
  end else begin
    MemAddrPipe0Reg <= mem0addr;
    MemBePipe0Reg <= mem0be << mem0addr[2:0];
    WEnPipe0Reg <= mem0en & mem0cmd[0];
    REnPipe0Reg <= mem0en & ~mem0cmd[0];
    MemWDataPipe0Reg <= (mem0out<<{mem0addr[2:0],3'b0});
  end
end

// Stage 2: Access the block ram.
BRAM i2(
  .waddr(MemAddrPipe0Reg[$(getGVBit(Num64GV)+2):3]),
  .raddr(MemAddrPipe0Reg[$(getGVBit(Num64GV)+2):3]),
  .be(MemBePipe0Reg),
  .wdata(MemWDataPipe0Reg),
  .we(WEnPipe0Reg),
  .clk(clk),
  .q(MemRDataPipe1Wire)
);


always@(posedge clk,negedge rstN)begin
  if(!rstN)begin
    MemAddrPipe1Reg <= 0;
    REnPipe1Reg <= 0;
  end else begin
    MemAddrPipe1Reg <= MemAddrPipe0Reg;
    REnPipe1Reg <= REnPipe0Reg;
  end
end

// Stage 3: Generate the output.
always@(posedge clk,negedge rstN)begin
  if(!rstN)begin
    mem0rdy <= 0;
    MemRDataPipe2Reg <=0;
    MemAddrPipe2Reg <=0;
  end else begin
    mem0rdy <= REnPipe1Reg;
    MemAddrPipe2Reg <= MemAddrPipe1Reg[2:0];
    MemRDataPipe2Reg <= MemRDataPipe1Wire;
  end
end

assign mem0in = MemRDataPipe2Reg>> {MemAddrPipe2Reg[2:0],3'b0};

endmodule

]=]

Passes.InterfaceGen = { FunctionScript = [=[
if Functions[FuncInfo.Name] ~= nil then
end
]=], GlobalScript =[=[
table_name = {}
table_num = {}
LineTotal = {}
local preprocess = require "luapp" . preprocess
--FIXME: Simply load the script.
local _, message = preprocess {input=BlockRAMInitFileGenScript}
local IntfFile = assert(io.open (INTFFILE, "w+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=InterfaceGen, output=IntfFile}
if message ~= nil then
  print(message)
  print('BRAMGen')
end
IntfFile:close()
]=]}

BRAMGen = [=[
#local table_size_tmp = # LineTotal
#local Num64GV = 0
#if table_size_tmp > 0 then Num64GV = LineTotal[table_size_tmp] + 1 end
module BRAM
  $('#')(parameter int
    ADDR_WIDTH = $(getGVBit(Num64GV)),                       ///////////////////////////
    BYTE_WIDTH = 8,
    BYTES = 8,
    WIDTH = BYTES * BYTE_WIDTH
)
(
  input [ADDR_WIDTH-1:0] waddr,
  input [ADDR_WIDTH-1:0] raddr,
  input [BYTES-1:0] be,
  input [WIDTH-1:0] wdata,
  input we, clk,
  output reg [WIDTH - 1:0] q
);
  localparam int WORDS = 1 << ADDR_WIDTH ;
  reg [WIDTH-1:0] q_tmp;
  // use a multi-dimensional packed array to model individual bytes within the word
  logic [BYTES-1:0][BYTE_WIDTH-1:0] ram[0:WORDS-1];

  // Add the initial file in ram
  initial  begin
  $('$')readmemb("$(RTLModuleName)_BramInit.txt",ram);
  end

  always@(posedge clk) begin
    if(we) begin
    // edit this code if using other than four bytes per word
      if(be[0]) ram[waddr][0] <= wdata[7:0];
      if(be[1]) ram[waddr][1] <= wdata[15:8];
      if(be[2]) ram[waddr][2] <= wdata[23:16];
      if(be[3]) ram[waddr][3] <= wdata[31:24];
      if(be[4]) ram[waddr][4] <= wdata[39:32];
      if(be[5]) ram[waddr][5] <= wdata[47:40];
      if(be[6]) ram[waddr][6] <= wdata[55:48];
      if(be[7]) ram[waddr][7] <= wdata[63:56];
  end
    q <= ram[raddr];
  end
endmodule : BRAM

]=]

Passes.BRAMGen = { FunctionScript = [=[
if Functions[FuncInfo.Name] ~= nil then
end
]=], GlobalScript =[=[
table_name = {}
table_num = {}
LineTotal = {}
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=BlockRAMInitFileGenScript}
local BramFile = assert(io.open (BRAMFILE, "w+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=BRAMGen, output=BramFile}
if message ~= nil then
  print(message)
  print('BRAMGen')
end
BramFile:close()
]=]}

DUT_TB_Template = [=[
`timescale 1ns/1ps
module DUT_TOP_tb();
reg clk;
reg rstN;
reg start;
wire [7:0] LED7;
wire succ;
wire fin;
reg startcnt;

DUT_TOP i1 (
  .clk(clk),
  .rstN(rstN),
    .start(start),
  .LED7(LED7),
  .succ(succ),
  .fin(fin)
);
  integer wfile,wtmpfile;
initial begin
  clk = 0;
  rstN = 1;
  start = 0;
  startcnt = 0;
  $('#')<half-period>ns;
  $('#')1ns;
  rstN = 0;
  $('#')<half-period>ns;
  $('#')<half-period>ns;
  rstN = 1;
  $('#')<half-period>ns;
  $('#')<half-period>ns;
  start = 1;
  $('#')<half-period>ns;
  $('#')<half-period>ns;
  start = 0;
  startcnt = 1;
end

// Generate the 100MHz clock.
always $('#')<half-period>ns clk = ~clk;

reg [31:0] cnt = 0;

always_comb begin
  if (!succ) begin
    $('$')display ("The result is incorrect!");
    $('$')stop;
  end

  if (fin) begin
    wfile = $('$')fopen("$(CounterFile)");
    $('$')fwrite (wfile,"$(RTLModuleName) hardware run cycles %0d\n",cnt);
    $('$')fclose(wfile);

    wtmpfile = $('$')fopen("$(BenchmarkCycles)","a");
    $('$')fwrite (wtmpfile,",\n{\"name\":\"$(RTLModuleName)\", \"total\": %0d, \"wait\": 1}", cnt);
    $('$')fclose(wtmpfile);
    $display("At %t the result is correct!", $('$')time());
  
    //$display("$(RTLModuleName) memory access cycles: %d", DUT_TOP_tb.i1.i1.MemAccessCycles);
    $('$')stop;
  end
end

always@(posedge clk) begin
  if (startcnt) cnt <= cnt + 1;
  // Produce the heard beat of the simulation.
  if (cnt % 80 == 0) $('$')write(".");
  // Do not exceed 80 columns.
  if (cnt % 6400 == 0) $('$')write("%t\n", $('$')time());
end

endmodule
]=]

Passes.DUT_TB_Gen = { FunctionScript = [=[
if Functions[FuncInfo.Name] ~= nil then
end
]=], GlobalScript =[=[
local tbFile = assert(io.open (TBFILE, "w+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=DUT_TB_Template, output=tbFile}
if message ~= nil then print(message) end
tbFile:close()
]=]}
