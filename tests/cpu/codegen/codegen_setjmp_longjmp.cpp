#include <array>
#include <fstream>
#include <iostream>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

int main(int argc, char** argv) {
  if (argc != 2) return 2;

  // Parent guest function:
  //   mflr r31              ; preserve caller LR like a real non-leaf frame
  //   mr   r30,r3           ; preserve jump-buffer pointer
  //   bl   setjmp_helper
  //   cmpwi r3,0
  //   bne  resumed
  //   mr   r3,r30
  //   bl   nested
  //   b    resumed          ; should only run if nested unexpectedly returns
  // resumed:
  //   mtlr r31
  //   blr
  constexpr GuestAddress kParent = 0x1000u;
  constexpr std::array<std::uint32_t, 10> parent_words = {
      0x7FE802A6u,  // mflr r31
      0x7C7E1B78u,  // mr r30,r3
      0x480001F9u,  // bl 0x1200 (setjmp helper)
      0x2C030000u,  // cmpwi r3,0
      0x40820010u,  // bne 0x1020
      0x7FC3F378u,  // mr r3,r30
      0x480000E9u,  // bl 0x1100 (nested)
      0x48000004u,  // b 0x1020
      0x7FE803A6u,  // mtlr r31
      0x4E800020u,  // blr
  };

  // Nested guest function deliberately mutates nonvolatile/stack state, then
  // invokes longjmp. Correct execution never reaches its blr.
  constexpr GuestAddress kNested = 0x1100u;
  constexpr std::array<std::uint32_t, 6> nested_words = {
      0x3821FFE0u,  // addi r1,r1,-32
      0x39C01234u,  // li r14,0x1234
      0x7FC3F378u,  // mr r3,r30
      0x38800007u,  // li r4,7
      0x480001F1u,  // bl 0x1300 (longjmp helper)
      0x4E800020u,  // blr (must not execute normally)
  };

  StaticFunctionCompiler compiler;
  const auto parent = compiler.compile(kParent, parent_words);
  const auto nested = compiler.compile(kNested, nested_words);
  if (!parent.ok || !nested.ok) {
    const auto& failed = parent.ok ? nested : parent;
    std::cerr << "function compile failed at 0x" << std::hex
              << failed.error_address << ": " << failed.error << "\n";
    return 3;
  }

  backend::CppAotBackend backend;
  std::ofstream out(argv[1]);
  if (!out) return 4;
  out << backend.emit_translation_unit(parent.function, "setjmp_parent");
  out << "\n";
  out << backend.emit_translation_unit(nested.function, "setjmp_nested");
  return 0;
}
