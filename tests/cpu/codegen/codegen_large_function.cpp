// Generates a very large guest function and a small one for
// x86_64/large_function.cpp. Past CppAotBackend's size limit a function is
// compiled with MSVC's global optimizations off around it alone (whole AC6
// shards took over 15 minutes each at /O2 because of one such function, and
// 4 s without global optimization); it must behave exactly the same. The
// generator also records which functions carry the pragma.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

namespace {

// `units` repetitions of: addi r3,r3,1; cmpwi r3,-1; beq +8; addi r4,r4,1
// (the branch is never taken but splits every unit into its own blocks),
// then blr. r3 and r4 both end at r3/r4 + units.
std::vector<std::uint32_t> counting_function(std::uint32_t units) {
  std::vector<std::uint32_t> words;
  for (std::uint32_t i = 0; i < units; ++i) {
    words.push_back(0x38630001u);  // addi r3,r3,1
    words.push_back(0x2C03FFFFu);  // cmpwi cr0,r3,-1
    words.push_back(0x41820008u);  // beq cr0,+8
    words.push_back(0x38840001u);  // addi r4,r4,1
  }
  words.push_back(0x4E800020u);  // blr
  return words;
}

bool emit(std::ofstream& out, GuestAddress base, const std::vector<std::uint32_t>& words,
          std::string_view name, bool& reduced) {
  StaticFunctionCompiler compiler;
  const FunctionCodeRange range{base, words};
  auto result = compiler.compile_ranges(base, std::span(&range, 1u), {}, {});
  if (!result.ok) {
    std::cerr << name << " failed to compile: " << result.error << "\n";
    return false;
  }
  backend::CppAotBackend backend;
  const auto source = backend.emit_function(result.function, name);
  reduced = source.find("#pragma optimize(\"g\", off)") != std::string::npos;
  out << source << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  bool large_reduced = false;
  bool small_reduced = true;
  if (!emit(out, 0x10000u, counting_function(3000u), "large_counting", large_reduced)) return 4;
  if (!emit(out, 0x40000u, counting_function(8u), "small_counting", small_reduced)) return 5;
  // After the functions: a reduced function must restore the command-line
  // optimization for whatever follows it.
  out << "extern const bool kLargeCountingReduced = " << (large_reduced ? "true" : "false") << ";\n";
  out << "extern const bool kSmallCountingReduced = " << (small_reduced ? "true" : "false") << ";\n";
  return 0;
}
