#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/optimizer.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::ir;

namespace {

std::size_t count_op(const Block& block, Op op) {
  return static_cast<std::size_t>(std::count_if(
      block.instructions.begin(), block.instructions.end(),
      [&](const Instruction& i) { return i.op == op; }));
}

auto find_op(Block& block, Op op) {
  return std::find_if(block.instructions.begin(), block.instructions.end(),
                      [&](const Instruction& i) { return i.op == op; });
}

void test_xer_bit_forwarding() {
  Block block{};
  block.guest_address = 0x1000;
  Builder b(block);
  (void)b.emit(Op::ReadXerCA, Type::I1);
  auto one = b.constant_i1(true);
  const ValueId set_args[] = {one};
  b.emit(Op::SetXerCA, Type::Void, set_args);
  (void)b.emit(Op::ReadXerCA, Type::I1);

  const auto stats = Optimizer{}.run(block);
  assert(stats.xer_reads_eliminated >= 1);
  assert(stats.xer_writes_deferred >= 1);
  assert(count_op(block, Op::ReadXerCA) == 1);
  assert(count_op(block, Op::SetXerCA) == 1);
}

void test_whole_xer_observation_materializes() {
  Block block{};
  block.guest_address = 0x2000;
  Builder b(block);
  auto one = b.constant_i1(true);
  const ValueId set_args[] = {one};
  b.emit(Op::SetXerCA, Type::Void, set_args);
  (void)b.emit(Op::ReadXER, Type::I32);

  Optimizer{}.run(block);
  auto set = find_op(block, Op::SetXerCA);
  auto read = find_op(block, Op::ReadXER);
  assert(set != block.instructions.end() && read != block.instructions.end());
  assert(set < read);
}

void test_sticky_overflow_values_remain_typed() {
  Block block{};
  block.guest_address = 0x3000;
  Builder b(block);
  auto old_so = b.emit(Op::ReadXerSO, Type::I1);
  auto ov = b.constant_i1(true);
  const ValueId sticky_args[] = {old_so, ov};
  auto new_so = b.emit(Op::Or, Type::I1, sticky_args);
  const ValueId ov_args[] = {ov};
  const ValueId so_args[] = {new_so};
  b.emit(Op::SetXerOV, Type::Void, ov_args);
  b.emit(Op::SetXerSO, Type::Void, so_args);
  (void)b.emit(Op::ReadXerSO, Type::I1);
  (void)b.emit(Op::ReadXER, Type::I32);

  const auto stats = Optimizer{}.run(block);
  assert(stats.xer_reads_eliminated >= 1);
  assert(count_op(block, Op::SetXerOV) == 1);
  assert(count_op(block, Op::SetXerSO) == 1);
  auto ov_write = find_op(block, Op::SetXerOV);
  auto so_write = find_op(block, Op::SetXerSO);
  auto full_read = find_op(block, Op::ReadXER);
  assert(ov_write < full_read && so_write < full_read);
}

void test_compare_to_branch_forwarding() {
  Block block{};
  block.guest_address = 0x4000;
  Builder b(block);
  auto lt = b.constant_i1(false);
  auto gt = b.constant_i1(false);
  auto eq = b.constant_i1(true);
  auto so = b.constant_i1(false);
  const ValueId fields[] = {lt, gt, eq, so};
  b.emit(Op::WriteCRCompare, Type::Void, fields, 0);
  auto condition = b.emit(Op::ReadCRBit, Type::I1, {}, 2); // CR0.EQ
  auto target = b.constant_i64(0x5000);
  const ValueId branch_args[] = {condition, target};
  b.emit(Op::BranchIf, Type::Void, branch_args);

  const auto stats = Optimizer{}.run(block);
  assert(stats.cr_reads_eliminated == 1);
  assert(stats.cr_writes_deferred == 1);
  assert(count_op(block, Op::ReadCRBit) == 0);
  assert(count_op(block, Op::WriteCRCompare) == 1);
  auto write = find_op(block, Op::WriteCRCompare);
  auto branch = find_op(block, Op::BranchIf);
  assert(write != block.instructions.end() && branch != block.instructions.end());
  assert(write < branch);
  assert(branch->args[0] == eq);

  Function function{};
  function.guest_address = block.guest_address;
  function.blocks.push_back(block);
  const auto source = backend::CppAotBackend{}.emit_function(function, "cr_forwarding");
  assert(source.find("state.cr_bit(") == std::string::npos);
}

void test_whole_cr_observation_materializes() {
  Block block{};
  block.guest_address = 0x5000;
  Builder b(block);
  auto no = b.constant_i1(false);
  auto yes = b.constant_i1(true);
  const ValueId fields[] = {no, no, yes, no};
  b.emit(Op::WriteCRCompare, Type::Void, fields, 3);
  (void)b.emit(Op::ReadCR, Type::I32);

  Optimizer{}.run(block);
  auto write = find_op(block, Op::WriteCRCompare);
  auto read = find_op(block, Op::ReadCR);
  assert(write != block.instructions.end() && read != block.instructions.end());
  assert(write < read);
}

void test_mcrxr_invalidates_promoted_xer_bits() {
  Block block{};
  block.guest_address = 0x6000;
  Builder b(block);
  auto yes = b.constant_i1(true);
  const ValueId a[] = {yes};
  b.emit(Op::SetXerCA, Type::Void, a);
  b.emit(Op::SetXerOV, Type::Void, a);
  b.emit(Op::SetXerSO, Type::Void, a);
  b.emit(Op::MoveXERToCR, Type::Void, {}, 2);
  (void)b.emit(Op::ReadXerCA, Type::I1);

  Optimizer{}.run(block);
  auto move = find_op(block, Op::MoveXERToCR);
  auto read = find_op(block, Op::ReadXerCA);
  assert(move != block.instructions.end() && read != block.instructions.end());
  assert(count_op(block, Op::SetXerCA) == 1);
  assert(count_op(block, Op::SetXerOV) == 1);
  assert(count_op(block, Op::SetXerSO) == 1);
  assert(move < read); // mcrxr clears SO/OV/CA, so the post-op read cannot alias.
}

}  // namespace

int main() {
  test_xer_bit_forwarding();
  test_whole_xer_observation_materializes();
  test_sticky_overflow_values_remain_typed();
  test_compare_to_branch_forwarding();
  test_whole_cr_observation_materializes();
  test_mcrxr_invalidates_promoted_xer_bits();
  std::cout << "xenon_cpu_v2_flags: ok\n";
  return 0;
}
