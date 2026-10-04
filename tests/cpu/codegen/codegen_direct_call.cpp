#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

int main(int argc, char** argv) {
  if (argc != 2) return 2;

  constexpr GuestAddress kCaller = 0x8000u;
  constexpr GuestAddress kCallee = 0x9000u;
  const std::vector<std::uint32_t> callee_words = {
      0x38630001u,  // addi r3,r3,1
      0x4E800020u,  // blr
  };
  const std::vector<std::uint32_t> caller_words = {
      0x48001001u,  // bl +0x1000 -> 0x9000
      0x38840001u,  // addi r4,r4,1
      0x4E800020u,  // blr
  };

  StaticFunctionCompiler compiler;
  const auto callee = compiler.compile(kCallee, callee_words);
  const auto caller = compiler.compile(kCaller, caller_words);
  if (!callee.ok || !caller.ok) {
    std::cerr << "direct call fixture compile failed\n";
    return 3;
  }

  backend::CppAotBackend backend;
  const backend::DirectCallBinding binding{kCallee, "direct_callee_v2"};

  // static_call_targets() reports exactly the external constant call target
  // (what the recomp driver binds), and nothing for a leaf function.
  const auto caller_targets = backend::CppAotBackend::static_call_targets(caller.function);
  if (caller_targets.size() != 1u || caller_targets[0] != kCallee) {
    std::cerr << "static_call_targets did not report the bl target\n";
    return 5;
  }
  if (!backend::CppAotBackend::static_call_targets(callee.function).empty()) {
    std::cerr << "static_call_targets reported targets for a leaf function\n";
    return 6;
  }
  // A bound call is a plain native call: no registry lookup for that target.
  const auto caller_source = backend.emit_function(
      caller.function, "direct_caller", std::span<const backend::DirectCallBinding>(&binding, 1));
  if (caller_source.find("direct_callee_v2(context)") == std::string::npos ||
      caller_source.find("lookup_compiled(" + std::to_string(kCallee) + "u") != std::string::npos) {
    std::cerr << "bound call was not emitted as a direct call\n";
    return 7;
  }

  std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
  if (!out) return 4;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\n\n";
  out << "using namespace xenon::cpu;\n";
  out << backend.emit_function(callee.function, "direct_callee");
  out << caller_source;
  return 0;
}
