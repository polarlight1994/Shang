FUs.BRam.Prefix = [=[altsyncram:]=]

FUs.BRam.Template=[=[
// Block Ram $(num)
reg  [$(datawidth - 1):0]  bram$(num)arrayout;
(* ramstyle = "no_rw_check" *) reg  [$(datawidth - 1):0]  bram$(num)array[0:$(size - 1)];

#if filename ~= [[]] then
initial
    $(_put('$'))readmemh("$(filepath .. '/' .. filename)", bram$(num)array);
#end
]=]

RunOnDatapath = [=[
#local Slack = RTLDatapath.Slack
#local EstimatiedDelay = RTLDatapath.EstimatiedDelay
#local DstNameSet = RTLDatapath.Nodes[1].NameSet
#local SrcNameSet = RTLDatapath.Nodes[table.getn(RTLDatapath.Nodes)].NameSet

foreach DstPattern $(DstNameSet) {
  set dst [get_keepers "*$(CurModule:getName())_inst|$DstPattern*"]

  if { [get_collection_size $dst] } { break }
}

foreach SrcPattern $(SrcNameSet) {
  set src [get_keepers "*$(CurModule:getName())_inst|$SrcPattern*"]

  if { [get_collection_size $src] } { break }
}

#if RTLDatapath.EstimatiedDelay ~= '<TNL not provided>' and RTLDatapath.Slack < RTLDatapath.EstimatiedDelay then
$(_put('#')) Arrival time violation detected!
#end

if {[get_collection_size $src] && [get_collection_size $dst]} {
#if (RTLDatapath.isCriticalPath == 1) then
$(_put('#')) $(DstNameSet) <- $(SrcNameSet) Slack $(Slack) EstimatiedDelay $(EstimatiedDelay)
  set_multicycle_path -from $src -to $dst -setup -end $(Slack)
#end -- Only generate constraits with -though if the current path is not critical.
#for i, n in pairs(RTLDatapath.Nodes) do
#  if (i~=1 and i~= table.getn(RTLDatapath.Nodes)) then
#local ThuNameSet = n.NameSet
$(_put('#')) $(DstNameSet) <- $(ThuNameSet) <- $(SrcNameSet) Slack $(Slack) EstimatiedDelay $(EstimatiedDelay)
#    if (RTLDatapath.isCriticalPath ~= 1) then
  foreach ThuPattern $(ThuNameSet) {
    set thu [get_nets "*$(CurModule:getName())_inst|$ThuPattern*"]

    if { [get_collection_size $thu] } { break }
  }

  if {[get_collection_size $thu]} {
    set_multicycle_path -from $src -through $thu -to $dst -setup -end $(Slack)
  }
#    end
#  end -- End valid thu node.
#end -- for
}
]=]

SDCHeader = [=[
create_clock -name "clk" -period $(PERIOD)ns [get_ports {clk}]
derive_pll_clocks -create_base_clocks
derive_clock_uncertainty
set_multicycle_path -from [get_clocks {clk}] -to [get_clocks {clk}] -hold -end 0
]=]

VerifyHeader = [=[
proc runOnPath { path max_ll min_ll from thu to } {
  # Accumulate the number of logic levels.
  set cur_path_ll [get_path_info $path -num_logic_levels]
  set cur_path_delay [get_path_info $path -data_delay]
  set cur_total_ic_delay 0
  set cur_slack [ get_path_info $path -slack]

  set PlotXY [open "[pwd]/PlotXY.data" a+]
  foreach_in_collection point [ get_path_info $path -arrival_points ] {
    if { [ get_point_info $point -type ] eq "ic" } {
      set cur_total_ic_delay [expr {$cur_total_ic_delay + [ get_point_info $point -incr     ]}]
    }
  }

  if [ expr $max_ll < $cur_path_ll ] {
    set status "underestimated"
  } else {
    set status "overestimated"
  }

  puts $PlotXY "$cur_path_ll $cur_path_delay $cur_total_ic_delay $cur_slack $max_ll $min_ll $from $thu $to $status"
  
  close $PlotXY
}

set PlotXY [open "[pwd]/PlotXY.data" w]
close $PlotXY
]=]

Misc.DatapathScript = [=[
local SlackFile = assert(io.open (MainSDCOutput, "a+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=RunOnDatapath, output=SlackFile}
if message ~= nil then print(message) end
SlackFile:close()
]=]

Misc.TimingConstraintsHeaderScript = [=[
local SlackFile = assert(io.open (MainSDCOutput, "w"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SDCHeader, output=SlackFile}
if message ~= nil then print(message) end
SlackFile:close()

local VerifyFile = assert(io.open (MainDelayVerifyOutput, "w"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=VerifyHeader, output=VerifyFile}
if message ~= nil then print(message) end
VerifyFile:close()
]=]

SynAttr.ParallelCaseAttr = '/* parallel_case */'
SynAttr.FullCaseAttr = '/* full_case */'

VerifyDatapathDelay = [=[
#for i, n in pairs(RTLDatapathDelay) do

#-- Ignore the invalid record
#  if n.Thu ~= '<null>' then
#    local DstNameSet = n.Dst.NameSet
#    local SrcNameSet = n.Src.NameSet
#    local Delay = n.Delay

foreach DstPattern $(DstNameSet) {
  set dst [get_keepers "*$(CurModule:getName())_inst|$DstPattern*"]
  if { [get_collection_size $dst] } { break }
}

foreach SrcPattern $(SrcNameSet) {
  set src [get_keepers "*$(CurModule:getName())_inst|$SrcPattern*"]
  if { [get_collection_size $src] } { break }
}

if {[get_collection_size $src] && [get_collection_size $dst]} {
#    if n.Thu ~= nil then
#    local ThuNameSet = n.Thu.NameSet
  foreach ThuPattern $(ThuNameSet) {
    set thu [get_nets "*$(CurModule:getName())_inst|$ThuPattern*"]
    if { [get_collection_size $thu] } { break }
  }

$(_put('#')) $(DstNameSet) <- $(ThuNameSet) <- $(SrcNameSet) delay $(Delay)

  if {[get_collection_size $thu]} {
    foreach_in_collection path [ get_timing_paths -from $src -through $thu -to $dst -nworst 1 -pairs_only -setup ] {
      runOnPath $path $(n.MaxLL) $(n.MinLL) "$(SrcNameSet)" "$(ThuNameSet)" "$(DstNameSet)"
    }
  }

#    else

$(_put('#')) $(DstNameSet) <- $(SrcNameSet) delay $(Delay)
  foreach_in_collection path [ get_timing_paths -from $src -to $dst -nworst 1 -pairs_only -setup ] {
    runOnPath $path $(n.MaxLL) $(n.MinLL) "$(SrcNameSet)" "<null>" "$(DstNameSet)"
  }
#    end
}
#  end
#end -- for
]=]

Misc.DelayVerifyScript = [=[
local VerifyFile = assert(io.open (MainDelayVerifyOutput, "a+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=VerifyDatapathDelay, output=VerifyFile}
if message ~= nil then print(message) end
VerifyFile:close()
]=]
