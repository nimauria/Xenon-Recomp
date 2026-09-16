#include <cassert>
#include <cstddef>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;
using XenonGeneratedFn=ExecutionResult(*)(CpuState&,MemoryPort&,RuntimeServices&);
extern XenonGeneratedFn xenon_generated_all[];
extern std::size_t xenon_generated_all_count;

int main(){
  assert(xenon_generated_all_count==455);
  for(std::size_t i=0;i<xenon_generated_all_count;++i){
    CpuState state{};
    FlatMemory memory(0x20000u,0);
    NullRuntimeServices runtime;
    // Valid finite FP/vector defaults reduce host-library corner-case noise.
    state.set_fpr(0,1.0); state.set_fpr(1,2.0); state.gpr[1]=0x1000u;
    for(unsigned r=0;r<128;++r) for(unsigned b=0;b<16;++b) state.vr[r].bytes[b]=std::uint8_t(b);
    try { (void)xenon_generated_all[i](state,memory,runtime); }
    catch(const std::exception& e){ std::cerr<<"generated opcode #"<<i<<" threw: "<<e.what()<<"\n";return 2; }
  }
  std::cout<<"xenon_cpu_exhaustive_native: ok ("<<xenon_generated_all_count<<" native canonical executions)\n";
  return 0;
}
