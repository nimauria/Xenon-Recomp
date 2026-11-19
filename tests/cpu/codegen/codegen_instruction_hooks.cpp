// Generates native AOT code for guest functions carrying module instruction
// hooks, for x86_64/instruction_hooks.cpp to run against its hook routines.

#include <array>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

namespace {

// Ordinal n names hook_symbols[n]; the runner defines each routine.
const std::array<std::string, 4> kHookSymbols = {"hook_after", "hook_jump", "hook_observe",
                                                 "hook_return"};

bool emit(std::ofstream& out, GuestAddress base, std::span<const std::uint32_t> words,
          std::span<const InstructionHook> hooks, std::string_view name) {
  StaticFunctionCompiler compiler;
  const FunctionCodeRange range{base, words};
  auto result = compiler.compile_ranges(base, std::span(&range, 1u), {}, hooks);
  if (!result.ok) {
    std::cerr << name << " failed to compile at 0x" << std::hex << result.error_address << ": "
              << result.error << "\n";
    return false;
  }
  backend::CppAotBackend backend;
  out << backend.emit_translation_unit(result.function, name, {}, {}, kHookSymbols) << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;

  // observe: li r3,1; li r5,0x100; addi r4,r3,1; blr. hook_observe runs
  // before the addi (must see r3/r5 just written, and its r3 write must
  // reach the addi); hook_after runs after it (must see the addi's r4).
  const std::array<std::uint32_t, 4> observe = {0x38600001u, 0x38A00100u, 0x38830001u,
                                                0x4E800020u};
  const std::array<InstructionHook, 2> observe_hooks = {
      InstructionHook{.address = 0x5008u, .ordinal = 2u},
      InstructionHook{.address = 0x5008u, .ordinal = 0u, .after_instruction = true}};

  // jump: li r3,0; li r3,1; blr; li r3,2; blr. hook_jump runs before the
  // second li and, when it returns true, jumps to 0x600C - a block nothing
  // else reaches.
  const std::array<std::uint32_t, 5> jump = {0x38600000u, 0x38600001u, 0x4E800020u, 0x38600002u,
                                             0x4E800020u};
  const std::array<InstructionHook, 1> jump_hooks = {
      InstructionHook{.address = 0x6004u, .ordinal = 1u,
                      .on_true = InstructionHookAction::Jump, .jump_on_true = 0x600Cu}};

  // ret: li r3,9; li r3,10; blr. hook_return returns from the function
  // before the second li when it returns true.
  const std::array<std::uint32_t, 3> ret = {0x38600009u, 0x3860000Au, 0x4E800020u};
  const std::array<InstructionHook, 1> ret_hooks = {
      InstructionHook{.address = 0x7004u, .ordinal = 3u,
                      .on_true = InstructionHookAction::Return}};

  if (!emit(out, 0x5000u, observe, observe_hooks, "hooks_observe")) return 4;
  if (!emit(out, 0x6000u, jump, jump_hooks, "hooks_jump")) return 5;
  if (!emit(out, 0x7000u, ret, ret_hooks, "hooks_return")) return 6;
  return 0;
}
