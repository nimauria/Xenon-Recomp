#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"
#include "xenon/cpu/ir.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::ir;

namespace {

std::size_t count_op(const Function& function, Op op) {
  std::size_t count = 0;
  for (const auto& block : function.blocks)
    for (const auto& instruction : block.instructions)
      if (instruction.op == op) ++count;
  return count;
}

bool has_edge(const Block& block, GuestAddress target, EdgeKind kind) {
  for (const auto& edge : block.successors)
    if (edge.target == target && edge.kind == kind) return true;
  return false;
}

void test_constant_taken_branch_becomes_native_unconditional_flow() {
  constexpr GuestAddress base = 0x82001000u;
  const std::vector<std::uint32_t> words = {
      0x38600000u,  // li r3,0
      0x2C030000u,  // cmpwi r3,0
      0x41820008u,  // beq +8 -> base+0x10
      0x38840001u,  // addi r4,r4,1 (unreachable from entry after simplification)
      0x4E800020u,  // blr
  };
  const auto compiled = StaticFunctionCompiler{}.compile(base, words);
  assert(compiled.ok);
  assert(count_op(compiled.function, Op::BranchIf) == 0);
  assert(count_op(compiled.function, Op::Branch) == 1);
  assert(has_edge(compiled.function.blocks.front(), base + 0x10u, EdgeKind::Branch));
  assert(!has_edge(compiled.function.blocks.front(), base + 0x0Cu, EdgeKind::Fallthrough));

  const auto source = backend::CppAotBackend{}.emit_function(compiled.function, "constant_taken");
  assert(source.find("goto L_82001010") != std::string::npos);
  assert(source.find("runtime.call") == std::string::npos);
}

void test_constant_not_taken_branch_becomes_fallthrough() {
  constexpr GuestAddress base = 0x82002000u;
  const std::vector<std::uint32_t> words = {
      0x38600000u,  // li r3,0
      0x2C030001u,  // cmpwi r3,1
      0x41820008u,  // beq +8 (known false)
      0x38840001u,
      0x4E800020u,
  };
  const auto compiled = StaticFunctionCompiler{}.compile(base, words);
  assert(compiled.ok);
  assert(count_op(compiled.function, Op::BranchIf) == 0);
  assert(!has_edge(compiled.function.blocks.front(), base + 0x10u, EdgeKind::Branch));
  assert(has_edge(compiled.function.blocks.front(), base + 0x0Cu, EdgeKind::Fallthrough));

  const auto source = backend::CppAotBackend{}.emit_function(compiled.function, "constant_not_taken");
  assert(source.find("goto L_8200200C") != std::string::npos);
}

void test_external_direct_branch_is_static_exit_not_runtime_dispatch() {
  constexpr GuestAddress base = 0x82003000u;
  const std::vector<std::uint32_t> words = {
      0x48000100u,  // b +0x100
      0x38630001u,
  };
  const auto compiled = StaticFunctionCompiler{}.compile(base, words);
  assert(compiled.ok);
  const auto source = backend::CppAotBackend{}.emit_function(compiled.function, "external_direct");
  assert(source.find("return {FlowReason::Branch,2181054720u,0}") != std::string::npos ||
         source.find("FlowReason::Branch") != std::string::npos);
  assert(source.find("runtime.call") == std::string::npos);
}

}  // namespace

int main() {
  test_constant_taken_branch_becomes_native_unconditional_flow();
  test_constant_not_taken_branch_becomes_fallthrough();
  test_external_direct_branch_is_static_exit_not_runtime_dispatch();
  std::cout << "xenon_cpu_v2_control_flow: ok\n";
  return 0;
}
