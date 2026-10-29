// Generates native AOT code for guest loops, for x86_64/safepoint.cpp to run
// with the thread's safepoint flag set and clear.

#include <array>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

namespace {

bool emit(std::ofstream& out, GuestAddress base, std::span<const std::uint32_t> words,
          std::string_view name) {
  StaticFunctionCompiler compiler;
  auto result = compiler.compile(base, words);
  if (!result.ok) {
    std::cerr << name << " failed to compile at 0x" << std::hex << result.error_address << ": "
              << result.error << "\n";
    return false;
  }
  backend::CppAotBackend backend;
  out << backend.emit_translation_unit(result.function, name) << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;

  // spin: b spin - a guest thread that never leaves one compiled function.
  const std::array<std::uint32_t, 1> spin = {0x48000000u};
  // count: r3 = 0; r4 = 3; do { ++r3; } while (r3 != r4); blr
  const std::array<std::uint32_t, 6> count = {
      0x38600000u, 0x38800003u, 0x38630001u, 0x7C032000u,
      0x40000000u | (4u << 21) | (2u << 16) | 0xFFF8u,  // bne 0x4008
      0x4E800020u,
  };
  if (!emit(out, 0x3000u, spin, "safepoint_spin")) return 4;
  if (!emit(out, 0x4000u, count, "safepoint_count")) return 5;
  return 0;
}
