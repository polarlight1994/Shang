ModelsimGenTemplate = [=[
$('#')!/bin/bash
vdel -lib work -all
vlib work
vlog +define+quartus_synthesis $(RTLModuleName).sv
vlog INTF_$(RTLModuleName).v
sed -e 's/<half-period>/5/' DUT_TOP_tb.sv > DUT_TOP_tb_5ns.sv
vlog -sv DUT_TOP_tb_5ns.sv
vsim -t 1ps work.DUT_TOP_tb -c -do "run -all;quit -f"

]=]

ModelsimGenTemplatePostRoutedSim = [=[
$('#')!/bin/bash
cd ./simulation/modelsim
vdel -lib work -all
vlib work
vlog DUT_TOP.vo
vlog -sv ../../DUT_TOP_tb.sv
vsim -t 1ps -L cycloneive_ver -L altera_ver work.DUT_TOP_tb -c -do "do DUT_TOP_dump_all_vcd_nodes.tcl;run <run-time>ns;vcd flush;quit -f"

]=]

Passes.ModelsimGen = { FunctionScript = [=[
if Functions[FuncInfo.Name] ~= nil then
end
]=], GlobalScript =[=[
local preprocess = require "luapp" . preprocess

local ModelDoFile = assert(io.open (MODELDOFILE, "w+"))

local _, message = preprocess {input=ModelsimGenTemplate, output=ModelDoFile}
if message ~= nil then print(message) end
ModelDoFile:close()

ModelDoFile = assert(io.open (MODELDOFILE .. "_post_routed", "w+"))
local _, post_routed_message = preprocess {input=ModelsimGenTemplatePostRoutedSim, output=ModelDoFile}
if post_routed_message ~= nil then print(post_routed_message) end
ModelDoFile:close()
]=]}
