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
import os

# Base class of test step.
class TestStep :
  config_template_env = Environment()

  def __init__(self, config, config_template_env):
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

# High-level synthesis step.
class HLSStep(TestStep) :

  def __init__(self, config, config_template_env):
    TestStep.__init__(self, config, config_template_env)

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

    #print template.render(self.__dict__)
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

    self.hls_stdout = os.path.join(self.hls_base_dir, 'hls.stdout')
    jt.outputPath = ':' + self.hls_stdout

    self.hls_stderr = os.path.join(self.hls_base_dir, 'hls.stderr')
    jt.errorPath = ':' + self.hls_stderr

    print 'Submitted', self.test_name
    #Submit the job.
    self.jobid = session.runJob(jt)
    session.deleteJobTemplate(jt)