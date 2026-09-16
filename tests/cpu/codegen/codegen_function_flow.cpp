#include <array>
#include <fstream>
#include <iostream>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

int main(int argc, char** argv) {
  if (argc != 2) return 2;

  // r3 = 0; r4 = 3; do { ++r3; } while (r3 != r4); blr
  // This deliberately contains a backwards local branch so the generated host
  // function proves local PPC control flow stays inside native code.
  constexpr GuestAddress base = 0x2000u;
  const std::array<std::uint32_t, 6> words = {
      0x38600000u,                                      // addi r3,r0,0
      0x38800003u,                                      // addi r4,r0,3
      0x38630001u,                                      // addi r3,r3,1
      0x7C032000u,                                      // cmpw cr0,r3,r4
      0x40000000u | (4u << 21) | (2u << 16) | 0xFFF8u, // bne 0x2008
      0x4E800020u,                                      // blr
  };

  StaticFunctionCompiler compiler;
  auto result = compiler.compile(base, words);
  if (!result.ok) {
    std::cerr << "function compile failed at 0x" << std::hex << result.error_address
              << ": " << result.error << "\n";
    return 3;
  }

  backend::CppAotBackend backend;
  std::ofstream out(argv[1]);
  if (!out) return 4;
  out << backend.emit_translation_unit(result.function, "flow_loop");
  return 0;
}
