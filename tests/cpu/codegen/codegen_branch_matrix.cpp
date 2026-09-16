#include <fstream>
#include <iostream>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";

  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  constexpr GuestAddress address = 0x2000u;
  constexpr unsigned bi = 5u;
  for (unsigned bo = 0; bo < 32; ++bo) {
    const std::uint32_t word = 0x40000000u | (bo << 21) | (bi << 16) | 0x8u;
    auto decoded = decoder.decode(address, word);
    if (!decoded.valid() || decoded.mnemonic() != "bcx") return 4;
    ir::Block block{address,{}};
    ir::Builder builder(block);
    if (!lifter.lift(decoded,builder)) return 5;
    out << backend.emit_function(block,"branch_bo_" + std::to_string(bo)) << "\n";
  }
  out << "using BranchFn=ExecutionResult(*)(CpuState&,MemoryPort&,RuntimeServices&);\n";
  out << "BranchFn xenon_branch_matrix[32]={\n";
  for (unsigned bo=0;bo<32;++bo) out << "  &branch_bo_" << bo << ",\n";
  out << "};\n";
  return 0;
}
