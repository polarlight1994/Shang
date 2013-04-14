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
import os, sys

# Base class of test step.
class TestStep :
  config_template_env = Environment()
  jobid = 0
  stdout = ''
  stderr = ''

  HybridSim = 'hybrid_sim'
  PureHWSim = 'pure_hw_sim'

  def __init__(self, config):
    self.__dict__ = config.copy()
    self.config_template_env.filters['joinpath'] = lambda list: os.path.join(*list)

  def __getitem__(self, key):
    return self.__dict__[key]

  #def __setitem__(self, key, value):
  #  self.__dict__[key] = value

  # Create the test environment

  # Run the step

  # Parse the test result

  # Optionally, lauch the subtests

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

  def __init__(self, config):
    TestStep.__init__(self, config)

  def prepareTest(self) :
    # Create the local folder for the current test.
    from datetime import datetime
    self.hls_base_dir = os.path.join(os.path.dirname(self.test_file),
                           self.test_name,
                           datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    os.makedirs(self.hls_base_dir)

    # Create the template
    template = self.config_template_env.from_string('''
-- Initialize the global variables.
ptr_size = {{ ptr_size }}
test_binary_root = [[{{ hls_base_dir }}]]
InputFile = [[{{ test_file }}]]
RTLOutput = [[{{ [hls_base_dir, test_name + ".sv"]|joinpath }}]]
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

-- Load ip module and simulation interface script.
dofile([[{{ [config_dir, 'InterfaceGen.lua']|joinpath }}]])
dofile([[{{ [config_dir, 'ModelSimGen.lua']|joinpath }}]])

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
''')

    #Generate the HLS config.
    self.synthesis_config_file = os.path.join(self.hls_base_dir, 'test_config.lua')
    template.stream(self.__dict__).dump(self.synthesis_config_file)

  # Run the test
  def runTest(self, session) :
    # Create the HLS job.
    jt = session.createJobTemplate()
    jt.remoteCommand = 'timeout'
    jt.args = ['60s', self.shang, self.synthesis_config_file]
    #Set up the correct working directory and the output path
    jt.workingDirectory = os.path.dirname(self.synthesis_config_file)

    self.stdout = os.path.join(self.hls_base_dir, 'hls.stdout')
    jt.outputPath = ':' + self.stdout

    self.stderr = os.path.join(self.hls_base_dir, 'hls.stderr')
    jt.errorPath = ':' + self.stderr

    print 'Submitted HLS', self.test_name
    #Submit the job.
    self.jobid = session.runJob(jt)
    session.deleteJobTemplate(jt)

  def generateSubTests(self) :
    #If test type == hybrid simulation
    if self.mode == TestStep.HybridSim :
      return [ HybridSimStep(self) ]

    return []

# The test step for hybrid simulation.
class HybridSimStep(TestStep) :

  def __init__(self, hls_step):
    TestStep.__init__(self, hls_step.__dict__)


  def prepareTest(self) :
    self.hybrid_sim_base_dir = os.path.join(self.hls_base_dir, 'hybrid_sim')
    os.makedirs(self.hybrid_sim_base_dir)

    # Create the template
    template = self.config_template_env.from_string('''#!/bin/bash
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
    ''')

    #Generate the hybrid simulation script.
    self.hybrid_sim_script = os.path.join(self.hls_base_dir, 'test_config.lua')
    template.stream(self.__dict__).dump(self.hybrid_sim_script)

  def runTest(self, session) :
    # Create the hybrid simulation job.
    jt = session.createJobTemplate()

    jt.remoteCommand = 'bash'
    jt.args = [ self.hybrid_sim_script ]
    #Set up the correct working directory and the output path
    jt.workingDirectory = self.hybrid_sim_base_dir
    self.stdout = os.path.join(self.hybrid_sim_base_dir, 'hybrid_sim.output')
    jt.outputPath = ':' + self.stdout
    self.stderr = os.path.join(self.hybrid_sim_base_dir, 'hybrid_sim.stderr')
    jt.errorPath = ':' + self.stderr


    print 'Submitted hybrid simulation', self.test_name
    self.jobid = session.runJob(jt)
    session.deleteJobTemplate(jt)