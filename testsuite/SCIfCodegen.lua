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

-- DIRTY HACK: Generate an interface file for each module even it is not the top
-- level module. However, only the interface file for the top-level module will
-- be compile and link.
-- Also no need to worry about the main function at this moment because this
-- script is only run on the hybrid flow.
SCIFFScript = [=[
#if FuncInfo.Name == RTLModuleName then
#local CurRTLModuleName = FuncInfo.Name
// Include the header file of the generated module.
$('#')include "V$(CurRTLModuleName).h"
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

int sw_main();

$('#')ifdef __cplusplus
}
$('#')endif

//Top module here
SC_MODULE(V$(CurRTLModuleName)_tb){
  public:
    sc_in_clk clk;
    sc_signal<bool> fin;
    sc_signal<bool> mem0ren;
    sc_signal<bool> mem0wen;
    sc_signal<uint32_t> mem0rbe;
    sc_signal<uint32_t> mem0wbe;
    $(getRetPort(FuncInfo.ReturnSize));
    sc_signal<uint$(FUs.MemoryBus.AddressWidth)_t> mem0raddr;
    sc_signal<uint$(FUs.MemoryBus.AddressWidth)_t> mem0waddr;
    sc_signal<uint64_t> mem0rdata;
    sc_signal<uint64_t> mem0wdata;
    sc_signal<bool> rstN;
    sc_signal<bool> start;

#for i,v in ipairs(FuncInfo.Args) do
    sc_signal<$(getBitWidth(v.Size))>$(v.Name);
#end

    V$(CurRTLModuleName) DUT;
    
    void sw_main_entry(){
      V$(CurRTLModuleName)_tb *tb_ptr = V$(CurRTLModuleName)_tb::Instance();
      wait(); tb_ptr->rstN = 0;
      wait(10); tb_ptr->rstN = 1;
      wait();

			sw_main();

      ofstream outfile;
      outfile.open ("$(CounterFile)"); 
      outfile <<"$(CurRTLModuleName) hardware run cycles " << cnt << " wait cycles " << memcnt <<endl;
      outfile.close();
      outfile.open ("$(BenchmarkCycles)", ios_base::app); 
      outfile <<",\n{\"name\":\"$(CurRTLModuleName)\", \"total\":" << cnt << ", \"wait\":" << memcnt << '}' <<endl;
      outfile.close();
      exit(0);
    }

    //Memory bus function
    bool brige_read_en_pipe, brige_read_ready, brige_write_en_pipe;
    uint8_t brige_read_byte_en_pipe, brige_write_byte_en_pipe;
    uint64_t brige_raddr_pipe, brige_waddr_pipe, brige_read_data_pipe, brige_write_data_pipe;

    // Stage 1, latch the signals in to the brige.
    void brige_pipe_1() {
      while (true){
        brige_read_byte_en_pipe = mem0rbe.read();
        brige_write_byte_en_pipe = mem0wbe.read();
        brige_read_en_pipe = mem0ren.read();
        brige_write_en_pipe = mem0wen.read();
        brige_raddr_pipe = mem0raddr.read();;
        brige_waddr_pipe = mem0waddr.read();
        brige_write_data_pipe = mem0wdata.read();

        wait();
      }
    }

    // Stage 2, the memory recive the signals from the brige.
    void brige_pipe_2() {
      unsigned char addrmask = 0;
      while (true){
        if (brige_read_en_pipe) {
          switch (brige_read_byte_en_pipe){
          case 1:  (brige_read_data_pipe) = *((uint8_t*)(brige_raddr_pipe));  addrmask = 0; break;
          case 3:  (brige_read_data_pipe) = *((uint16_t*)(brige_raddr_pipe)); addrmask = 1; break;
          case 15: (brige_read_data_pipe) = *((uint32_t*)(brige_raddr_pipe));   addrmask = 3; break;
          case 255: (brige_read_data_pipe)= *((uint64_t*)(brige_raddr_pipe)); addrmask = 7; break;
          default: assert(0 && "Unsupported size!"); break;
          }
        } else {
          brige_read_data_pipe = 0x0123456789abcdef;
        }

        brige_read_ready = brige_read_en_pipe;

        if(brige_write_en_pipe) { // Write memory
          switch (brige_write_byte_en_pipe){
          case 1:  *((unsigned char *)(brige_waddr_pipe)) = ((uint8_t) (brige_write_data_pipe));   addrmask = 0; break;
          case 3:  *((uint16_t*)(brige_waddr_pipe)) = ((uint16_t) (brige_write_data_pipe)); addrmask = 1; break;
          case 15: *((uint32_t*)(brige_waddr_pipe)) = ((uint32_t) (brige_write_data_pipe));     addrmask = 3; break;
          case 255: *((uint64_t*)(brige_waddr_pipe)) = ((uint64_t) (brige_write_data_pipe)); addrmask = 7; break;
          default: assert(0 && "Unsupported size!"); break;
          }
        }

        wait();
      }
    }

    // Stage 3, output the signal.
    void brige_pipe_3() {
      while (true){
        mem0rdata = brige_read_data_pipe;
        wait();
      }
    }

    static V$(CurRTLModuleName)_tb* Instance() {
      static V$(CurRTLModuleName)_tb _instance("top");
      return &_instance ;
    }

    protected:
    //Include the DUT in the top module
      SC_CTOR(V$(CurRTLModuleName)_tb): DUT("DUT"){
        DUT.clk(clk);
#for i,v in ipairs(FuncInfo.Args) do
        DUT.$(v.Name)($(v.Name));
#end        
        DUT.fin(fin);
        //whether there is a return value
#if FuncInfo.ReturnSize~=0 then
        DUT.return_value(return_value);          
#end
        DUT.start(start);
        DUT.rstN(rstN);
        DUT.mem0ren(mem0ren);
        DUT.mem0wen(mem0wen);
        DUT.mem0rbe(mem0rbe);
        DUT.mem0wbe(mem0wbe);
        DUT.mem0rdata(mem0rdata);
        DUT.mem0wdata(mem0wdata);
        DUT.mem0raddr(mem0raddr);
        DUT.mem0waddr(mem0waddr);
        SC_CTHREAD(sw_main_entry,clk.pos());
        SC_CTHREAD(brige_pipe_1,clk.pos());
        SC_CTHREAD(brige_pipe_2,clk.pos());
        SC_CTHREAD(brige_pipe_3,clk.pos());
      }
    private:
      V$(CurRTLModuleName)_tb(const V$(CurRTLModuleName)_tb&) ;
      V$(CurRTLModuleName)_tb& operator=(const V$(CurRTLModuleName)_tb&) ;
    };  

  $(getType(FuncInfo.ReturnSize)) $(FuncInfo.Name)_if($(
    for i,v in ipairs(FuncInfo.Args) do
      if i ~= 1 then _put(', ') end
      _put(getType(v.Size) .. ' '.. v.Name)
    end
  )){
    V$(CurRTLModuleName)_tb *tb_ptr = V$(CurRTLModuleName)_tb::Instance();
#for i,v in ipairs(FuncInfo.Args) do
    tb_ptr->$(v.Name)=$(v.Name);
#end       
    tb_ptr->start=1;
    wait();
    tb_ptr->start=0;
    while(!(tb_ptr->fin)){
      wait();
      ++cnt;
    }
    
    //printf("$(CurRTLModuleName) finish\n");
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
    V$(CurRTLModuleName)_tb *top = V$(CurRTLModuleName)_tb::Instance();
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
