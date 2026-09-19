#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/cpu/function_compiler.hpp"
#include "xenon/cpu/ir.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::ir;

namespace {

std::size_t count_op(const Function& function, Op op) {
  std::size_t count = 0;
  for (const auto& block : function.blocks)
    count += static_cast<std::size_t>(std::count_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const Instruction& instruction) { return instruction.op == op; }));
  return count;
}

FunctionCompileResult compile_one(std::uint32_t word) {
  const std::vector<std::uint32_t> words{word};
  return StaticFunctionCompiler{}.compile(0x82000000u, words);
}

void test_d_form_uses_immediate_address_node() {
  // lwz r3, 8(r4)
  const auto result = compile_one((32u << 26) | (3u << 21) | (4u << 16) | 8u);
  assert(result.ok);
  assert(count_op(result.function, Op::AddImmediate) == 1);
  assert(count_op(result.function, Op::Truncate) == 0);
}

void test_zero_base_d_form_folds_to_static_guest_address() {
  // lwz r3, -4(0): RA=0 means a literal/sign-extended displacement base.
  const auto result = compile_one((32u << 26) | (3u << 21) | 0xFFFCu);
  assert(result.ok);
  assert(count_op(result.function, Op::AddImmediate) == 0);
  assert(count_op(result.function, Op::Add) == 0);
  assert(count_op(result.function, Op::Truncate) == 0);

  bool saw_address = false;
  for (const auto& instruction : result.function.blocks.front().instructions) {
    if (instruction.op == Op::Constant && instruction.type == Type::I64 &&
        instruction.imm0 == 0xFFFFFFFFFFFFFFFCull) {
      saw_address = true;
    }
  }
  assert(saw_address);
}

void test_zero_base_indexed_form_reuses_rb() {
  // lwzx r3, 0, r4. Primary 31 / XO 23.
  const auto result = compile_one(0x7C00002Eu | (3u << 21) | (4u << 11));
  assert(result.ok);
  assert(count_op(result.function, Op::AddImmediate) == 0);
  assert(count_op(result.function, Op::Add) == 0);
  assert(count_op(result.function, Op::Truncate) == 0);
  assert(count_op(result.function, Op::ReadGpr) == 1);
}

void test_update_form_keeps_full_ea() {
  // lwzu r3, 8(r4) must use the same 64-bit EA for the memory access and RA update.
  const auto result = compile_one((33u << 26) | (3u << 21) | (4u << 16) | 8u);
  assert(result.ok);
  assert(count_op(result.function, Op::AddImmediate) == 1);
  assert(count_op(result.function, Op::Truncate) == 0);
}

}  // namespace

int main() {
  test_d_form_uses_immediate_address_node();
  test_zero_base_d_form_folds_to_static_guest_address();
  test_zero_base_indexed_form_reuses_rb();
  test_update_form_keeps_full_ea();
  std::cout << "xenon_cpu_v2_effective_address: ok\n";
  return 0;
}
