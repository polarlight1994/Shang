#!/usr/bin/env python

# The classes defined in this file represent the teststeps,
# includes:
#  The high-level synthesis step
#  The simulation step
#    The hybrid simulation step
#    The pure hardware simulation step
#    The post-fitted simulation step
#  The synthesis step
#    The altera synthesis step

# At the same time, the teststep provides:
#  The configuration data about the step
#  The function to drive the test
#  The function to parse the output

from jinja2 import Environment, FileSystemLoader, Template

import os, sys, json, re
from datetime import datetime

import drmaa

# Initialize the gridengine
Session = drmaa.Session()
Session.initialize()

FamilyNames = { "CycloneII" : "Cyclone II", "CycloneIV" : "Cyclone IV E", "StratixIV" : "Stratix IV" }
FamilyDevices = { "CycloneII" : "EP2C70F896C6", "CycloneIV" : "EP4CE75F29C6", "StratixIV" : "EP4SGX530KH40C2" }

# Base class of test step.
class TestStep :

  HybridSim = 'hybrid_sim'
  PureHWSim = 'pure_hw_sim'
  AlteraSyn = 'altera_syn'

  def __init__(self, config):
    self.__dict__.update(config)
    #
    self.config_template_env = Environment()
    self.config_template_env.filters['joinpath'] = lambda list: os.path.join(*list)
    # jobid of the step
    self.jobid = 0
    # The path to the stdout and stderr logfile.
    self.stdout = ''
    self.stderr = ''
    # The results dict, for debug use
    self.results = {}

  def __getitem__(self, key):
    return self.__dict__[key]

  #def __setitem__(self, key, value):
  #  self.__dict__[key] = value

  # Create the test environment

  # Run the step

  # Parse the test result

  # Optionally, lauch the subtests

  def generateFileFromTemplate(self, template_str, output_file_path) :
    self.config_template_env.from_string(template_str).stream(self.__dict__).dump(output_file_path)

  def submitResults(self, connection) : pass

  def getStepDesc(self) :
    return ' '.join([self.test_name, self.step_name, str(self.option)]).replace(':', '-')

  def jobStatus(self) :
    status = Session.jobStatus(self.jobid)
    if status == drmaa.JobState.DONE or status == drmaa.JobState.FAILED:
      retval = Session.wait(self.jobid, drmaa.Session.TIMEOUT_WAIT_FOREVER)
      if not retval.hasExited or retval.exitStatus != 0 :
        if self.test_name in self.xfails :
          return 'xfailed'
        else :
          return 'failed'

      return 'passed'

    return 'running'

  def getStepResult(self, status) :
    return  {
              'test_name' : self.test_name,
              'parameter' : json.dumps(self.option),
              'stdout' :  self.stdout,
              'stderr' : self.stderr,
              'test_file' : self.test_file,
              'synthesis_config_file' : self.synthesis_config_file,
              'status' : status
            }

  def dumplog(self) :
    print "stdout of", self.test_name, "begin"
    with open(self.stdout, "r") as logfile:
      sys.stdout.write(logfile.read())
    print "stdout of", self.test_name, "end"

    print "stderr of", self.test_name, "begin"
    with open(self.stderr, "r") as logfile:
      sys.stdout.write(logfile.read())
    print "stderr of", self.test_name, "end"

  def generateSubTests(self) :
    return []

# High-level synthesis step.
class HLSStep(TestStep) :
  step_name = 'high-level synthesis'

  def __init__(self, config):
    TestStep.__init__(self, config)

  def prepareTest(self) :
    # Create the local folder for the current test.
    self.hls_base_dir = os.path.join(os.path.dirname(self.test_file),
                           self.test_name,
                           datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    os.makedirs(self.hls_base_dir)

    #Generate the HLS config.
    self.synthesis_config_file = os.path.join(self.hls_base_dir, 'test_config.lua')
    self.generateFileFromTemplate('''-- Initialize the global variables.
ptr_size = {{ ptr_size }}
test_binary_root = [[{{ hls_base_dir }}]]
InputFile = [[{{ test_file }}]]
RTLOutput = [[{{ [hls_base_dir, test_name + ".sv"]|joinpath }}]]
MCPDataBase =  [[{{ [hls_base_dir, test_name + ".sql"]|joinpath }}]]
MainSDCOutput =  [[{{ [hls_base_dir, test_name + ".sdc"]|joinpath }}]]
--MainDelayVerifyOutput = [[{{ MAIN_DELAY_VERIFY_SRC }}]]
SoftwareIROutput =  [[{{ [hls_base_dir, test_name + "_soft.ll"]|joinpath }}]]
IFFileName =  [[{{ [hls_base_dir, test_name + "_if.cpp"]|joinpath }}]]
RTLModuleName = [[{{ [test_name, "_RTL_DUT"]|join }}]]

Functions.{{ hardware_function }} = RTLModuleName

local FMAX = {{ fmax }}
PERIOD = 1000.0 / FMAX
FUs.Period = PERIOD

dofile([[{{ [config_dir, 'common_config.lua']|joinpath }}]])

-- Define some function
dofile([[{{ [config_dir, 'FuncDefine.lua']|joinpath }}]])
dofile([[{{ [config_dir, 'AddModules.lua']|joinpath }}]])
-- load platform information script
dofile([[{{ [config_dir, 'AlteraCommon.lua']|joinpath }}]])
dofile([[{{ [config_dir, 'Altera4LUTFUs.lua']|joinpath }}]])
dofile([[{{ [config_dir, 'EP4CE75F29C6.lua']|joinpath }}]])

-- ExternalTool.Path = [[@QUARTUS_BIN_DIR@/quartus_sh]]

{% if hardware_function == 'main' %}
Misc.RTLGlobalScript = [=[
RTLGlobalCode = FUs.CommonTemplate
]=]

{% else %}

-- Load ip module and simulation interface script.
dofile([[{{ [config_dir, 'SCIfCodegen.lua']|joinpath }}]])

--Code for globalvariable symbols.
RTLGlobalTemplate = [=[
/* verilator lint_off DECLFILENAME */
/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */

`ifdef quartus_synthesis
// FIXME: Parse the address from the object file.
#local addr = 0

#for k,v in pairs(GlobalVariables) do
`define gv$(k) $(addr)
#addr = addr + 8
#end

`else
#for k,v in pairs(GlobalVariables) do
#if v.AddressSpace == 0 then
import "DPI-C" function chandle vlt_$(escapeNumber(k))();
`define gv$(k) vlt_$(escapeNumber(k))()
#end
#end
`endif
]=]

Misc.RTLGlobalScript = [=[
local preprocess = require "luapp" . preprocess
RTLGlobalCode, message = preprocess {input=RTLGlobalTemplate}
if message ~= nil then print(message) end

RTLGlobalCode = RTLGlobalCode .. FUs.CommonTemplate
]=]
{% endif %}
''', self.synthesis_config_file)

  # Run the test
  def runTest(self) :
    # Create the HLS job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getStepDesc()
    jt.remoteCommand = 'timeout'
    jt.args = ['%ds' % self.hls_timeout, self.shang, self.synthesis_config_file, '-stats',
               '-timing-model=%(timing_model)s' % self.option,
               '-vast-disable-mux-slack=%(vast_disable_mux_slack)s' % self.option,
               '-shang-enable-mux-pipelining=%(shang_enable_mux_pipelining)s' % self.option,
               '-shang-baseline-scheduling-only=%(shang_baseline_scheduling_only)s' % self.option,
               '-shang-enable-memory-optimization=%(shang_enable_memory_optimization)s' % self.option,
               '-shang-enable-memory-partition=%(shang_enable_memory_partition)s' % self.option,
               '-shang-enable-pre-schedule-lut-mapping=%(shang_enable_pre_schedule_lut_mapping)s' % self.option,
               '-shang-enable-register-sharing=%(shang_enable_register_sharing)s' % self.option,
               '-shang-selector-ignore-trivial-loops=true',
               '-shang-selector-ignore-x-fanins=true'
              ]

    #Set up the correct working directory and the output path
    jt.workingDirectory = os.path.dirname(self.synthesis_config_file)

    self.stdout = os.path.join(self.hls_base_dir, 'hls.stdout')
    jt.outputPath = ':' + self.stdout

    self.stderr = os.path.join(self.hls_base_dir, 'hls.stderr')
    jt.errorPath = ':' + self.stderr
    jt.joinFiles=True

    jt.nativeSpecification = '-q %s' % self.sge_queue

    print "Submitted", self.getStepDesc()
    #Submit the job.
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def generateSubTests(self) :
    #If test type == hybrid simulation
    if self.mode == TestStep.HybridSim :
      return self.generateHybridSim()
    elif self.mode == TestStep.PureHWSim or self.mode == TestStep.AlteraSyn :
      return self.generatePureHWSim()

    return []

  def generateHybridSim(self) :
    return [ HybridSimStep(self) ]

  def generatePureHWSim(self) :
    return [ PureHWSimStep(self) ]

# The test step for hybrid simulation.
class HybridSimStep(TestStep) :
  step_name = 'software-hardware co-simulation'

  def __init__(self, hls_step):
    TestStep.__init__(self, hls_step.__dict__)
    self.results.update(hls_step.results)

  def prepareTest(self) :
    self.hybrid_sim_base_dir = os.path.join(self.hls_base_dir, 'hybrid_sim')
    os.makedirs(self.hybrid_sim_base_dir)

    #Generate the hybrid simulation script.
    self.hybrid_sim_script = os.path.join(self.hls_base_dir, 'hybrid_sim.sge')
    self.generateFileFromTemplate('''#!/bin/bash
#$ -S /bin/bash

export VERILATOR_ROOT=$(dirname $(dirname {{ verilator }}))
export SYSTEMC={{ systemc }}
export LD_LIBRARY_PATH=$SYSTEMC/lib-linux{{ ptr_size }}:$LD_LIBRARY_PATH

# Generate native code for the software part.
llc={{ llc }}
{{ llc }} -march={{ llc_march}} \
          {{ [hls_base_dir, test_name + "_soft.ll"]|joinpath }} \
          -filetype=obj \
          -mc-relax-all \
          -o {{ [hybrid_sim_base_dir, test_name + "_soft.o"]|joinpath }} \
|| exit 1

# Generate cpp files from the rtl
{{ verilator }} {{ [hls_base_dir, test_name + ".sv"]|joinpath }} \
                -Wall --sc -D__DEBUG_IF +define+__VERILATOR_SIM \
                --top-module {{ [test_name, "_RTL_DUT"]|join }} \
|| exit 1

# Compile the verilator objects
make -C "{{ hybrid_sim_base_dir }}/obj_dir/"\
      -j -f "V{{ [test_name, "_RTL_DUT"]|join }}.mk" \
     "V{{ [test_name, "_RTL_DUT"]|join }}__ALL.a" \
|| exit 1

make -C "{{ hybrid_sim_base_dir }}/obj_dir/" \
     -j -f "V{{ [test_name, "_RTL_DUT"]|join }}.mk" \
     {{ [hls_base_dir, test_name + "_if.o"]|joinpath }} "verilated.o" \
|| exit 1

# Generate the testbench executables.
g++ -L$SYSTEMC/lib-linux{{ ptr_size }}  -lsystemc \
    {{ [hls_base_dir, test_name + "_if.o"]|joinpath }} \
    {{ [hybrid_sim_base_dir, test_name + "_soft.o"]|joinpath }} \
    {{ [hybrid_sim_base_dir, "obj_dir", 'V' + test_name + "_RTL_DUT__ALL*.o"]|joinpath }} \
    {{ [hybrid_sim_base_dir, "obj_dir", "verilated.o"]|joinpath }} \
    -o {{ [hls_base_dir, test_name + "_systemc_testbench"]|joinpath }} \
|| exit 1

# Run the systemc testbench
timeout 1200s {{ [hls_base_dir, test_name + "_systemc_testbench"]|joinpath }} > hardware.out \
|| exit 1

# Compare the hardware output with the standar output
{{ lli }} {{ test_file }} > expected.output || exit 1

[ -f hardware.output ]  && exit 1
diff expected.output hardware.out || exit 1
    ''', self.hybrid_sim_script)

  def runTest(self) :
    # Create the hybrid simulation job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getStepDesc()
    jt.remoteCommand = 'bash'
    jt.args = [ self.hybrid_sim_script ]
    #Set up the correct working directory and the output path
    jt.workingDirectory = self.hybrid_sim_base_dir
    self.stdout = os.path.join(self.hybrid_sim_base_dir, 'hybrid_sim.output')
    jt.outputPath = ':' + self.stdout
    self.stderr = os.path.join(self.hybrid_sim_base_dir, 'hybrid_sim.stderr')
    jt.errorPath = ':' + self.stderr
    jt.joinFiles=True

    jt.nativeSpecification = '-q %s' % self.sge_queue

    print "Submitted", self.getStepDesc()
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)


class PureHWSimStep(TestStep) :
  step_name = 'hardware simulation'

  def __init__(self, hls_step):
    TestStep.__init__(self, hls_step.__dict__)
    self.results.update(hls_step.results)

  def prepareTest(self) :
    self.pure_hw_sim_base_dir = os.path.join(self.hls_base_dir, 'pure_hw_sim')
    os.makedirs(self.pure_hw_sim_base_dir)
    # Generate the testbench

    self.generateFileFromTemplate('''`timescale 1ns/1ps
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

  // integer wfile,wtmpfile;
  initial begin
    clk = 0;
    rstN = 1;
    start = 0;
    startcnt = 0;
    #{{ (1000.0 / fmax) / 2 }}ns;
    #1ns;
    rstN = 0;
    #{{ (1000.0 / fmax) / 2 }}ns;
    #{{ (1000.0 / fmax) / 2 }}ns;
    rstN = 1;
    #{{ (1000.0 / fmax) / 2 }}ns;
    #{{ (1000.0 / fmax) / 2 }}ns;
    start = 1;
    $display ("Start at %t!", $time());
    #{{ (1000.0 / fmax) / 2 }}ns;
    #{{ (1000.0 / fmax) / 2 }}ns;
    start = 0;
    startcnt = 1;
  end

  // Generate the 100MHz clock.
  always #{{ (1000.0 / fmax) / 2 }}ns clk = ~clk;

  reg [31:0] cnt = 0;

  import "DPI-C" function int raise (int sig);

  integer cntfile;

  always_comb begin
    if (fin) begin
      if (!succ) begin
        $display ("The result is incorrect!");
        // Abort.
        raise(6);
        $stop;
      end

      cntfile = $fopen("cycles.rpt");
      $fwrite (cntfile, "%0d\\n",cnt);
      $fclose(cntfile);

      // cnt
      $stop;
    end
  end

  always@(posedge clk) begin
    if (startcnt) cnt <= cnt + 1;
    // Produce the heard beat of the simulation.
    if (cnt % 80 == 0) $write(".");
    // Do not exceed 80 columns.
    if (cnt % 6400 == 0) $write("%t\\n", $time());
  end

endmodule''', os.path.join(self.pure_hw_sim_base_dir, 'DUT_TOP_tb.sv'))

    #Generate the simulation script.
    self.pure_hw_sim_script = os.path.join(self.pure_hw_sim_base_dir, 'pure_hw_sim.sge')
    self.generateFileFromTemplate('''#!/bin/bash
#$ -S /bin/bash

export PATH=/nfs/app/altera/modelsim_ase_12_x64/modelsim_ase/bin/:$PATH

vlib work || exit 1
vlog -sv {{ [hls_base_dir, test_name + ".sv"]|joinpath }} || exit 1
vlog -sv DUT_TOP_tb.sv || exit 1
vsim -t 1ps work.DUT_TOP_tb -c -do "run -all;quit -f" || exit 1

# cycles.rpt will only generate when the simulation produce a correct result.
[ -f cycles.rpt ] || exit 1
''', self.pure_hw_sim_script)

  def runTest(self) :
    # Create the simulation job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getStepDesc()
    jt.remoteCommand = 'timeout'
    jt.args = [ '%ds' % self.sim_timeout, 'bash', self.pure_hw_sim_script ]
    #Set up the correct working directory and the output path
    jt.workingDirectory = self.pure_hw_sim_base_dir
    self.stdout = os.path.join(self.pure_hw_sim_base_dir, 'pure_hw_sim.output')
    jt.outputPath = ':' + self.stdout
    self.stderr = os.path.join(self.pure_hw_sim_base_dir, 'pure_hw_sim.stderr')
    jt.errorPath = ':' + self.stderr
    jt.joinFiles=True

    jt.nativeSpecification = '-q %s' % self.sge_queue

    print "Submitted", self.getStepDesc()
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def submitResults(self, connection) :
    with open(os.path.join(self.pure_hw_sim_base_dir, 'cycles.rpt')) as cycles_rpt:
      num_cycles = int(cycles_rpt.read())
      self.results["cycles"] = num_cycles
      connection.execute("INSERT INTO simulation(name, parameter, cycles) VALUES (:test_name, :parameter, :cycles)",
                         {"test_name" : self.test_name,  "parameter" : json.dumps(self.option), "cycles": num_cycles})

  def generateSubTests(self) :
    #If test type == hybrid simulation
    if self.mode == TestStep.AlteraSyn :
      return [ AlteraSynStep(self) ]

    return []

class AlteraSynStep(TestStep) :
  step_name = 'altera synthesis'

  def __init__(self, hls_step):
    TestStep.__init__(self, hls_step.__dict__)
    self.results.update(hls_step.results)
    self.require_license = (self.option['device_family'] == 'StratixIV')

    #Use full version for stratix devices.
    self.quartus_bin = '/nfs/app/altera/quartus12.1x64_%s/quartus/bin/' % ('full' if self.require_license else 'web')

  def prepareTest(self) :
    self.altera_synthesis_base_dir = os.path.join(self.hls_base_dir, 'altera_synthesis')
    os.makedirs(self.altera_synthesis_base_dir)
    # Generate the testbench

    self.altera_synthesis_script = os.path.join(self.altera_synthesis_base_dir, 'setup_prj.tcl')
    self.generateFileFromTemplate('''# Load necessary package.
load_package flow
load_package report

exec python {{ [config_dir, "altera_sdc_generator.py"]|joinpath }} --sql {{ [hls_base_dir, test_name + ".sql"]|joinpath }} --sdc {{ [hls_base_dir, test_name + ".sdc"]|joinpath }} --period {{ 1000.0 / fmax}} --factor {{ option['shang_constraints_factor'] }}

project_new {{ test_name }} -overwrite

set_global_assignment -name FAMILY "Cyclone IV E"
set_global_assignment -name DEVICE EP4CE75F29C6

set_global_assignment -name TOP_LEVEL_ENTITY main
set_global_assignment -name SOURCE_FILE {{ [hls_base_dir, test_name + ".sv"]|joinpath }}
set_global_assignment -name SDC_FILE {{ [hls_base_dir, test_name + ".sdc"]|joinpath }}

set_global_assignment -name RESERVE_ALL_UNUSED_PINS "AS INPUT TRI-STATED"
set_global_assignment -name RESERVE_ASDO_AFTER_CONFIGURATION "AS OUTPUT DRIVING AN UNSPECIFIED SIGNAL"
set_global_assignment -name RESERVE_ALL_UNUSED_PINS_NO_OUTPUT_GND "AS INPUT TRI-STATED"

#Power estimation settings
set_global_assignment -name EDA_SIMULATION_TOOL "ModelSim-Altera (Verilog)"
set_global_assignment -name EDA_OUTPUT_DATA_FORMAT "VERILOG HDL" -section_id eda_simulation
set_global_assignment -name EDA_TEST_BENCH_DESIGN_INSTANCE_NAME DUT_TOP_tb -section_id eda_simulation
set_global_assignment -name EDA_WRITE_NODES_FOR_POWER_ESTIMATION ALL_NODES -section_id eda_simulation
set_global_assignment -name EDA_MAP_ILLEGAL_CHARACTERS ON -section_id eda_simulation
set_global_assignment -name EDA_TIME_SCALE "1 ps" -section_id eda_simulation
set_global_assignment -name EDA_ENABLE_GLITCH_FILTERING ON -section_id eda_simulation

set ENABLE_PHYSICAL_SYNTHESIS "OFF"

source {{ [config_dir, 'quartus_compile.tcl']|joinpath }}
execute_module -tool sta -args {--report_script {{ [config_dir, 'extract_timing.tcl']|joinpath }} }

source {{ [config_dir, 'report_json_data.tcl']|joinpath }}

#Write the netlist
execute_module -tool eda

project_close
''', self.altera_synthesis_script)

  def runTest(self) :
    # Create the simulation job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getStepDesc()
    jt.remoteCommand = 'timeout'
    jt.args = [ '%ds' % self.syn_timeout, os.path.join(self.quartus_bin, 'quartus_sh'), '--64bit', '-t',  self.altera_synthesis_script] #self.syn_timeout
    #Set up the correct working directory and the output path
    jt.workingDirectory = self.altera_synthesis_base_dir
    self.stdout = os.path.join(self.altera_synthesis_base_dir, 'altera_synthesis.output')
    jt.outputPath = ':' + self.stdout
    self.stderr = os.path.join(self.altera_synthesis_base_dir, 'altera_synthesis.stderr')
    jt.errorPath = ':' + self.stderr
    jt.joinFiles=True

    jt.nativeSpecification = '-q %s' % self.sge_queue
    if self.require_license :
      jt.nativeSpecification += ' -v LM_LICENSE_FILE=1800@adsc-linux -l quartus_full=1'

    print "Submitted", self.getStepDesc()
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def submitResults(self, connection) :
    results = {"test_name" : self.test_name,  "parameter" : json.dumps(self.option) }
    # Read the fmax
    with open(os.path.join(self.altera_synthesis_base_dir, 'clk_fmax.rpt')) as fmax_rpt:
      fmax = float(fmax_rpt.read())
      results['fmax'] = fmax
      self.results['fmax'] = fmax

    with open(os.path.join(self.altera_synthesis_base_dir, 'resource.rpt')) as resource_rpt:
      # Read the results into the result dict.
      for k, v in json.load(resource_rpt).items() :
        v = re.match(r"(^\d*)",v.replace(',','')).group(1)

        if v == '' :
          v = '0'

        results[k] = int(v)
        self.results[k] = v

    print self.results

    connection.execute('''INSERT INTO synthesis(
          name,
          parameter,

          fmax,

          mem_bit,

          regs,

          alut,
          alm,

          les,
          les_wo_reg,
          les_w_reg_only,
          les_and_reg,
          les_normal,
          les_arit,

          lut2,
          lut3,
          lut4,
          lut6,

          mult9,
          mult18)

        VALUES (
          :test_name,
          :parameter,

          :fmax,

          :mem_bit,

          :regs,

          :alut,
          :alm,

          :les,
          :les_wo_reg,
          :les_w_reg_only,
          :les_and_reg,
          :les_normal,
          :les_arit,

          :lut2,
          :lut3,
          :lut4,
          :lut6,

          :mult9,
          :mult18)''',
    results)
