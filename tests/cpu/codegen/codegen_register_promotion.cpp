// Generates each scenario twice - guest registers in CpuState and promoted to
// C++ locals - for x86_64/register_promotion.cpp, which runs both from the
// same state and memory and requires identical results.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

namespace {

struct Scenario {
  const char* name;
  GuestAddress base;
  std::vector<std::uint32_t> words;
};

const std::vector<Scenario> kScenarios = {
    // A loop: r3 += 2 a hundred times, counted down in r4 by addic., then
    // stored - a back-edge (safepoint poll) and a store inside the function.
    {"loop", 0x10000u,
     {0x38600000u, 0x38800064u, 0x38630002u, 0x3484FFFFu, 0x4082FFF8u, 0x90650000u, 0x4E800020u}},
    // FP: f2 = f1 + f1 from memory, stored back.
    {"fp", 0x30000u, {0xC8250000u, 0xFC41082Au, 0xD8450008u, 0x4E800020u}},
    // Vector: v2 = v1 + v1 (vaddfp) from memory, stored.
    {"vector", 0x40000u, {0x7C2028CEu, 0x1041080Au, 0x7C4031CEu, 0x4E800020u}},
    // lswi r3,r8,8 writes r3 and r4 through CpuState; the add reads them.
    {"string_load", 0x50000u, {0x7C6844AAu, 0x7CC32214u, 0x4E800020u}},
    // twi eq,r3,77 right after li r3,77: the trap handler must see r3 = 77.
    {"trap", 0x60000u, {0x3860004Du, 0x0C83004Du, 0x4E800020u}},
};

// A caller and callee: the callee changes r3 and r4, the caller uses both
// after the call (its locals must be reloaded).
constexpr GuestAddress kCaller = 0x20000u;
constexpr GuestAddress kCallee = 0x21000u;
const std::vector<std::uint32_t> kCallerWords = {
    0x7D8802A6u,  // mflr r12
    0x38600005u,  // li r3,5
    0x48000FF9u,  // bl 0x21000
    0x38630001u,  // addi r3,r3,1
    0x7CE32214u,  // add r7,r3,r4
    0x7D8803A6u,  // mtlr r12
    0x4E800020u,  // blr
};
const std::vector<std::uint32_t> kCalleeWords = {
    0x1C630007u,  // mulli r3,r3,7
    0x38800009u,  // li r4,9
    0x4E800020u,  // blr
};

bool compile(GuestAddress base, const std::vector<std::uint32_t>& words, ir::Function& out) {
  StaticFunctionCompiler compiler;
  const FunctionCodeRange range{base, words};
  auto result = compiler.compile_ranges(base, std::span(&range, 1u), {}, {});
  if (!result.ok) {
    std::cerr << "compile failed at " << base << ": " << result.error << "\n";
    return false;
  }
  out = std::move(result.function);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  bool promoted_marks = true;
  bool plain_marks = false;
  for (const bool promote : {false, true}) {
    const backend::CppAotBackend backend(promote);
    const std::string suffix = promote ? "_promoted" : "_plain";
    for (const auto& scenario : kScenarios) {
      ir::Function function;
      if (!compile(scenario.base, scenario.words, function)) return 4;
      const auto source = backend.emit_function(function, std::string(scenario.name) + suffix);
      const bool marked = source.find("xenon_flush") != std::string::npos;
      (promote ? promoted_marks : plain_marks) = promote ? (promoted_marks && marked) : (plain_marks || marked);
      out << source << "\n";
    }
    ir::Function callee, caller;
    if (!compile(kCallee, kCalleeWords, callee) || !compile(kCaller, kCallerWords, caller)) return 5;
    out << backend.emit_function(callee, "callee" + suffix) << "\n";
    const backend::DirectCallBinding binding{kCallee, "callee" + suffix + "_v2"};
    out << backend.emit_function(caller, "caller" + suffix, std::span(&binding, 1u)) << "\n";
  }
  out << "extern const bool kPromotedFunctionsPromote = " << (promoted_marks ? "true" : "false") << ";\n";
  out << "extern const bool kPlainFunctionsPromote = " << (plain_marks ? "true" : "false") << ";\n";
  return 0;
}
