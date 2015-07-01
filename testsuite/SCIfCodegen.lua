SCIFTemplate = [=[
// Include the headers.
$('#')include "systemc.h"
$('#')include "verilated.h"
$('#')include <iostream>
$('#')include <fstream>
using namespace std;

// GlobalVariables
$('#')ifdef __cplusplus
extern "C" {
$('#')endif

// Dirty Hack: Only C function is supported now.
$('#')ifdef __cplusplus
}
$('#')endif

#if FuncInfo.Name ~= nil then
#local RTLModuleName = FuncInfo.Name
// And the header file of the generated module.
$('#')include "V$(RTLModuleName).h"
static int cnt = 0;

//Including the functions which used in the tb
$('#')ifdef __cplusplus
extern"C"{
$('#')endif
$(getType(FuncInfo.ReturnSize)) $(FuncInfo.Name)_if($(
  for i,v in ipairs(FuncInfo.Args) do
    if i ~= 1 then _put(', ') end
    _put(getType(v.Size) .. ' '.. v.Name)
  end
 ));

#if FuncInfo.Name ~= "main" then
int sw_main();
#end

$('#')ifdef __cplusplus
}
$('#')endif

//Top module here
SC_MODULE(V$(RTLModuleName)_tb){
  public:
    sc_in_clk clk;
    sc_signal<bool> fin;
    $(getRetPort(FuncInfo.ReturnSize));
    sc_signal<bool> rstN;
    sc_signal<bool> start;
#for i,v in ipairs(FuncInfo.Args) do
    sc_signal<$(getBitWidth(v.Size))>$(v.Name);
#end

    V$(RTLModuleName) DUT;
    
    void sw_main_entry(){
      V$(RTLModuleName)_tb *tb_ptr = V$(RTLModuleName)_tb::Instance();
      wait(); tb_ptr->rstN = 0;
      wait(10); tb_ptr->rstN = 1;
      wait();
#if FuncInfo.Name ~= "main"	then
			sw_main();
#else
			$(getType(FuncInfo.ReturnSize)) RetVle = $(FuncInfo.Name)_if($(
				for i,v in ipairs(FuncInfo.Args) do
					if i ~= 1 then _put(', ') end
					_put(v.Name)
				end
			 ));
		  assert(RetVle == 0 && "Return value of main function is not 0!");
#end
      sc_stop();
    }    

    static V$(RTLModuleName)_tb* Instance() {
      static V$(RTLModuleName)_tb _instance("top");
      return &_instance ;
    }

    protected:
    //Include the DUT in the top module
      SC_CTOR(V$(RTLModuleName)_tb): DUT("DUT"){
        DUT.clk(clk);
#for i,v in ipairs(FuncInfo.Args) do
        DUT.$(v.Name)($(v.Name));
#end        
        DUT.fin(fin);
        //whether there is a return value
#if FuncInfo.ReturnSize~=0 then
        DUT.return_value(return_value);  
#else
        
#end
        DUT.start(start);
        DUT.rstN(rstN);
        SC_CTHREAD(sw_main_entry,clk.pos());
      }
    private:
      V$(RTLModuleName)_tb(const V$(RTLModuleName)_tb&) ;
      V$(RTLModuleName)_tb& operator=(const V$(RTLModuleName)_tb&) ;
    };  

  $(getType(FuncInfo.ReturnSize)) $(FuncInfo.Name)_if($(
    for i,v in ipairs(FuncInfo.Args) do
      if i ~= 1 then _put(', ') end
      _put(getType(v.Size) .. ' '.. v.Name)
    end
  )){
    V$(RTLModuleName)_tb *tb_ptr = V$(RTLModuleName)_tb::Instance();
#for i,v in ipairs(FuncInfo.Args) do
    tb_ptr->$(v.Name)=$(v.Name);
#end       
    tb_ptr->start=1;
    wait(); tb_ptr->start=0;
    wait(tb_ptr->clk == 1);
    while(!(tb_ptr->fin)){
      wait();
      ++cnt;
    }
    
    //printf("$(RTLModuleName) finish\n");
#if FuncInfo.ReturnSize~=0 then
    return ($(getType(FuncInfo.ReturnSize))) tb_ptr->return_value;      
#else 
    return;
#end
  }

  //The main function called sc_main in SystemC  
  int sc_main(int argc, char **argv) {
    //Close the information which provided by SystemC
    sc_report_handler::set_actions("/IEEE_Std_1666/deprecated", SC_DO_NOTHING);
    
    Verilated::commandArgs(argc,argv);
    sc_clock clk ("clk",10, 0.5, 3, true);
    V$(RTLModuleName)_tb *top = V$(RTLModuleName)_tb::Instance();
    //Link the stimulate to the clk
    top->clk(clk);
    //Start the test
    sc_start();

    return 0;
  }  
#end  
]=]

SCIFCodeGen = [=[
local IfFile = assert(io.open (IFFileName, "a+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SCIFTemplate, output=IfFile}
if message ~= nil then print(message) end
IfFile:close()
]=]