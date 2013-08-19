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
    sc_signal<bool> mem0en;
    sc_signal<bool> mem0wen;
    sc_signal<uint32_t> mem0be; 
    $(getRetPort(FuncInfo.ReturnSize));
    sc_signal<uint$(FUs.MemoryBus.AddressWidth)_t> mem0addr;
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

      std::cerr <<"$(CurRTLModuleName) hardware run cycles " << cnt << " wait cycles " << memcnt <<endl;
      exit(0);
    }

    //Memory bus function

    // Stage 1, the memory recive the signals from the brige.
    void brige_pipe_1() {
      unsigned char addrmask = 0;
      while (true){
        if (mem0en.read()) {
          switch (mem0be.read()){
          case 1:  (mem0rdata) = *((uint8_t*)(mem0addr.read()));  addrmask = 0; break;
          case 3:  (mem0rdata) = *((uint16_t*)(mem0addr.read())); addrmask = 1; break;
          case 15: (mem0rdata) = *((uint32_t*)(mem0addr.read()));   addrmask = 3; break;
          case 255: (mem0rdata)= *((uint64_t*)(mem0addr.read())); addrmask = 7; break;
          default: assert(0 && "Unsupported size!"); break;
          }
        } else {
          mem0rdata = 0x0123456789abcdef;
        }

        if(mem0wen.read()) { // Write memory
          switch (mem0be.read()){
          case 1:  *((unsigned char *)(mem0addr.read())) = ((uint8_t) (mem0wdata.read()));   addrmask = 0; break;
          case 3:  *((uint16_t*)(mem0addr.read())) = ((uint16_t) (mem0wdata.read())); addrmask = 1; break;
          case 15: *((uint32_t*)(mem0addr.read())) = ((uint32_t) (mem0wdata.read()));     addrmask = 3; break;
          case 255: *((uint64_t*)(mem0addr.read())) = ((uint64_t) (mem0wdata.read())); addrmask = 7; break;
          default: assert(0 && "Unsupported size!"); break;
          }
        }

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
        DUT.mem0en(mem0en);
        DUT.mem0wen(mem0wen);
        DUT.mem0be(mem0be);
        DUT.mem0rdata(mem0rdata);
        DUT.mem0wdata(mem0wdata);
        DUT.mem0addr(mem0addr);
        SC_CTHREAD(sw_main_entry,clk.pos());
        SC_CTHREAD(brige_pipe_1,clk.pos());
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
    wait();
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
