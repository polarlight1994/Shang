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

import altera

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
  AlteraNls = 'altera_nls'

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
    # The FPGA parameter.
    self.fpga_family = FamilyNames[config['device_family']]
    self.fpga_device = FamilyDevices[config['device_family']]
    self.require_license = (config['device_family'] == 'StratixIV')
    #Use full version for stratix devices.
    self.quartus_bin = '/nfs/app/altera/quartus12.1x64_%s/quartus/bin/' % ('full' if self.require_license else 'web')

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

  def getOptionCompack(self) :
    return [ (k, v) for k, v in self.option.iteritems() if set([v]) != set(self.option_space_dict[k])]

  def submitLogfiles(self, connection, status) :
    connection.execute('''
INSERT INTO
  logfile(name, parameter, stdout, stderr, test_file, synthesis_config_file, status)
  VALUES (:test_name, :parameter, :stdout, :stderr, :test_file, :synthesis_config_file, :status)
''',{
      'test_name' : self.test_name,
      'parameter' : json.dumps(self.getOptionCompack()),
      'stdout' :  self.stdout,
      'stderr' : self.stderr,
      'test_file' : self.test_file,
      'synthesis_config_file' : self.synthesis_config_file,
      'status' : status
    })

    return status == 'passed'

  def submitResults(self, connection, status) :
    self.submitLogfiles(connection, status)

  def getStepDesc(self) :
    return ' '.join([self.test_name, self.step_name, str(self.getOptionCompack())]).replace(':', '-')

  def getJobName(self) :
    return ' '.join([self.test_name, self.step_name])

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

  def getLicenseSpecification(self) :
    return ' -v LM_LICENSE_FILE=1800@adsc-linux -l quartus_full=1'

  def dumplog(self) :
    print "stdout of", self.test_name, "begin"
    with open(self.stdout, "r") as logfile:
      sys.stdout.write(logfile.read())
    print "stdout of", self.test_name, "end"

    #print "stderr of", self.test_name, "begin"
    #with open(self.stderr, "r") as logfile:
    #  sys.stdout.write(logfile.read())
    #print "stderr of", self.test_name, "end"

  def generateSubTests(self) :
    return []

# High-level synthesis step.
class HLSStep(TestStep) :
  step_name = 'high-level synthesis'

  def __init__(self, config):
    TestStep.__init__(self, config)

  def prepareTestDIR(self) :
    # Create the local folder for the current test.
    self.hls_base_dir = os.path.join(os.path.dirname(self.test_file),
                           self.test_name,
                           datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    os.makedirs(self.hls_base_dir)

  def submitResults(self, connection, status) :
    if (not self.submitLogfiles(connection, status)) : return

    # TODO: Extract other statistics
    connection.execute('''
INSERT INTO
  highlevelsynthesis(name, parameter, rtl_output)
  VALUES (:test_name, :parameter, :rtl_output)
''',{
      'test_name' : self.test_name,
      'parameter' : json.dumps(self.getOptionCompack()),
      'rtl_output' :  self.rtl_output
    })

  def createJobTemplate(self) :
    # Create the HLS job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getJobName()

    #Set up the correct working directory and the output path
    jt.workingDirectory = os.path.dirname(self.synthesis_config_file)

    self.stdout = os.path.join(self.hls_base_dir, 'hls.stdout')
    jt.outputPath = ':' + self.stdout

    self.stderr = os.path.join(self.hls_base_dir, 'hls.stderr')
    jt.errorPath = ':' + self.stderr
    jt.joinFiles=True

    jt.nativeSpecification = '-q %s' % self.sge_queue

    if self.require_license :
      jt.nativeSpecification += self.getLicenseSpecification()

    return jt

class LegUpHLSStep(HLSStep) :
  step_name = 'LegUp high-level synthesis'

  def __init__(self, config):
    HLSStep.__init__(self, config)
    self.legup_llc = '''/nfs/app/legup/3.0/legup/llvm/Release+Asserts/bin/llc'''
    self.legup_families_config = '''/nfs/app/legup/3.0/legup/hwtest/%(device_family)s.tcl''' % config
    self.legup_family = config['device_family']

  def prepareTest(self) :
    self.prepareTestDIR()
    self.rtl_output = os.path.join(self.hls_base_dir, self.test_name + ".sv")

    with open(os.path.join(self.hls_base_dir, self.test_name + ".sql"), 'w') as dummy_sql:
      dummy_sql.write('''
CREATE TABLE mcps(
       id INTEGER PRIMARY KEY AUTOINCREMENT,
       src TEXT,
       dst TEXT,
       thu TEXT,
       cycles INTEGER,
       normalized_delay REAL
       );
''')

    #Generate the HLS config.
    self.period = int(1000.0/self.fmax)
    self.synthesis_config_file = os.path.join(self.hls_base_dir, 'test_config.tcl')
    with open(self.synthesis_config_file, 'w') as tcl_config:
      tcl_config.write('''
# legup.tcl - LegUp configuration file for test suite
#
# These variables can be overridden by environment variables using the naming
# convention:
#
#       LEGUP_($VARIABLE_NAME)
#
# ie. to turn off divider sharing:
#
#       export LEGUP_SHARE_DIV=0
#
# See Makefile.config for more examples
#

if { [get_device_family] != "CycloneII" &&
     [get_device_family] != "CycloneIV" &&
     [get_device_family] != "StratixIV" } {
    puts stderr "Unrecognized Family. Make to you include the ../hwtest/(family).tcl\n";
}

# if set, div/rem will be shared with any required mux width (as in Legup 1.0)
set_parameter SHARE_DIV 1 
set_parameter SHARE_REM 1

# only turn on multiplier sharing with DSPs off
#set_parameter SHARE_MUL 1

# set to ensure that muxing will be placed on multipliers if max DSPs
# are exceeded (as opposed to performing multiplication with logic)
set_parameter RESTRICT_TO_MAXDSP 0

# Enable multi-pumping of multipliers that use DSPs
#set_parameter MULTIPUMPING 0

# Only schedule one multiplier per cycle
# now deprecated in favour of SDC_RES_CONSTRAINTS
set_parameter MINIMIZE_MULTIPLIERS 0

# Override DSP infer estimation algorithm - assume multiply-by-constant
# always infers DSPs
set_parameter MULT_BY_CONST_INFER_DSP 0

# use local block rams for every array and remove global memory controller
# WARNING: only works with simple pointers and only one function (main)
#set_parameter LOCAL_RAMS 1

# when LOCAL_RAMS is on try to schedule independent load/stores in parallel
#set_parameter PARALLEL_LOCAL_RAMS 1

# Explicitly instantiate all multipliers as lpm_mult modules
# Setting MULTIPLIER_PIPELINE_STAGES > 0 will also turn this on
#set_parameter EXPLICIT_LPM_MULTS 1

# Number of pipeline stages for a multiplier
set_parameter MULTIPLIER_PIPELINE_STAGES 0

# Don't chain multipliers
#set_parameter MULTIPLIER_NO_CHAIN 1

# Maximum chain size to consider. Setting to 0 uses Legup 1.0 original binding
# SET TO 0 TO DISABLE PATTERN SHARING
# (setting to 0 shares only dividers and remainders, as in LegUp 1.0)
set_parameter MAX_SIZE 10

# The settings below should all be nonzero, but can be disabled when debugging
# if set, these will be included in patterns and shared with 2-to-1 muxing
if { [get_device_family] == "StratixIV" } {
    # these aren't worth sharing on CycloneII
    set_parameter SHARE_ADD 1
    set_parameter SHARE_SUB 1
} 

set_parameter SHARE_BITOPS 1
set_parameter SHARE_SHIFT 1

# Two operations will only be shared if the difference of their true bit widths
# is below this threshold: e.g. an 8-bit adder will not be shared with 
# a 32-bit adder unless BIT_DIFF_THRESHOLD >= 24
set_parameter BIT_DIFF_THRESHOLD 10
set_parameter BIT_DIFF_THRESHOLD_PREDS 30

# The minimum bit width of an instruction to consider
# (e.g. don't bother sharing 1 bit adders)
set_parameter MIN_WIDTH 2

# write patterns to dot file
#set_parameter WRITE_TO_DOT 1

# write patterns to verilog file
#set_parameter WRITE_TO_VERILOG 1

# MinimizeBitwidth parameters
#set to 1 to print bitwidth minimization stats
#set_parameter MB_PRINT_STATS 1
#set to filename from which to read initial data ranges.  If it's
#undefined, then no initial ranges are assumed
#set_parameter MB_RANGE_FILE "range.profile"
#max number of backward passes to execute (-1 for infinite)
set_parameter MB_MAX_BACK_PASSES -1
set_parameter MB_MINIMIZE_HW 0


# Minimum pattern frequency written to dot/v file
#set_parameter FREQ_THRESHOLD 1

# disable register sharing based on live variable analysis
#set_parameter DISABLE_REG_SHARING 1

#
# Scheduling Variables
#

# Setting this environment variable to a particular integer value in ns will
# set the clock period constraint.
# WARNING: This gets overriden by the environment variable LEGUP_SDC_PERIOD in
# Makefile.config based on the target family. 
set_parameter SDC_PERIOD %(period)s

# Disable chaining of operations in a clock cycle. This will achieve the
# maximum amount of pipelining. 
# Note: this overrides SDC_PERIOD 
#set_parameter SDC_NO_CHAINING 1

# Perform as-late-as-possible (ALAP) scheduling instead of as-soon-as-possible
# (ASAP).
#set_parameter SDC_ALAP 1

# Cause debugging information to be printed from the scheduler.
#set_parameter SDC_DEBUG 1

# Disable SDC scheduling and use the original scheduling that was in the LegUp
# 1.0 release.
#set_parameter NO_SDC 1

# Push more multipliers into the same state for multi-pumping
#set_parameter SDC_MULTIPUMP 1

#
# Debugging
#

# prepend every printf with the number of cycle elapsed
#set_parameter PRINTF_CYCLES 1

# print all signals to the verilog file even if they don't drive outputs
#set_parameter KEEP_SIGNALS_WITH_NO_FANOUT 1

# display cur_state on each cycle for each function
#set_parameter PRINT_STATES 1

# turn off getelementptr instructions chaining
#set_parameter DONT_CHAIN_GET_ELEM_PTR 0

# SDC resource constraints
set_parameter SDC_RES_CONSTRAINTS 1

# number of multipliers - SDC resource constraints must be on
#set_parameter NUM_MULTIPLIERS 2

# number of memory ports - SDC resource constraints must be on
# to enable dual port    - DUAL_PORT_BINDING must be on 
#                        - ALIAS_ANALYSIS must be on
set_parameter NUM_MEM_PORTS 2

# number of divider/remainder functional units in the hardware
set_parameter NUM_DIVIDERS 1

# enable dual port binding
set_parameter DUAL_PORT_BINDING 1

# create load/store dependencies based on LLVM alias analysis
set_parameter ALIAS_ANALYSIS 1

# turn off generating data flow graph dot files for every basic block
#set_parameter NO_DFG_DOT_FILES 1

# turn off minimize bitwidth path
#set_parameter NO_MIN_BITWIDTH 1

# pipeline a loop
# use the optional ii parameter to force a specific pipeline initiation
# interval
#loop_pipeline "loop1"
#loop_pipeline "loop2" -ii 1

# turn off all loop pipelining
#set_parameter NO_LOOP_PIPELINING 1
''' % self)

  # Run the test
  def runTest(self) :
    jt = self.createJobTemplate()
    jt.remoteCommand = 'timeout'
    # Use a bigger timeout if we are runing the feedback flow.
    timeout = self.hls_feedback_flow_timeout * self.shang_max_scheduling_iteration if self.timing_model == 'external' else self.hls_timeout

    # Setup the device family and the period, need to reset the nativeSpecification
    jt.nativeSpecification = '-q %s' % self.sge_queue
    jt.nativeSpecification += ' -v SDC_PERIOD=%(period)s,FAMILY=%(legup_family)s,DEVICE_FAMILY=%(fpga_family)s,DEVICE=%(fpga_device)s'
    if self.require_license :
      jt.nativeSpecification += ',LM_LICENSE_FILE=1800@adsc-linux -l quartus_full=1 '

    #$(LLVM_HOME)llc $(LLC_FLAGS) -march=v $(NAME).bc -o $(VFILE)
    jt.args = ['%ds' % timeout, self.legup_llc,
               '-legup-config=%s' % self.legup_families_config,
               '-legup-config=%s' % self.synthesis_config_file,
               '-march=v',
               self.test_file,
               '-o',
               self.rtl_output
              ]

    print "Submitted", self.getStepDesc()
    #Submit the job.
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def generateSubTests(self) :
    return [ LegUpHWSimStep(self) ]

# High-level synthesis step.
class ShangHLSStep(HLSStep) :
  step_name = 'shang high-level synthesis'

  def __init__(self, config):
    HLSStep.__init__(self, config)

  def prepareTest(self) :
    self.prepareTestDIR()
    self.rtl_output = os.path.join(self.hls_base_dir, self.test_name + ".sv")

    #Generate the HLS config.
    self.synthesis_config_file = os.path.join(self.hls_base_dir, 'test_config.lua')
    self.generateFileFromTemplate('''-- Initialize the global variables.
ptr_size = {{ ptr_size }}
InputFile = [[{{ test_file }}]]
RTLOutput = [[{{ rtl_output }}]]
MCPDataBase =  [[{{ [hls_base_dir, test_name + ".sql"]|joinpath }}]]
MainSDCOutput =  [[{{ [hls_base_dir, test_name + ".sdc"]|joinpath }}]]
--MainDelayVerifyOutput = [[{{ MAIN_DELAY_VERIFY_SRC }}]]
SoftwareIROutput =  [[{{ [hls_base_dir, test_name + "_soft.ll"]|joinpath }}]]
IFFileName =  [[{{ [hls_base_dir, test_name + "_if.cpp"]|joinpath }}]]
RTLModuleName = [[{{ [test_name, "_RTL_DUT"]|join }}]]

Functions.{{ hardware_function }} = RTLModuleName

TimingAnalysis.ExternalTool = [[{{ [quartus_bin, 'quartus_sh']|joinpath }}]]
TimingAnalysis.Device = [[{{ fpga_device }}]]

local FMAX = {{ fmax }}
PERIOD = 1000.0 / FMAX
FUs.Period = PERIOD

dofile([[{{ [config_dir, 'common_config.lua']|joinpath }}]])

-- Define some function
dofile([[{{ [config_dir, 'FuncDefine.lua']|joinpath }}]])
-- load platform information script
dofile([[{{ [config_dir, 'AlteraCommon.lua']|joinpath }}]])
dofile([[{{ [config_dir, fpga_device + ".lua"]|joinpath }}]])

{% if hardware_function == 'main' %}
Misc.RTLGlobalScript = [=[
RTLGlobalCode = FUs.CommonTemplate
]=]
Misc.RTLTopModuleScript = [=[ ]=]
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

local IfFile = assert(io.open (IFFileName, "w"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SCIFGScript, output=IfFile}
if message ~= nil then print(message) end
IfFile:close()
]=]

Misc.RTLTopModuleScript = [=[
local IfFile = assert(io.open (IFFileName, "a+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SCIFFScript, output=IfFile}
if message ~= nil then print(message) end
IfFile:close()
]=]
{% endif %}
''', self.synthesis_config_file)

  # Run the test
  def runTest(self) :
    jt = self.createJobTemplate()
    jt.remoteCommand = 'timeout'
    # Use a bigger timeout if we are runing the feedback flow.
    timeout = self.hls_feedback_flow_timeout * ( 3 * self.shang_max_scheduling_iteration ) if self.timing_model == 'external' else self.hls_timeout
    jt.args = ['%ds' % timeout, self.shang, self.synthesis_config_file, '-stats',
               '-timing-model=%(timing_model)s' % self,
               '-shang-enable-mux-pipelining=%(shang_enable_mux_pipelining)s' % self,
               '-shang-enable-memory-optimization=%(shang_enable_memory_optimization)s' % self,
               '-shang-enable-memory-partition=%(shang_enable_memory_partition)s' % self,
               '-shang-enable-dual-port-ram=%(shang_enable_dual_port_ram)s' % self,
               '-shang-enable-pre-schedule-lut-mapping=%(shang_enable_pre_schedule_lut_mapping)s' % self,
               '-shang-enable-register-sharing=%(shang_enable_register_sharing)s' % self,
               '-shang-max-scheduling-iteration=%(shang_max_scheduling_iteration)s' % self,
               '-shang-dump-intermediate-netlist=%(shang_dump_intermediate_netlist)s' % self,
               '-shang-selector-ignore-trivial-loops=true',
               '-shang-selector-ignore-x-fanins=true'
              ]

    print "Submitted", self.getStepDesc()
    #Submit the job.
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def generateSubTests(self) :
    #If test type == hybrid simulation
    if self.mode == TestStep.HybridSim :
      return self.generateHybridSim()
    elif self.mode == TestStep.PureHWSim or self.mode == TestStep.AlteraSyn \
         or self.mode == TestStep.AlteraNls :
      return self.generatePureHWSim()

    return []

  def generateHybridSim(self) :
    return [ HybridSimStep(self) ]

  def generatePureHWSim(self) :
    num_iter = self.shang_max_scheduling_iteration
    if self.shang_dump_intermediate_netlist == 'true' and num_iter > 1 :
      for i in range(num_iter - 1) :
        sim_step = ShangHWSimStep(self)
        sim_step.option = self.option.copy()
        sim_step.hls_base_dir = os.path.join(sim_step.hls_base_dir, str(i))
        sim_step.rtl_output = os.path.join(sim_step.hls_base_dir, sim_step.test_name + ".sv")
        sim_step.option['shang_max_scheduling_iteration'] = i + 1
        yield sim_step
    
    yield ShangHWSimStep(self)

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
{{ verilator }} {{ rtl_output }} \
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
timeout 60s {{ [hls_base_dir, test_name + "_systemc_testbench"]|joinpath }} > hardware.out \
|| exit 1

# Compare the hardware output with the standar output
{{ lli }} {{ test_file }} > expected.output || exit 1

[ -f hardware.output ]  && exit 1
diff expected.output hardware.out || exit 1
    ''', self.hybrid_sim_script)

  def runTest(self) :
    # Create the hybrid simulation job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getJobName()
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

class HWSimStep(TestStep) :
  def __init__(self, hls_step):
    TestStep.__init__(self, hls_step.__dict__)
    self.results.update(hls_step.results)

  def prepareTestDIR(self) :
    self.pure_hw_sim_base_dir = os.path.join(self.hls_base_dir, 'pure_hw_sim')
    os.makedirs(self.pure_hw_sim_base_dir)

  def runTest(self) :
    # Create the simulation job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getJobName()
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

  def generateSubTests(self) :
    #If test type == hybrid simulation
    if self.mode == TestStep.AlteraSyn or self.mode == TestStep.AlteraNls:
      return [ AlteraSynStep(self) ]

    return []

class ShangHWSimStep(HWSimStep) :
  step_name = 'shang hardware simulation'

  def __init__(self, hls_step):
    HWSimStep.__init__(self, hls_step)

  def prepareTest(self) :
    self.prepareTestDIR()

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

// The module successfully complete its execution if return_value is 0.
assign succ = ~(|return_value);

  main main_inst(
  .clk(clk),
  .rstN(rstN),
  .start(start),
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
    rstN = 0;
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

  integer cntfile;

  always_comb begin
    if (fin) begin
      if (!succ) begin
        $display ("The result is incorrect!");
        $finish(1);
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

endmodule
''', os.path.join(self.pure_hw_sim_base_dir, 'DUT_TOP_tb.sv'))

    #Generate the simulation script.
    self.pure_hw_sim_script = os.path.join(self.pure_hw_sim_base_dir, 'pure_hw_sim.sge')
    self.generateFileFromTemplate('''#!/bin/bash
#$ -S /bin/bash

export PATH=/nfs/app/altera/modelsim_ase_12_x64/modelsim_ase/bin/:$PATH

vlib work || exit 1
vlog -sv {{ rtl_output }} || exit 1
vlog -sv DUT_TOP_tb.sv || exit 1
vsim -t 1ps work.DUT_TOP_tb -c -do "run -all;quit -f" || exit 1

# cycles.rpt will only generate when the simulation produce a correct result.
[ -f cycles.rpt ] || exit 1
''', self.pure_hw_sim_script)

  def submitResults(self, connection, status) :
    if (not self.submitLogfiles(connection, status)) : return

    with open(os.path.join(self.pure_hw_sim_base_dir, 'cycles.rpt')) as cycles_rpt:
      num_cycles = int(cycles_rpt.read())
      self.results["cycles"] = num_cycles
      connection.execute("INSERT INTO simulation(name, parameter, cycles) VALUES (:test_name, :parameter, :cycles)",
                         {"test_name" : self.test_name,  "parameter" : json.dumps(self.getOptionCompack()), "cycles": num_cycles})
    cycles_rpt.close()

class LegUpHWSimStep(HWSimStep) :
  step_name = 'LegUp hardware simulation'

  def __init__(self, hls_step):
    HWSimStep.__init__(self, hls_step)

  def prepareTest(self) :
    self.pure_hw_sim_base_dir = self.hls_base_dir

    #Generate the simulation script.
    self.pure_hw_sim_script = os.path.join(self.pure_hw_sim_base_dir, 'pure_hw_sim.sge')
    self.generateFileFromTemplate('''#!/bin/bash
#$ -S /bin/bash

export PATH=/nfs/app/altera/modelsim_ase_12_x64/modelsim_ase/bin/:$PATH

vlib work || exit 1
vlog -sv {{ rtl_output }} || exit 1
vsim -L altera_mf_ver -L lpm_ver -t 1ps work.main_tb -c -do "run 7000000000000000ns; exit;" || exit 1

''', self.pure_hw_sim_script)

  def submitResults(self, connection, status) :
    if (not self.submitLogfiles(connection, status)) : return

    with open(os.path.join(self.pure_hw_sim_base_dir, 'cycles.rpt')) as cycles_rpt:
      num_cycles = int(cycles_rpt.read())
      self.results["cycles"] = num_cycles
      connection.execute("INSERT INTO simulation(name, parameter, cycles) VALUES (:test_name, :parameter, :cycles)",
                         {"test_name" : self.test_name,  "parameter" : json.dumps(self.getOptionCompack()), "cycles": num_cycles})
    cycles_rpt.close()

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

  def prepareTest(self) :
    self.altera_synthesis_base_dir = os.path.join(self.hls_base_dir, 'altera_synthesis')
    os.makedirs(self.altera_synthesis_base_dir)

    #Generate the scripts
    altera.generate_scripts(sql_path = os.path.join(self.hls_base_dir, self.test_name + ".sql"),
                            sdc_path = os.path.join(self.hls_base_dir, self.test_name + ".sdc"),
                            report_path = os.path.join(self.hls_base_dir, self.test_name + "_report_timing.tcl"),
                            period = 1000.0 / self.fmax,
                            factor = self.shang_constraints_factor)

    self.altera_synthesis_script = os.path.join(self.altera_synthesis_base_dir, 'setup_prj.tcl')
    self.generateFileFromTemplate('''# Load necessary package.
load_package flow
load_package report

project_new {{ test_name }} -overwrite

set_global_assignment -name FAMILY "{{ fpga_family }}"
set_global_assignment -name DEVICE {{ fpga_device }}

set_global_assignment -name TOP_LEVEL_ENTITY main
set_global_assignment -name SOURCE_FILE {{ rtl_output }}
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
execute_module -tool sta -args {--report_script {{ [hls_base_dir, test_name + "_report_timing.tcl"]|joinpath }} }

source {{ [config_dir, 'report_json_data.tcl']|joinpath }}

#Write the netlist
execute_module -tool eda

project_close
''', self.altera_synthesis_script)

  def runTest(self) :
    # Create the simulation job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getJobName()
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
      jt.nativeSpecification += self.getLicenseSpecification()

    print "Submitted", self.getStepDesc()
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def submitResults(self, connection, status) :
    if (not self.submitLogfiles(connection, status)) : return

    results = {"test_name" : self.test_name,  "parameter" : json.dumps(self.getOptionCompack()) }
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

  def generateSubTests(self) :
    if self.mode == TestStep.AlteraNls :
      return [ AlteraNetlistSimStep(self) ]

    return []

class AlteraNetlistSimStep(TestStep) :
  step_name = 'altera netlist simulation'

  def __init__(self, syn_step):
    TestStep.__init__(self, syn_step.__dict__)
    self.results.update(syn_step.results)

  def prepareTest(self) :
    self.netlist_sim_base_dir = os.path.join(self.altera_synthesis_base_dir, 'simulation', 'modelsim')
    #os.makedirs(self.netlist_sim_base_dir)

    self.reported_period = 1000.0 / self.results['fmax'] + 0.01

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

// The module successfully complete its execution if return_value is 0.
assign succ = ~(|return_value);

  main main_inst(
  .clk(clk),
  .rstN(rstN),
  .start(start),
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
  reg clk = 1'b0;
  reg rstN = 1'b0;
  wire [7:0] LED7;
  wire succ;
  wire fin;
  reg startcnt = 1'b0;

  DUT_TOP i1 (
    .clk(clk),
    .rstN(rstN),
    .start(1'b1),
    .LED7(LED7),
    .succ(succ),
    .fin(fin)
  );

  // integer wfile,wtmpfile;
  initial begin
    // Allocate some times to allow the circuit to become stable!
    for (int i = 0; i < 1000; ++i)
      @(posedge clk);
    rstN <= 1;
    @(posedge clk);
    $display ("Start at %t!", $time());
    startcnt <= 1;
  end

  // Generate the 100MHz clock.
  always #{{ "%.2f" % (reported_period / 2) }}ns clk = ~clk;

  reg [31:0] cnt = 0;

  integer cntfile;

  always_comb begin
    if (fin) begin
      if (!succ) begin
        $display ("The result is incorrect!");
        $finish(1);
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
    if (startcnt && cnt % 80 == 0) $write(".");
    // Do not exceed 80 columns.
    if (startcnt && cnt % 6400 == 0) $write("%t\n", $time());
  end

endmodule
''', os.path.join(self.netlist_sim_base_dir, 'DUT_TOP_tb.sv'))

    #Generate the simulation script.
    self.netlist_sim_expire = 1.2 * self.results["cycles"] + 1500
    self.netlist_sim_script = os.path.join(self.netlist_sim_base_dir, 'netlist_sim.sge')
    self.generateFileFromTemplate('''#!/bin/bash
#$ -S /bin/bash

export PATH=/nfs/app/altera/modelsim_ase_12_x64/modelsim_ase/bin/:$PATH

vlib work || exit 1
vlog -vlog01compat {{ test_name }}.vo || exit 1
vlog -sv DUT_TOP_tb.sv || exit 1
vsim -t 1ps -L altera_ver -L {{ device_family.lower() }}_ver work.DUT_TOP_tb -c -do "do {{ test_name }}_dump_all_vcd_nodes.tcl;run {{ "%.2f" % (netlist_sim_expire * reported_period) }}ns;vcd flush;quit -f"

# cycles.rpt will only generate when the simulation produce a correct result.
[ -f cycles.rpt ] || exit 1
''', self.netlist_sim_script)

  def runTest(self) :
    # Create the simulation job.
    jt = Session.createJobTemplate()

    jt.jobName = self.getJobName()
    jt.remoteCommand = 'bash'
    jt.args = [ self.netlist_sim_script ]
    #Set up the correct working directory and the output path
    jt.workingDirectory = self.netlist_sim_base_dir
    self.stdout = os.path.join(self.netlist_sim_base_dir, 'netlist_sim.output')
    jt.outputPath = ':' + self.stdout
    self.stderr = os.path.join(self.netlist_sim_base_dir, 'netlist_sim.stderr')
    jt.errorPath = ':' + self.stderr
    jt.joinFiles=True

    jt.nativeSpecification = '-q %s' % self.sge_queue

    print "Submitted", self.getStepDesc()
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def submitResults(self, connection, status) :
    if (not self.submitLogfiles(connection, status)) : return
