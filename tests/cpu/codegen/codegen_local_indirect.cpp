#include <array>
#include <fstream>
#include <iostream>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

// Generates two functions whose indirect control flow targets their own basic
// blocks, exercising the per-function shared is_local() helper and shared
// local-branch block (rather than a switch over every label at each site).
int main(int argc, char** argv) {
  if (argc != 2) return 2;

  // Function A (jump table shape):
  //   cmpwi r3,0 ; beq 0x2014 ; mtctr r4 ; bctr ; li r5,0x11 ; li r5,0x55 ; blr
  constexpr GuestAddress kBranchBase = 0x2000u;
  const std::array<std::uint32_t, 7> branch_words = {
      0x2C030000u,  // cmpwi cr0,r3,0
      0x41820010u,  // beq 0x2014
      0x7C8903A6u,  // mtctr r4
      0x4E800420u,  // bctr
      0x38A00011u,  // li r5,0x11 (never reached)
      0x38A00055u,  // li r5,0x55  <- local target 0x2014
      0x4E800020u,  // blr
  };

  // Function B (indirect call to a local block):
  //   cmpwi r3,0 ; beq 0x3014 ; mtctr r4 ; bctrl ; blr ; li r5,0x66 ; blr
  constexpr GuestAddress kCallBase = 0x3000u;
  const std::array<std::uint32_t, 7> call_words = {
      0x2C030000u,  // cmpwi cr0,r3,0
      0x41820010u,  // beq 0x3014
      0x7C8903A6u,  // mtctr r4
      0x4E800421u,  // bctrl
      0x4E800020u,  // blr          <- expected return 0x3010
      0x38A00066u,  // li r5,0x66   <- local target 0x3014
      0x4E800020u,  // blr
  };

  StaticFunctionCompiler compiler;
  const auto branch = compiler.compile(kBranchBase, branch_words);
  const auto call = compiler.compile(kCallBase, call_words);
  if (!branch.ok || !call.ok) {
    const auto& failed = branch.ok ? call : branch;
    std::cerr << "function compile failed at 0x" << std::hex << failed.error_address
              << ": " << failed.error << "\n";
    return 3;
  }

  backend::CppAotBackend backend;
  const auto branch_source = backend.emit_translation_unit(branch.function, "local_indirect_branch");
  const auto call_source = backend.emit_function(call.function, "local_indirect_call");
  const auto contains = [](const std::string& text, const char* token) {
    return text.find(token) != std::string::npos;
  };
  // The new shape must actually be exercised - otherwise this test would pass
  // vacuously against the old per-site switches.
  if (!contains(branch_source, "local_indirect_branch_dispatch_v2_is_local(guest_target)") ||
      !contains(branch_source, "goto xenon_local_branch;") ||
      !contains(branch_source, "xenon_local_branch:")) {
    std::cerr << "bctr did not use the shared local-branch dispatch\n";
    return 5;
  }
  if (!contains(call_source, "local_indirect_call_dispatch_v2_is_local(guest_target)")) {
    std::cerr << "bctrl did not use the shared is_local helper\n";
    return 6;
  }

  std::ofstream out(argv[1]);
  if (!out) return 4;
  out << branch_source << "\n" << call_source;
  return 0;
}
