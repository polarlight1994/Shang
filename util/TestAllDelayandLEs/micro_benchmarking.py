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
    self.working_dir = os.path.join(pwd, self.name + str(self.bitwidth) + self.fpga_device)
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
set results [ report_path -nworst 1 -from {input*} -to {result*} -through {dut*} ]
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
  input wire[%(bitwidth)s-1:0] lhs,
  input wire[%(bitwidth)s-1:0] rhs,
  input wire carry,
  output wire[%(bitwidth)s:0] result
);

assign result = lhs + rhs + carry;

endmodule

module top(
  input clk,
  input wire chain_in,
  input wire chain_in_en,
  input wire chain_out_en,
  output reg chain_out
);

  reg [%(bitwidth)s-1:0] lhs_pipe0;
  reg [%(bitwidth)s-1:0] input_lhs;
  reg [%(bitwidth)s-1:0] rhs_pipe0;
  reg [%(bitwidth)s-1:0] input_rhs;
  reg carry_pipe0;
  reg input_carry;
  reg  [%(bitwidth)s:0]result_pipe0;
  reg  [%(bitwidth)s:0]result_pipe1;
  wire [%(bitwidth)s:0]result_wire;
always@(posedge clk) begin
  if (chain_in_en) begin
    lhs_pipe0[0] <= chain_in;
    lhs_pipe0[%(bitwidth)s-1:1] <= lhs_pipe0[%(bitwidth)s-1-1:0];

    rhs_pipe0[0] <= lhs_pipe0[%(bitwidth)s-1];
    rhs_pipe0[%(bitwidth)s-1:1] <= rhs_pipe0[%(bitwidth)s-1-1:0];

    carry_pipe0 <= rhs_pipe0[%(bitwidth)s-1];
  end

  input_lhs <= lhs_pipe0;
  input_rhs <= rhs_pipe0;
  input_carry <= carry_pipe0;
  result_pipe0 <= result_wire;

  if (chain_out_en) begin
    result_pipe1[%(bitwidth)s:1] <= result_pipe1[%(bitwidth)s-1:0];
    chain_out <= result_pipe1[%(bitwidth)s];
  end else begin
    result_pipe1 <= result_pipe0;
  end
end

%(name)s dut(
  .lhs(input_lhs),
  .rhs(input_rhs),
  .carry(input_carry),
  .result(result_wire)
);

endmodule
''',
'Mult' : '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] lhs,
  input wire[%(bitwidth)s-1:0] rhs,
  output wire[%(bitwidth)s - 1:0] result
);

assign result = lhs * rhs;

endmodule

module top(
  input clk,
  input wire chain_in,
  input wire chain_in_en,
  input wire chain_out_en,
  output reg chain_out
);

  reg [%(bitwidth)s-1:0] lhs_pipe0;
  reg [%(bitwidth)s-1:0] input_lhs;
  reg [%(bitwidth)s-1:0] rhs_pipe0;
  reg [%(bitwidth)s-1:0] input_rhs;
  reg  [%(bitwidth)s-1:0]result_pipe0;
  reg  [%(bitwidth)s-1:0]result_pipe1;
  wire [%(bitwidth)s-1:0]result_wire;
always@(posedge clk) begin
  if (chain_in_en) begin
    lhs_pipe0[0] <= chain_in;
    lhs_pipe0[%(bitwidth)s-1:1] <= lhs_pipe0[%(bitwidth)s-1-1:0];

    rhs_pipe0[0] <= lhs_pipe0[%(bitwidth)s-1];
    rhs_pipe0[%(bitwidth)s-1:1] <= rhs_pipe0[%(bitwidth)s-1-1:0];
  end

  input_lhs <= lhs_pipe0;
  input_rhs <= rhs_pipe0;

  result_pipe0 <= result_wire;

  if (chain_out_en) begin
    result_pipe1[%(bitwidth)s-1:1] <= result_pipe1[%(bitwidth)s-1-1:0];
    chain_out <= result_pipe1[%(bitwidth)s-1];
  end else begin
    result_pipe1 <= result_pipe0;
  end
end

%(name)s dut(
  .lhs(input_lhs),
  .rhs(input_rhs),
  .result(result_wire)
);

endmodule
''',
'Shift' : '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] lhs,
  input wire[%(bitwidth)s-1:0] rhs,
  output wire[%(bitwidth)s-1:0] result
);

assign result = lhs >> rhs;

endmodule

module top(
  input clk,
  input wire chain_in,
  input wire chain_in_en,
  input wire chain_out_en,
  output reg chain_out
);

  reg [%(bitwidth)s-1:0] lhs_pipe0;
  reg [%(bitwidth)s-1:0] input_lhs;
  reg [%(bitwidth)s-1:0] rhs_pipe0;
  reg [%(bitwidth)s-1:0] input_rhs;
  reg  [%(bitwidth)s-1:0]result_pipe0;
  reg  [%(bitwidth)s-1:0]result_pipe1;
  wire [%(bitwidth)s-1:0]result_wire;
always@(posedge clk) begin
  if (chain_in_en) begin
    lhs_pipe0[0] <= chain_in;
    lhs_pipe0[%(bitwidth)s-1:1] <= lhs_pipe0[%(bitwidth)s-1-1:0];

    rhs_pipe0[0] <= lhs_pipe0[%(bitwidth)s-1];
    rhs_pipe0[%(bitwidth)s-1:1] <= rhs_pipe0[%(bitwidth)s-1-1:0];
  end

  input_lhs <= lhs_pipe0;
  input_rhs <= rhs_pipe0;

  result_pipe0 <= result_wire;

  if (chain_out_en) begin
    result_pipe1[%(bitwidth)s-1:1] <= result_pipe1[%(bitwidth)s-1-1:0];
    chain_out <= result_pipe1[%(bitwidth)s-1];
  end else begin
    result_pipe1 <= result_pipe0;
  end
end

%(name)s dut(
  .lhs(input_lhs),
  .rhs(input_rhs),
  .result(result_wire)
);

endmodule
''',
'ICmp' : '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] lhs,
  input wire[%(bitwidth)s-1:0] rhs,
  output wire result
);

assign result = (lhs > rhs)? 1:0;

endmodule

module top(
  input clk,
  input wire chain_in,
  input wire chain_in_en,
  output reg chain_out
);

  reg [%(bitwidth)s-1:0] lhs_pipe0;
  reg [%(bitwidth)s-1:0] input_lhs;
  reg [%(bitwidth)s-1:0] rhs_pipe0;
  reg [%(bitwidth)s-1:0] input_rhs;
  reg  result_pipe0;
  wire  result_wire;
always@(posedge clk) begin
  if (chain_in_en) begin
    lhs_pipe0[0] <= chain_in;
    lhs_pipe0[%(bitwidth)s-1:1] <= lhs_pipe0[%(bitwidth)s-1-1:0];

    rhs_pipe0[0] <= lhs_pipe0[%(bitwidth)s-1];
    rhs_pipe0[%(bitwidth)s-1:1] <= rhs_pipe0[%(bitwidth)s-1-1:0];
  end

  input_lhs <= lhs_pipe0;
  input_rhs <= rhs_pipe0;

  result_pipe0 <= result_wire;
  chain_out <= result_pipe0;
end

%(name)s dut(
  .lhs(input_lhs),
  .rhs(input_rhs),
  .result(result_wire)
);

endmodule
'''
}

MuxHDL = '''
module %(name)s(
  input wire[%(bitwidth)s-1:0] fis,
  input wire[%(bitwidth)s-1:0] ens,
  output wire sel_output
);
  integer l;
  reg sel_output_internal;
  always @(*) begin
    sel_output_internal = 1'b0;
    for(l = 0; l < %(bitwidth)s; l = l + 1) 
      sel_output_internal = sel_output_internal | (fis[l] & ens[l]);
  end

  assign sel_output = sel_output_internal;
endmodule


module top(
  input clk,
  input wire chain_in,
  input wire chain_in_en,
  output reg chain_out
);

  reg [%(bitwidth)s-1:0] ens_pipe0;
  reg [%(bitwidth)s-1:0] input_ens;

  reg [%(bitwidth)s-1:0] fis_pipe0;
  reg [%(bitwidth)s-1:0] input_fis;

  reg  result_pipe0;
  wire  result_wire;

always@(posedge clk) begin
  if (chain_in_en) begin
    ens_pipe0[0] <= chain_in;
    ens_pipe0[%(bitwidth)s-1:1] <= ens_pipe0[%(bitwidth)s-1-1:0];

    fis_pipe0[0] <= ens_pipe0[%(bitwidth)s-1];
    fis_pipe0[%(bitwidth)s-1:1] <= fis_pipe0[%(bitwidth)s-1-1:0];
  end

  input_ens <= ens_pipe0;
  input_fis <= fis_pipe0;

  result_pipe0 <= result_wire;
  chain_out <= result_pipe0;
end

%(name)s dut(
  .fis(input_fis),
  .ens(input_ens),
  .sel_output(result_wire)
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
