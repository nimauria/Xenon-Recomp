// Generates real AOT code for every vector case of the xenia-project/xenia
// PPC instruction tests (tests/cpu/xenia_ppc), for x86_64/xenia_ppc.cpp to
// run against the values recorded on Xbox 360 hardware. Each case's
// instructions up to its closing blr are lifted into one block, as the
// recompiler lifts straight-line guest code.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"
#include "../xenia_ppc/xenia_ppc_corpus.hpp"

namespace {
#include "../xenia_ppc/xenia_ppc_vector_corpus.inc"
#include "../xenia_ppc/xenon_vmx128_operands.inc"
const XeniaPpcCorpus kCorpora[] = {{kXeniaPpcCases, kXeniaPpcCaseCount},
                                   {kXenonPpcCases, kXenonPpcCasesCount}};
}  // namespace

using namespace xenon::cpu;

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  constexpr std::uint32_t kBase = 0x6000u;
  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  std::string table = "extern const XeniaPpcCompiled kXeniaPpcCompiled[] = {\n";
  unsigned n = 0;
  for (const auto& corpus : kCorpora)
  for (unsigned k = 0; k < corpus.count; ++k, ++n) {
    const auto& c = corpus.cases[k];
    ir::Block block{kBase, {}};
    ir::Builder builder(block);
    std::string failure;
    for (unsigned i = 0; i + 1 < c.word_count && failure.empty(); ++i) {
      const auto decoded = decoder.decode(kBase + i * 4u, c.words[i]);
      if (!decoded.valid())
        failure = "does not decode";
      else if (!lifter.lift(decoded, builder))
        failure = std::string("does not lift: ") + std::string(decoded.mnemonic());
    }
    const std::string name = std::string("xenia_") + c.name;
    if (failure.empty()) {
      out << backend.emit_function(block, name) << '\n';
      table += "  {&" + name + ", nullptr},\n";
    } else {
      table += "  {nullptr, \"" + failure + "\"},\n";
    }
  }
  out << "struct XeniaPpcCompiled {"
         " ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&);"
         " const char* failure; };\n"
      << table << "};\n";
}
