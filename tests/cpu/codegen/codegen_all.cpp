#include <fstream>
#include <iostream>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

int main(int argc,char**argv){
  if(argc!=2){std::cerr<<"usage: xenon_cpu_codegen_all output.cpp\n";return 2;}
  std::ofstream out(argv[1]); if(!out)return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\n\nusing namespace xenon::cpu;\n";
  Decoder decoder; Lifter lifter; backend::CppAotBackend backend;
  std::size_t n=0;
  for(const auto&op:Decoder::opcode_catalog()){
    auto di=decoder.decode(0x84000000u,op.pattern);
    ir::Block block{0x84000000u,{}};ir::Builder b(block);
    if(!lifter.lift(di,b)){std::cerr<<"lift failed: "<<op.mnemonic<<"\n";return 4;}
    out << backend.emit_function(block,"generated_"+std::to_string(n++)) << "\n";
  }
  out << "using XenonGeneratedFn=ExecutionResult(*)(CpuState&,MemoryPort&,RuntimeServices&);\n";
  out << "XenonGeneratedFn xenon_generated_all[] = {\n";
  for(std::size_t i=0;i<n;++i) out << "  &generated_" << i << ",\n";
  out << "};\nstd::size_t xenon_generated_all_count = " << n << ";\n";
  std::cout<<n<<" generated functions\n";
  return n==455?0:5;
}
