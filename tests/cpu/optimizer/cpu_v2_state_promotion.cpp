#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"
#include "xenon/cpu/optimizer.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::ir;

namespace {

std::size_t count_op(const Block& block, Op op) {
  return static_cast<std::size_t>(std::count_if(block.instructions.begin(),
      block.instructions.end(), [&](const Instruction& i){ return i.op == op; }));
}

void test_long_gpr_chain() {
  constexpr GuestAddress base = 0x10000;
  // addi r3,r3,1 repeated. Encoding: opcode 14, RT=3, RA=3, SIMM=1.
  const std::uint32_t addi = (14u<<26) | (3u<<21) | (3u<<16) | 1u;
  std::vector<std::uint32_t> words(100, addi);
  const auto compiled = StaticFunctionCompiler{}.compile(base, words);
  assert(compiled.ok);
  assert(compiled.function.blocks.size() == 1);
  const auto& block = compiled.function.blocks.front();
  assert(count_op(block, Op::ReadGpr) == 1);
  assert(count_op(block, Op::WriteGpr) == 1);

  const auto generated = backend::CppAotBackend{}.emit_function(compiled.function, "promotion_chain");
  std::size_t gpr3 = 0;
  for (std::size_t pos = 0; (pos = generated.find("state.gpr[3]", pos)) != std::string::npos; pos += 12) ++gpr3;
  assert(gpr3 == 2); // one initial load, one architectural materialization.
}

void test_fault_boundary_materializes() {
  Block block{};
  block.guest_address = 0x2000;
  Builder b(block);
  auto r3 = b.read_gpr(3);
  auto one = b.constant_i64(1);
  const ValueId add_args[] = {r3, one};
  auto updated = b.emit(Op::Add, Type::I64, add_args);
  b.write_gpr(3, updated);
  auto address = b.constant_i32(0x1000);
  const ValueId load_args[] = {address};
  (void)b.emit(Op::Load, Type::I32, load_args, static_cast<std::uint64_t>(Endian::Big));

  const auto stats = Optimizer{}.run(block);
  assert(stats.state_writes_deferred >= 1);
  auto write = std::find_if(block.instructions.begin(), block.instructions.end(),
      [](const Instruction& i){ return i.op == Op::WriteGpr && i.imm0 == 3; });
  auto load = std::find_if(block.instructions.begin(), block.instructions.end(),
      [](const Instruction& i){ return i.op == Op::Load; });
  assert(write != block.instructions.end() && load != block.instructions.end());
  assert(write < load); // precise fault state sees the updated GPR.
}

void test_call_invalidates_cached_state() {
  Block block{};
  block.guest_address = 0x3000;
  Builder b(block);
  auto r3 = b.read_gpr(3);
  b.write_gpr(3, r3);
  auto target = b.constant_i64(0x9000);
  const ValueId call_args[] = {target};
  b.emit(Op::Call, Type::Void, call_args);
  (void)b.read_gpr(3);
  Optimizer{}.run(block);
  // Runtime guest calls may change architectural state, so the post-call read
  // must remain rather than aliasing the pre-call value.
  assert(count_op(block, Op::ReadGpr) == 2);
}

void test_fpr_vector_lr_ctr_promotion() {
  Block block{};
  block.guest_address = 0x4000;
  Builder b(block);
  const auto f = b.read_fpr_bits(2); b.write_fpr_bits(2, f); (void)b.read_fpr_bits(2);
  const auto v = b.read_vector(9); b.write_vector(9, v); (void)b.read_vector(9);
  const auto lr = b.read_lr(); b.write_lr(lr); (void)b.read_lr();
  const auto ctr = b.read_ctr(); b.write_ctr(ctr); (void)b.read_ctr();
  const auto stats = Optimizer{}.run(block);
  assert(stats.state_reads_eliminated >= 4);
  assert(stats.state_writes_deferred >= 4);
  assert(count_op(block, Op::ReadFprBits) == 1);
  assert(count_op(block, Op::ReadVector) == 1);
  assert(count_op(block, Op::ReadLR) == 1);
  assert(count_op(block, Op::ReadCTR) == 1);
}

void test_vector_chain() {
  // Repeated updates must keep only the touched vector registers live, even
  // though the architectural VMX128 file contains 128 registers.
  const std::uint32_t vxor = 0x100004C4u | (3u << 21) | (3u << 16) | (4u << 11);
  const std::vector<std::uint32_t> words(100, vxor);
  const auto compiled = StaticFunctionCompiler{}.compile(0x5000u, words);
  assert(compiled.ok && compiled.function.blocks.size() == 1);
  const auto& block = compiled.function.blocks.front();
  assert(count_op(block, Op::ReadVector) == 2);
  assert(count_op(block, Op::WriteVector) == 1);
}

} // namespace

int main() {
  test_long_gpr_chain();
  test_fault_boundary_materializes();
  test_call_invalidates_cached_state();
  test_fpr_vector_lr_ctr_promotion();
  test_vector_chain();
  std::cout << "xenon_cpu_v2_state_promotion: ok\n";
  return 0;
}
