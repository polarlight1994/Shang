#!/usr/bin/env python

"""
Micro benchmarking script for functional units characterization
"""

import math, os, platform, random, re, sys, time, threading, itertools, json, argparse
import shutil
import drmaa
import sqlite3

Session = drmaa.Session()
Session.initialize()

class MicroBenchmark:
  def __init__(self, **kwargs):
    self.name = kwargs['name']
    self.bitwidth = kwargs['bitwidth']
    self.design_hdl = kwargs['design_hdl'] % kwargs
    self.fpga_device = kwargs['fpga_device']

  def generate_files(self, pwd) :
    #Create the directory
    self.working_dir = os.path.join(pwd, self.name + str(self.bitwidth))
    if os.path.exists(self.working_dir): shutil.rmtree(self.working_dir)
    os.makedirs(self.working_dir)
    #Generate the design file
    self.hdl_path = os.path.join(self.working_dir, self.name + '.v')
    with open(self.hdl_path, 'w') as hdl_file:
      hdl_file.write(self.design_hdl)
    #Generate the quartus project scripts
    self.sdc_path = os.path.join(self.working_dir, self.name + ".sdc")

    # 1. The script to extract the delay
    self.delay_json_path = os.path.join(self.working_dir, self.name + ".json")
    self.timing_extraction_path = os.path.join(self.working_dir, self.name + "_extract_timing.tcl")
    with open(self.timing_extraction_path, 'w') as timing_extraction_file:
      timing_extraction_file.write('''#Extract the delay
set JSONFile [open "%s" w]
set results [ report_path -nworst 1 -from [get_cells -compatibility_mode {dut*}] -to [get_cells -compatibility_mode {dut*}] ]
set delay [lindex $results 1]
puts $JSONFile "{ \\"delay\\":\\"$delay\\" }"
''' % self.delay_json_path)

    # 2. The project script
    self.synthesis_script_path = os.path.join(self.working_dir, self.name + "_synhtesis.tcl")
    with open(self.synthesis_script_path, 'w') as synthesis_script_file:
      synthesis_script_file.write('''# Load necessary package.
load_package flow
load_package report
load_package incremental_compilation

project_new %(name)s -overwrite

set_global_assignment -name DEVICE %(fpga_device)s

set_global_assignment -name TOP_LEVEL_ENTITY top
set_global_assignment -name SOURCE_FILE %(hdl_path)s

create_base_clock -fmax "1000 MHz" -target clk clk

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

set_global_assignment -name LL_ROOT_REGION ON -section_id "Root Region"
set_global_assignment -name LL_MEMBER_STATE LOCKED -section_id "Root Region"
create_partition -partition "%(name)s:dut" -contents %(name)s:dut

execute_module -tool map
execute_module -tool cdb -args {--merge=on}
execute_module -tool fit
execute_module -tool sta -args {--report_script "%(timing_extraction_path)s" }

#Write the netlist
#execute_module -tool eda

project_close
''' % self.__dict__)

  def submit_job(self):
    # Create the HLS job.
    jt = Session.createJobTemplate()

    jt.jobName = self.name
    jt.remoteCommand = '/nfs/app/altera/quartus12.1x64_full/quartus/bin/quartus_sh'
    jt.args = [ '--64bit', '-t',  self.synthesis_script_path ]

    #Set up the correct working directory and the output path
    jt.workingDirectory = self.working_dir

    self.stdout = os.path.join(self.working_dir, 'stdout')
    jt.outputPath = ':' + self.stdout

    jt.joinFiles=True

    jt.nativeSpecification = '-q fast.q -v LM_LICENSE_FILE=1800@adsc-linux -l quartus_full=1'

    print "Submitted", self.name, self.bitwidth
    #Submit the job.
    self.jobid = Session.runJob(jt)
    Session.deleteJobTemplate(jt)

  def processResults(self):
    with open(self.stdout, 'r') as logfile:
      sys.stdout.write(logfile.read())

    print self.name, self.bitwidth
    with open(self.delay_json_path, 'r') as delay_json:
      return json.load(delay_json)['delay']

    return 0.0

FUs = { 'Add' : '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  input wire d,
  output wire[%(bitwidth)s:0] c
);

assign c = a + b + d;

endmodule

module top(
  input clk,
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  input wire d,
  output reg[%(bitwidth)s:0] c
);

  reg [%(bitwidth)s-1:0] a_reg0;
  reg [%(bitwidth)s-1:0] a_reg1;
  reg [%(bitwidth)s-1:0] b_reg0;
  reg [%(bitwidth)s-1:0] b_reg1;
  reg d_reg0;
  reg d_reg1;
  reg [%(bitwidth)s:0] c_reg0;
  wire [%(bitwidth)s:0] c_wire;

always@(posedge clk) begin
  a_reg0 <= a;
  a_reg1 <= a_reg0;
  b_reg0 <= b;
  b_reg1 <= b_reg0;
  d_reg0 <= d;
  d_reg1 <= d_reg0;
  c_reg0 <= c_wire;
  c <= c_reg0;
end

%(name)s dut(
  .a(a_reg1),
  .b(b_reg1),
  .d(d_reg1),
  .c(c_wire)
);

endmodule
''',
'Mult' : '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  output wire[%(bitwidth)s - 1:0] c
);

assign c = a * b;

endmodule

module top(
  input clk,
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  output reg[%(bitwidth)s-1:0] c
);

  reg [%(bitwidth)s-1:0] a_reg0;
  reg [%(bitwidth)s-1:0] a_reg1;
  reg [%(bitwidth)s-1:0] b_reg0;
  reg [%(bitwidth)s-1:0] b_reg1;
  reg [%(bitwidth)s-1:0] c_reg0;
  wire [%(bitwidth)s-1:0] c_wire;

always@(posedge clk) begin
  a_reg0 <= a;
  a_reg1 <= a_reg0;
  b_reg0 <= b;
  b_reg1 <= b_reg0;
  c_reg0 <= c_wire;
  c <= c_reg0;
end

%(name)s dut(
  .a(a_reg1),
  .b(b_reg1),
  .c(c_wire)
);

endmodule
''',
'Shift' : '''

module %(name)s(
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  output wire[%(bitwidth)s-1:0] c
);

assign c = a >> b;

endmodule

module top(
  input clk,
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  output reg[%(bitwidth)s-1:0] c
);

  reg [%(bitwidth)s-1:0] a_reg0;
  reg [%(bitwidth)s-1:0] a_reg1;
  reg [%(bitwidth)s-1:0] b_reg0;
  reg [%(bitwidth)s-1:0] b_reg1;
  reg [%(bitwidth)s-1:0] c_reg0;
  wire [%(bitwidth)s-1:0] c_wire;

always@(posedge clk) begin
  a_reg0 <= a;
  a_reg1 <= a_reg0;
  b_reg0 <= b;
  b_reg1 <= b_reg0;
  c_reg0 <= c_wire;
  c <= c_reg0;
end

%(name)s dut(
  .a(a_reg1),
  .b(b_reg1),
  .c(c_wire)
);

endmodule
''',
'ICmp' : '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  output wire c
);

assign c = (a > b)? 1:0;

endmodule

module top(
  input clk,
  input wire[%(bitwidth)s-1:0] a,
  input wire[%(bitwidth)s-1:0] b,
  output reg c
);

  reg [%(bitwidth)s-1:0] a_reg0;
  reg [%(bitwidth)s-1:0] a_reg1;
  reg [%(bitwidth)s-1:0] b_reg0;
  reg [%(bitwidth)s-1:0] b_reg1;
  reg  c_reg0;
  wire  c_wire;
always@(posedge clk) begin
  a_reg0 <= a;
  a_reg1 <= a_reg0;
  b_reg0 <= b;
  b_reg1 <= b_reg0;
  c_reg0 <= c_wire;
  c <= c_reg0;
end

%(name)s dut(
  .a(a_reg1),
  .b(b_reg1),
  .c(c_wire)
);

endmodule
'''
}

MuxHDL = '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] inputs,
  input wire[%(bitwidth)s-1:0] ens,
  output wire sel_output
);
  integer l;
  reg sel_output_internal;
  always @(*) begin
    sel_output_internal = 1'b0;
    for(l = 0; l < %(bitwidth)s; l = l + 1) 
      sel_output_internal = sel_output_internal | (inputs[l] & ens[l]);
  end

  assign sel_output = sel_output_internal;
endmodule


module top(
  input clk,
  input wire ens,
  input wire inputs,
  output reg sel_output
);

  reg [%(bitwidth)s-1:0] ens_reg0;
  reg [%(bitwidth)s-1:0] ens_reg1;

  reg [%(bitwidth)s-1:0] inputs_reg0;
  reg [%(bitwidth)s-1:0] inputs_reg1;

  reg  sel_output_reg0;
  wire  sel_output_wire;

always@(posedge clk) begin
  ens_reg0[0] <= ens;
  ens_reg0[%(bitwidth)s-1:1] <= ens_reg0[%(bitwidth)s-1-1:0];
  ens_reg1 <= ens_reg0;

  inputs_reg0[0] <= inputs;
  inputs_reg0[%(bitwidth)s-1:1] <= inputs_reg0[%(bitwidth)s-1-1:0];
  inputs_reg1 <= inputs_reg0;

  sel_output_reg0 <= sel_output_wire;
  sel_output <= sel_output_reg0;
end

%(name)s dut(
  .inputs(inputs_reg1),
  .ens(ens_reg1),
  .sel_output(sel_output_wire)
);

endmodule

'''

Bitwidths = [ 1, 8, 16, 32, 64 ]

Devices = [ "EP2C70F896C6", "EP4CE75F29C6", "EP4SGX530KH40C2" ]

# Initialize the database connection
con = sqlite3.connect(":memory:")

# Create the tables for the experimental results.
# We create 3 tables: HLS results, simulation results, and synthesis results
con.executescript('''
  create table delays(
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT,
      fpga_device TEXT,
      bitwidth INTEGER,
      delay REAL
  );''')
# This is not necessary since we only have 1 connection.
con.commit()

active_jobs = []

for fpga_device in Devices:
  for name, hdl in FUs.items() :
    for bitwidth in Bitwidths:
      benchmark = MicroBenchmark(name=name, bitwidth=bitwidth, design_hdl=hdl, fpga_device=fpga_device)
      benchmark.generate_files(os.getcwd())
      benchmark.submit_job()
      active_jobs.append(benchmark)

for fpga_device in Devices:
  for bitwidth in  range(2, 513) :
    benchmark = MicroBenchmark(name="Mux", bitwidth=bitwidth, design_hdl=MuxHDL, fpga_device=fpga_device)
    benchmark.generate_files(os.getcwd())
    benchmark.submit_job()
    active_jobs.append(benchmark)

while active_jobs :
  next_active_jobs = []
  for job in active_jobs:
    status = Session.jobStatus(job.jobid)
    if status == drmaa.JobState.DONE or status == drmaa.JobState.FAILED:
      retval = Session.wait(job.jobid, drmaa.Session.TIMEOUT_WAIT_FOREVER)
      try:
        sys.stdout.write('\n')
        delay = job.processResults()
        con.execute('''
INSERT INTO
  delays(name, fpga_device, bitwidth, delay)
  VALUES (:name, :fpga_device, :bitwidth, :delay)
''', {
  'name' : job.name,
  'fpga_device' : job.fpga_device,
  'bitwidth' : job.bitwidth,
  'delay' : delay
})
      except:
        pass
    else:
      next_active_jobs.append(job)

  sys.stdout.write(' ')
  sys.stdout.write(str(len(active_jobs)))
  time.sleep(1)
  active_jobs = next_active_jobs[:]
  sys.stdout.flush()

with open('data.sql', 'w') as database_script:
  for line in con.iterdump():
    database_script.write(line)
    database_script.write('\n')
    print line
