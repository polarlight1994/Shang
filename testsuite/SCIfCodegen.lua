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

#for k,v in pairs(GlobalVariables) do
#if v.AddressSpace == 0 then
void *vlt_$(escapeNumber(k))() {
  $(if v.isLocal == 1 then _put('static') else _put('extern') end)
#if v.Alignment~=0 then
  __attribute__((aligned($(v.Alignment))))
#end
  $(getType(v.ElemSize)) $(k)$(if v.NumElems > 1 then  _put('[' .. v.NumElems .. ']') end)
  $(if v.Initializer ~= nil then
    _put(' = {')
    for i,n in ipairs(v.Initializer) do
      if i ~= 1 then _put(', ') end
      _put(n)
      if v.ElemSize == 32 then _put('u')
      elseif v.ElemSize > 32 then _put('ull')
      end
    end
    _put('}')
  end);
  return (void *)$(if v.NumElems == 1 then  _put('&') end)$(k);
}
#end --end addresssapce == 0
#end

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
    sc_signal<bool> rstN;
    sc_signal<bool> start;
    $(getRetPort(FuncInfo.ReturnSize))
#for i,v in ipairs(FuncInfo.Args) do
    sc_signal<$(getBitWidth(v.Size))> $(v.Name);
#end
#for i,v in pairs(VirtualMBs) do
	sc_signal<bool> $(v.EnableName);
	sc_signal<bool> $(v.WriteEnName);
#if v.RequireByteEn == 1 then
	sc_signal<$(getBitWidth(v.ByteEnWidth))> $(v.ByteEnName);
#end
	sc_signal<$(getBitWidth(v.AddrWidth))> $(v.AddrName);
	sc_signal<$(getBitWidth(v.DataWidth))> $(v.WDataName);
	sc_signal<$(getBitWidth(v.DataWidth))> $(v.RDataName);
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
		ofstream outfile;
		outfile.open ("$(CounterFile)");
		outfile <<"$(RTLModuleName) hardware run cycles " << cnt <<endl;
		outfile.close();
		sc_stop();
    }

#for i,v in pairs(VirtualMBs) do
	void $(v.BankAccessFunc)() {
		while(true) {
			if(($(v.EnableName))) {
			    unsigned CyclesToWait = 0;
				long long cur_addr = $(v.AddrName).read();
#if v.RequireByteEn == 1 then
				$(getType(v.ByteEnWidth)) cur_be = $(v.ByteEnName).read();
				if(($(v.WriteEnName))) {
					CyclesToWait = 1;
					switch (cur_be) {
						case 1:   *((unsigned char *)cur_addr) = ((unsigned char)$(v.WDataName).read()); break;
						case 2:   *((unsigned char *)cur_addr) = ((unsigned char)($(v.WDataName).read() >> 8)); break;
						case 4:   *((unsigned char *)cur_addr) = ((unsigned char)($(v.WDataName).read() >> 16)); break;
						case 8:   *((unsigned char *)cur_addr) = ((unsigned char)($(v.WDataName).read() >> 24)); break;
						case 16:   *((unsigned char *)cur_addr) = ((unsigned char)($(v.WDataName).read() >> 32)); break;
						case 32:   *((unsigned char *)cur_addr) = ((unsigned char)($(v.WDataName).read() >> 40)); break;
						case 64:   *((unsigned char *)cur_addr) = ((unsigned char)($(v.WDataName).read() >> 48)); break;
						case 128:   *((unsigned char *)cur_addr) = ((unsigned char)($(v.WDataName).read() >> 56)); break;

						case 3:   *((unsigned short *)cur_addr) = ((unsigned short)$(v.WDataName).read()); break;
						case 12:  *((unsigned short *)cur_addr) = ((unsigned short)($(v.WDataName).read() >> 16)); break;
						case 48:  *((unsigned short *)cur_addr) = ((unsigned short)($(v.WDataName).read() >> 32)); break;
						case 192: *((unsigned short *)cur_addr) = ((unsigned short)($(v.WDataName).read() >> 48)); break;

						case 15:  *((unsigned int *)cur_addr) = ((unsigned int)$(v.WDataName).read()); break;
						case 240: *((unsigned int *)cur_addr) = ((unsigned int)($(v.WDataName).read() >> 32)); break;

						case 255: *((unsigned long long *)cur_addr) = ((unsigned long long)$(v.WDataName).read()); break;
						default: printf("cur_be is %d\n", cur_be); assert(0 && "Unsupported cur_be!"); break;
					}
				} else {
					CyclesToWait = 2;
					switch (cur_be) {
						case 1:   $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)); break;
						case 2:   $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)) << 8; break;
						case 4:   $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)) << 16; break;
						case 8:   $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)) << 24; break;
						case 16:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)) << 32; break;
						case 32:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)) << 40; break;
						case 64:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)) << 48; break;
						case 128:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned char *)cur_addr)) << 56; break;

						case 3:   $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned short *)cur_addr)); break;
						case 12:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned short *)cur_addr)) << 16; break;
						case 48:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned short *)cur_addr)) << 32; break;
						case 192:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned short *)cur_addr)) << 48; break;

						case 15:  $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned int *)cur_addr)); break;
						case 240: $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned int *)cur_addr)) << 32; break;

						case 255: $(v.RDataName) = ($(getType(v.DataWidth)))(*((unsigned long long *)cur_addr)); break;
						default: printf("cur_be is %d\n", cur_be); assert(0 && "Unsupported cur_be!"); break;
					}
				}
#else
				if(($(v.WriteEnName))) {
					CyclesToWait = 1;
					*(($(getType(v.DataWidth)) *)cur_addr) = (($(getType(v.DataWidth)))$(v.WDataName).read());
				} else {
					CyclesToWait = 2;
					$(v.RDataName) = *(($(getType(v.DataWidth)) *)cur_addr);
				}
#end
				wait();
				for (unsigned i = 0; i < CyclesToWait - 1; ++i) {
					wait();
					if (($(v.EnableName))) {
						printf("In cycles %d\n", i);
					}
					assert(!($(v.EnableName)) && "Please disable memory while waiting it ready!");
				}
			} else {
				wait();
			}
		}
	}
#end

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

#for i,v in pairs(VirtualMBs) do
		DUT.$(v.EnableName)($(v.EnableName));
		DUT.$(v.WriteEnName)($(v.WriteEnName));
#if v.RequireByteEn == 1 then
		DUT.$(v.ByteEnName)($(v.ByteEnName));
#end
		DUT.$(v.AddrName)($(v.AddrName));
		DUT.$(v.WDataName)($(v.WDataName));
		DUT.$(v.RDataName)($(v.RDataName));
#end

        SC_CTHREAD(sw_main_entry,clk.pos());
#for i,v in pairs(VirtualMBs) do
		SC_CTHREAD($(v.BankAccessFunc), clk.pos());
#end
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
local IfFile = assert(io.open (IFFileName, "w"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SCIFTemplate, output=IfFile}
if message ~= nil then print(message) end
IfFile:close()
]=]

GlobalVariableTemplate = [=[
/* verilator lint_off DECLFILENAME */
/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */

#for k,v in pairs(GlobalVariables) do
#if v.AddressSpace == 0 then
import "DPI-C" function chandle vlt_$(escapeNumber(k))();
`define gv$(k) vlt_$(escapeNumber(k))()
#end
#end
]=]

GlobalVariableCodeGen = [=[
local preprocess = require "luapp" . preprocess
GlobalVariableCode, message = preprocess {input=GlobalVariableTemplate}
if message ~= nil then print(message) end
]=]