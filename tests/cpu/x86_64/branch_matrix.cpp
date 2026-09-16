#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;
using BranchFn=ExecutionResult(*)(CpuState&,MemoryPort&,RuntimeServices&);
extern BranchFn xenon_branch_matrix[32];

static bool expected_branch(unsigned bo, bool cr, std::uint64_t initial_ctr,
                            std::uint64_t& final_ctr) {
  final_ctr=initial_ctr;
  bool ctr_ok=true;
  if ((bo & 0b00100u)==0) {
    final_ctr=initial_ctr-1u;
    const bool want_zero=(bo & 0b00010u)!=0;
    ctr_ok=want_zero ? final_ctr==0 : final_ctr!=0;
  }
  bool cr_ok=true;
  if ((bo & 0b10000u)==0) {
    const bool want_true=(bo & 0b01000u)!=0;
    cr_ok=cr==want_true;
  }
  return ctr_ok && cr_ok;
}

int main(){
  FlatMemory mem(0x1000u,0x1000u);
  NullRuntimeServices runtime;
  constexpr std::array<std::uint64_t,5> ctrs={0u,1u,2u,3u,0xFFFFFFFFFFFFFFFFull};
  std::size_t cases=0;
  for(unsigned bo=0;bo<32;++bo){
    for(bool cr:{false,true}){
      for(auto ctr:ctrs){
        CpuState state{}; state.ctr=ctr; state.set_cr_bit(5,cr);
        std::uint64_t expected_ctr{};
        const bool taken=expected_branch(bo,cr,ctr,expected_ctr);
        const auto rr=xenon_branch_matrix[bo](state,mem,runtime);
        assert(state.ctr==expected_ctr);
        if(taken){assert(rr.reason==FlowReason::Branch);assert(rr.next_address==0x2008u);}
        else assert(rr.reason==FlowReason::Fallthrough);
        ++cases;
      }
    }
  }
  std::cout<<"xenon_cpu_branch_matrix: ok ("<<cases<<" BO/CR/CTR cases)\n";
}
