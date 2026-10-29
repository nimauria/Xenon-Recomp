#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

// A callee whose epilogue is a tail branch into another function, the shape of
// every compiler epilogue that ends `b __restgprlr_N`. The caller must resume
// natively when the tail target returns to it.
int main(int argc, char** argv) {
  if (argc != 2) return 2;

  constexpr GuestAddress kCaller = 0x8000u;
  constexpr GuestAddress kTailer = 0x9000u;
  constexpr GuestAddress kTarget = 0xA000u;
  const std::vector<std::uint32_t> caller_words = {
      0x48001001u,  // bl +0x1000 -> 0x9000
      0x38840001u,  // addi r4,r4,1
      0x4E800020u,  // blr
  };
  const std::vector<std::uint32_t> tailer_words = {
      0x38A50001u,  // addi r5,r5,1
      0x48000FFCu,  // b +0xFFC (from 0x9004) -> 0xA000
  };
  const std::vector<std::uint32_t> target_words = {
      0x38630001u,  // addi r3,r3,1
      0x4E800020u,  // blr
  };

  StaticFunctionCompiler compiler;
  const auto caller = compiler.compile(kCaller, caller_words);
  const auto tailer = compiler.compile(kTailer, tailer_words);
  const auto target = compiler.compile(kTarget, target_words);
  if (!caller.ok || !tailer.ok || !target.ok) {
    std::cerr << "tail branch fixture compile failed\n";
    return 3;
  }

  backend::CppAotBackend backend;
  const backend::DirectCallBinding binding{kTailer, "tail_tailer_v2"};
  const auto caller_source = backend.emit_function(
      caller.function, "tail_caller", std::span<const backend::DirectCallBinding>(&binding, 1));
  if (caller_source.find("follow_tail_branches(rr)") == std::string::npos) {
    std::cerr << "call site does not follow a callee's tail branch\n";
    return 5;
  }

  std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
  if (!out) return 4;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\n\n";
  out << "using namespace xenon::cpu;\n";
  out << backend.emit_function(target.function, "tail_target");
  out << backend.emit_function(tailer.function, "tail_tailer");
  out << caller_source;
  return 0;
}
