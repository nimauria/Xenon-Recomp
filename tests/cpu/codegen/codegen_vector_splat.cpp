// Generates native AOT code for every vector splat form and immediate, plus a
// case table, for x86_64/vector_splat.cpp to execute.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

namespace {
struct Form { const char* mnemonic; std::uint32_t base; bool vmx128; };
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  // VX forms: vD=1, vB=2, immediate/element in bits 16-20. VMX128 forms keep
  // their catalog registers; the runner finds the destination itself.
  const std::vector<Form> forms = {
      {"vspltb", 0x1000020Cu, false},   {"vsplth", 0x1000024Cu, false},
      {"vspltw", 0x1000028Cu, false},   {"vspltisb", 0x1000030Cu, false},
      {"vspltish", 0x1000034Cu, false}, {"vspltisw", 0x1000038Cu, false},
      {"vspltw128", 0x18000730u, true}, {"vspltisw128", 0x18000770u, true},
  };
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  std::vector<std::string> table;
  GuestAddress address = 0x6000u;
  for (const auto& form : forms) {
    for (std::uint32_t field = 0; field < 32u; ++field, address += 4u) {
      std::uint32_t word = form.base | (field << 16);
      if (!form.vmx128) word |= (1u << 21) | (2u << 11);
      const auto decoded = decoder.decode(address, word);
      if (!decoded.valid() || decoded.mnemonic() != form.mnemonic) {
        std::cerr << "decode failed for " << form.mnemonic << " field " << field << " got "
                  << decoded.mnemonic() << "\n";
        return 4;
      }
      ir::Block block{address, {}};
      ir::Builder builder(block);
      if (!lifter.lift(decoded, builder)) return 5;
      const auto name = std::string("splat_") + form.mnemonic + "_" + std::to_string(field);
      out << backend.emit_function(block, name) << "\n";
      table.push_back("  {\"" + std::string(form.mnemonic) + "\", " + std::to_string(word) + "u, " +
                      std::to_string(field) + "u, " + (form.vmx128 ? "true" : "false") +
                      ", aot::VectorSemantic::" + form.mnemonic + ", &" + name + "},");
    }
  }
  out << "struct SplatCase { const char* mnemonic; std::uint32_t word; std::uint32_t field; bool vmx128;"
         " aot::VectorSemantic semantic; ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&); };\n"
         "extern const SplatCase kSplatCases[] = {\n";
  for (const auto& row : table) out << row << "\n";
  out << "};\nextern const unsigned kSplatCaseCount = " << table.size() << "u;\n";
  return 0;
}
