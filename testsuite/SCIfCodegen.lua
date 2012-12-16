SCIFGScript = [=[
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

// Wrapper functions.
void *verilator_memset(void *src, int v, long long num) {
  return memset(src, v, num);
}

void *verilator_memcpy(void *dst, void *src, long long num) {
  return memcpy(dst, src, num);
}

void *verilator_memmove(void *dst, void *src, long long num) {
  return memmove(dst, src, num);
}

// Dirty Hack: Only C function is supported now.
$('#')ifdef __cplusplus
}
$('#')endif

]=]

SCIFFScript = [=[
#if Functions[FuncInfo.Name] ~= nil then
#local RTLModuleName = Functions[FuncInfo.Name].ModName
// And the header file of the generated module.
$('#')include "V$(RTLModuleName).h"
static int cnt = 0;
static int memcnt = 0;

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
    sc_signal<bool> mem0en;
    sc_signal<uint32_t> mem0cmd;
    sc_signal<uint32_t> mem0be;
    $(getRetPort(FuncInfo.ReturnSize));
    sc_signal<uint$(FUs.MemoryBus.AddressWidth)_t> mem0addr;
    sc_signal<uint64_t> mem0out;
    sc_signal<bool> rstN;
    sc_signal<bool> start;
    sc_signal<bool> mem0rdy;

    sc_signal<bool> mem0waitrequest;

    sc_signal<uint64_t>mem0in;
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
      ofstream outfile;
      outfile.open ("$(CounterFile)"); 
      outfile <<"$(RTLModuleName) hardware run cycles " << cnt << " wait cycles " << memcnt <<endl;
      outfile.close();
      outfile.open ("$(BenchmarkCycles)", ios_base::app); 
      outfile <<",\n{\"name\":\"$(RTLModuleName)\", \"total\":" << cnt << ", \"wait\":" << memcnt << '}' <<endl;
      outfile.close();
      exit(0);
    }

    //Memory bus function
    unsigned char brige_byte_en_pipe, brige_read_en_pipe, brige_read_ready, brige_write_en_pipe;
    long long brige_addr_pipe, brige_data_out_pipe, brige_data_in_pipe;

    // Stage 1, latch the signals in to the brige.
    void brige_pipe_1() {
      while (true){
        brige_byte_en_pipe = mem0be.read();
        brige_read_en_pipe = mem0en.read() && !mem0cmd.read();
        brige_write_en_pipe = mem0en.read() && mem0cmd.read();
        if (mem0en.read()) {
          brige_addr_pipe = mem0addr.read();
          brige_data_in_pipe = mem0out.read();
        } else {
          brige_addr_pipe = 0;
          brige_data_in_pipe = 0x0123456789abcdef;
        }

        wait();
      }
    }

    // Stage 2, the memory recive the signals from the brige.
    void brige_pipe_2() {
      unsigned char addrmask = 0;
      while (true){
        if (brige_read_en_pipe) {
          switch (brige_byte_en_pipe){
          case 1:  (brige_data_out_pipe) = *((unsigned char *)(brige_addr_pipe));  addrmask = 0; break;
          case 3:  (brige_data_out_pipe) = *((unsigned short *)(brige_addr_pipe)); addrmask = 1; break;
          case 15: (brige_data_out_pipe) = *((unsigned int *)(brige_addr_pipe));   addrmask = 3; break;
          case 255: (brige_data_out_pipe)= *((unsigned long long *)(brige_addr_pipe)); addrmask = 7; break;
          default: assert(0 && "Unsupported size!"); break;
          }
        } else {
          brige_data_out_pipe = 0x0123456789abcdef;
        }

        brige_read_ready = brige_read_en_pipe;

        if(brige_write_en_pipe) { // Write memory
          switch (brige_byte_en_pipe){
          case 1:  *((unsigned char *)(brige_addr_pipe)) = ((unsigned char ) (brige_data_in_pipe));   addrmask = 0; break;
          case 3:  *((unsigned short *)(brige_addr_pipe)) = ((unsigned short ) (brige_data_in_pipe)); addrmask = 1; break;
          case 15: *((unsigned int *)(brige_addr_pipe)) = ((unsigned int ) (brige_data_in_pipe));     addrmask = 3; break;
          case 255: *((unsigned long long *)(brige_addr_pipe)) = ((unsigned long long ) (brige_data_in_pipe)); addrmask = 7; break;
          default: assert(0 && "Unsupported size!"); break;
          }
        }

        wait();
      }
    }

    // Stage 3, output the signal.
    void brige_pipe_3() {
      while (true){
        mem0in = brige_data_out_pipe;
        mem0rdy = brige_read_ready ? 1 : 0;
        wait();
      }
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
        DUT.mem0en(mem0en);
        DUT.mem0cmd(mem0cmd);
        DUT.mem0rdy(mem0rdy);
        DUT.mem0in(mem0in);
        DUT.mem0out(mem0out);
        DUT.mem0be(mem0be);
        DUT.mem0addr(mem0addr);
        SC_CTHREAD(sw_main_entry,clk.pos());
        SC_CTHREAD(brige_pipe_1,clk.pos());
        SC_CTHREAD(brige_pipe_2,clk.pos());
        SC_CTHREAD(brige_pipe_3,clk.pos());
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

Passes.SCIFCodegen = { FunctionScript = [=[
local IfFile = assert(io.open (IFFileName, "a+"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SCIFFScript, output=IfFile}
if message ~= nil then print(message) end
IfFile:close()
]=], GlobalScript =[=[
local IfFile = assert(io.open (IFFileName, "w"))
local preprocess = require "luapp" . preprocess
local _, message = preprocess {input=SCIFGScript, output=IfFile}
if message ~= nil then print(message) end
IfFile:close()
]=]}
