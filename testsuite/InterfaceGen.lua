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
  output wire[7:0] LED7,
  output wire succ,
  output wire fin
);

wire  [31:0]         return_value;
wire                 mem0en;
wire  [3:0]         mem0cmd;
wire  [31:0]         mem0addr;
wire                 mem0rdy;
wire  [7:0]          mem0be;
wire  [$(getGVBit(Num64GV)-1):0]         addr2R;   //////////////////////////////
wire  [7:0]         byteenable;
wire  [63:0]         data2R;
wire  [63:0]         q_i;
wire  [63:0]         mem0in;
wire                 wren;
wire  [63:0]        mem0out;
wire                start_N =~start;

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
  .addr2R(addr2R),
  .clk(clk),
  .mem0addr(mem0addr),
  .mem0cmd(mem0cmd),
  .mem0en(mem0en),
  .mem0rdy(mem0rdy),
  .mem0be(mem0be),
  .mem0out(mem0out),
  .mem0in(mem0in),
  .q_i(q_i),
  .data2R(data2R),
  .byen2R(byteenable),
  .rstN(rstN),
  .wren(wren),
  .fin(fin),
  .return_value(return_value),
  .LED7(LED7)
);

BRAM i2(
  .waddr(addr2R),
  .raddr(addr2R),
  .be(byteenable),
  .wdata(data2R),
  .we(wren),
  .clk(clk),
  .q(q_i)
);

endmodule

//-_-------------------------Interface module for Bram-----------------------------_-//
//-_-------------------------Interface module for Bram-----------------------------_-//
//-_-------------------------Interface module for Bram-----------------------------_-//
module Main2Bram(
  //_---------Signal from IP----------------------//
  input                   clk,
  input                   rstN,
  input                    fin,
  input                   mem0en,
  input       [3:0]       mem0cmd,
  input        [7:0]        mem0be,
  input       [31:0]       mem0addr,
  input        [63:0]        mem0out,//
  input       [31:0]      return_value,
  //--------Signal from Bram----------------------//
  input        [63:0]      q_i,
  //_-------Linking LED to show the activity-------//
  output reg  [7:0]        LED7,
  //--------Signal to IP--------------------------//
  output                  mem0rdy,
  output      [63:0]      mem0in,
  //--------Signal to Bram------------------------//
  output      [7:0]        byen2R,
  output        [$(getGVBit(Num64GV)-1):0]       addr2R,  ///////////////////////////////////
  output      [63:0]       data2R,
  output                   wren
  );
//-=======================================================================================
//Some declarn
//-=======================================================================================
//The process of Reading
//-=======================================================================================
parameter         S0 = 2'b00,
          S_wait0 = 2'b01,
          S_wait1 = 2'b11;
//-=======================================================================================
reg [1:0]              state;
reg [31:0]         addr2R_read;
reg                readrdy;
reg [7:0]          readbyte_en;
reg               rden;
//-=======================================================================================
//-Active signal start the read process
//-=======================================================================================
wire               readactive = mem0en&&(mem0cmd==0)? 1:0;
wire    [7:0]      mem0be_wire = readactive?  (mem0be << mem0addr[2:0]):8'b1111_1111;
wire    [63:0]    q;
reg  [63:0]    q_pipe;
//-=======================================================================================
assign          q = q_i >> {addr2R_read[2:0],3'b0};
//-=======================================================================================
assign          mem0in = q_pipe;

always@(posedge clk,negedge rstN)begin
  if(!rstN)begin
    q_pipe <= 0;
  end else begin
    q_pipe <= q ;
  end
end

// synthesis translate_off
integer MemAccessCycles = 0;
// synthesis translate_on

always@(posedge clk,negedge rstN)begin
  if(!rstN)begin
    state <= S0;
    addr2R_read <= 0;
    readrdy <= 0;
    readbyte_en <= 8'b1111_1111;
    rden <= 0;
  end else begin
    case(state)
      S0 :begin//Get the read or write data when mem0en turns to high
        if(readactive)begin
          addr2R_read <= mem0addr;
          state <= S_wait0;
          readbyte_en <= mem0be_wire;
                  rden <= 1;
          // synthesis translate_off
          ++MemAccessCycles;
          // synthesis translate_on
        end else begin
          addr2R_read <= 0;
          state <= S0;
          readbyte_en <= 8'b1111_1111;
          readrdy <= 0;
          rden <= 0;
        end
      end
      S_wait0 :begin
        state <= S_wait1;//Write process is less by 2 cycle to Read process
        readrdy <= 0;
      end
      S_wait1 :begin
        state <= S0;//Write process is less by 2 cycle to Read process
        readrdy <= 1;
        // synthesis translate_off
        ++MemAccessCycles;
        // synthesis translate_on
      end

      default : begin
        state <= S0;
      end
    endcase
  end
end

//-=======================================================================================
//The process of Writing
//
//-Active the wren signal
//-=======================================================================================
reg [31:0]       memwaddr_reg;
reg [7:0]       mem0be_reg;
reg              writeactive_reg;
reg [63:0]       writedata_reg;

//registering all the input for writes
always@(posedge clk,negedge rstN)begin
  if(!rstN)begin
    memwaddr_reg <= 0;
    mem0be_reg <= 0;
    writeactive_reg <= 0;
    writedata_reg <=0;
  end else begin
    memwaddr_reg <= mem0addr;
    mem0be_reg <= mem0be << mem0addr[2:0];
    writeactive_reg <= mem0en&&mem0cmd[0];
    writedata_reg <= (mem0out<<{mem0addr[2:0],3'b0});
  end
end

//connecting to the blockRAM
assign byen2R = mem0be_reg;
assign addr2R = memwaddr_reg[$(getGVBit(Num64GV)+2):3];
assign data2R = writedata_reg;
assign wren = writeactive_reg;
assign mem0rdy = (readrdy);

// synthesis translate_off
always@(posedge clk) begin
  if (writeactive_reg) ++MemAccessCycles;
end
// synthesis translate_on

//-=======================================================================================
//-=======================================================================================
//Return the value
//-=======================================================================================
always@(posedge clk,negedge rstN)begin
  if(!rstN)begin
    LED7 <= 8'b10101010;
  end else begin
    if(fin)begin
      if(return_value==0)begin
        LED7 <= 8'b11111111;
      end else begin
        LED7 <= 8'b00000000;
      end
    end
  end
end

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
