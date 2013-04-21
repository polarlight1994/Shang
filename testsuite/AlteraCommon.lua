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

RunOnDatapathCompact = [=[
#local Slack = RTLDatapath.Slack
#local EstimatiedDelay = RTLDatapath.EstimatiedDelay
#local DstNameSet = RTLDatapath.Nodes[1].NameSet
#local SrcNameSet = RTLDatapath.Nodes[table.getn(RTLDatapath.Nodes)].NameSet
#
#if (RTLDatapath.isCriticalPath == 1) then
  apply_from_to $(SrcNameSet) $(DstNameSet) $(Slack)
#end -- Only generate constraits with -though if the current path is not critical.
#
#if (table.getn(RTLDatapath.Nodes) ~= 2 and RTLDatapath.isCriticalPath ~= 1) then
  apply_from_thu_to $(SrcNameSet) $(DstNameSet) $(Slack) {
#  for i, n in pairs(RTLDatapath.Nodes) do
#    if (i~=1 and i~= table.getn(RTLDatapath.Nodes)) then
#      local ThuNameSet = n.NameSet
  $(ThuNameSet)
#    end
#  end
  }
#end
]=]

SDCHeader = [=[
create_clock -name "clk" -period $(PERIOD)ns [get_ports {clk}]
derive_pll_clocks -create_base_clocks
derive_clock_uncertainty
set_multicycle_path -from [get_clocks {clk}] -to [get_clocks {clk}] -hold -end 0

proc apply_from_to { Src Dst Slack } {
  foreach DstPattern $Dst {
    set dst [get_keepers "*$DstPattern*"]

    if { [get_collection_size $dst] } { break }
  }

  foreach SrcPattern $Src {
    set src [get_keepers "*$SrcPattern*"]

    if { [get_collection_size $src] } { break }
  }
  
  if {[get_collection_size $src] && [get_collection_size $dst]} {
    set_multicycle_path -from $src -to $dst -setup -end $Slack
  }
}

proc apply_from_thu_to { Src Dst Slack ThuNodes } {
  foreach DstPattern $Dst {
    set dst [get_keepers "*$DstPattern*"]

    if { [get_collection_size $dst] } { break }
  }

  foreach SrcPattern $Src {
    set src [get_keepers "*$SrcPattern*"]

    if { [get_collection_size $src] } { break }
  }

  if {[get_collection_size $src] && [get_collection_size $dst]} {
    foreach thu_node $ThuNodes {
      foreach ThuPattern $thu_node {
        set thu [get_nets "*$ThuPattern*"]

        if { [get_collection_size $thu] } { break }
      }
    
      if {[get_collection_size $thu]} {
        set_multicycle_path -from $src -through $thu -to $dst -setup -end $Slack
      }
    }
  }
}
]=]

Misc.GenerateMultiCycleConstraint = [=[
local SlackFile = assert(io.open (MainSDCOutput, "a+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=RunOnDatapathCompact, output=SlackFile}
if message ~= nil then print(message) end
SlackFile:close()
]=]

Misc.TimingConstraintsHeaderScript = [=[
local SlackFile = assert(io.open (MainSDCOutput, "w"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SDCHeader, output=SlackFile}
if message ~= nil then print(message) end
SlackFile:close()
]=]

SynAttr.ParallelCaseAttr = '/* parallel_case */'
SynAttr.FullCaseAttr = '/* full_case */'
