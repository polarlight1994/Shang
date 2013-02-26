
InterfaceGen =[=[

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
wire                 start_N =~start;

// The module successfully complete its execution if return_value is 0.
assign succ = ~(|return_value);

  main main_inst(
  .clk(clk),
  .rstN(rstN),
  .start(start_N),
  .fin(fin),
  .return_value(return_value)
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
]=]

Passes.InterfaceGen = { FunctionScript = [=[
table_name = {}
table_num = {}
LineTotal = {}
local preprocess = require "luapp" . preprocess

local IntfFile = assert(io.open (INTFFILE, "w+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=InterfaceGen, output=IntfFile}
if message ~= nil then
  print(message)
end
IntfFile:close()
]=], GlobalScript =[=[ ]=]}

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

import "DPI-C" function int raise (int sig);

always_comb begin
  if (!succ) begin
    $('$')display ("The result is incorrect!");
    // Abort.
    raise(6);
    $('$')stop;
  end

  if (fin) begin
    wfile = $('$')fopen("$(CounterFile)");
    $('$')fwrite (wfile,"$(RTLModuleName) hardware run cycles %0d %0d\n",cnt, 0);
    $('$')fclose(wfile);

    wtmpfile = $('$')fopen("$(BenchmarkCycles)","a");
    $('$')fwrite (wtmpfile,",\n{\"name\":\"$(RTLModuleName)\", \"total\": %0d, \"wait\": %0d}", cnt, 0);
    $('$')fclose(wtmpfile);
    $display("At %t the result is correct!", $('$')time());

    //$display("$(RTLModuleName) memory access cycles: %d", 0);
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

]=], GlobalScript =[=[
local tbFile = assert(io.open (TBFILE, "w+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=DUT_TB_Template, output=tbFile}
if message ~= nil then print(message) end
tbFile:close()
]=]}
