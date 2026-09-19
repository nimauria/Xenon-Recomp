#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include "xenon/cpu/optimizer.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::ir;

namespace {

std::size_t count_op(const Block& block, Op op) {
  return static_cast<std::size_t>(std::count_if(
      block.instructions.begin(), block.instructions.end(),
      [&](const Instruction& i) { return i.op == op; }));
}

void test_algebraic_and_select_simplification() {
  Block block{};
  block.guest_address = 0x1000;
  Builder b(block);
  auto x = b.read_gpr(3);
  auto y = b.read_gpr(4);
  auto zero = b.constant_i64(0);
  const ValueId add_args[] = {x, zero};
  auto add = b.emit(Op::Add, Type::I64, add_args);
  auto yes = b.constant_i1(true);
  const ValueId select_args[] = {yes, add, y};
  auto selected = b.emit(Op::Select, Type::I64, select_args);
  b.write_gpr(5, selected);

  const auto stats = Optimizer{}.run(block);
  assert(stats.algebraic_simplifications >= 1);
  assert(stats.selects_simplified >= 1);
  assert(count_op(block, Op::Add) == 0);
  assert(count_op(block, Op::Select) == 0);
}

void test_local_value_numbering_and_dce() {
  Block block{};
  block.guest_address = 0x2000;
  Builder b(block);
  auto a = b.read_gpr(3);
  auto c = b.read_gpr(4);
  const ValueId args[] = {a, c};
  auto first = b.emit(Op::Add, Type::I64, args);
  auto second = b.emit(Op::Add, Type::I64, args);
  const ValueId dead_args[] = {a, c};
  (void)b.emit(Op::Mul, Type::I64, dead_args); // no architectural user
  b.write_gpr(5, first);
  b.write_gpr(6, second);

  const auto stats = Optimizer{}.run(block);
  assert(stats.common_subexpressions_eliminated >= 1);
  assert(stats.dead_code_eliminated >= 1);
  assert(count_op(block, Op::Add) == 1);
  assert(count_op(block, Op::Mul) == 0);
}

void test_signed_constant_compare_uses_operand_width() {
  Block block{};
  block.guest_address = 0x3000;
  Builder b(block);
  auto minus_one = b.constant_i32(0xFFFFFFFFu);
  auto zero = b.constant_i32(0);
  const ValueId compare_args[] = {minus_one, zero};
  auto less = b.emit(Op::CompareSlt, Type::I1, compare_args);
  const ValueId out[] = {less};
  b.emit(Op::WriteCRBit, Type::Void, out, 0);

  Optimizer{}.run(block);
  auto constant = std::find_if(block.instructions.begin(), block.instructions.end(),
      [](const Instruction& i) { return i.op == Op::Constant && i.type == Type::I1; });
  assert(constant != block.instructions.end());
  assert(constant->imm0 == 1u);
}

void test_extension_round_trip_elimination() {
  Block block{};
  block.guest_address = 0x4000;
  Builder b(block);
  auto cr = b.emit(Op::ReadCR, Type::I32);
  const ValueId a[] = {cr};
  auto wide = b.emit(Op::ZeroExtend, Type::I64, a);
  const ValueId w[] = {wide};
  auto narrow = b.emit(Op::Truncate, Type::I32, w);
  const ValueId n[] = {narrow};
  b.emit(Op::WriteCR, Type::Void, n);

  const auto stats = Optimizer{}.run(block);
  assert(stats.extensions_eliminated >= 1);
  assert(count_op(block, Op::ZeroExtend) == 0);
  assert(count_op(block, Op::Truncate) == 0);
}

void test_report_is_available_without_release_hot_path_logging() {
  Function function{};
  function.guest_address = 0x5000;
  function.blocks.emplace_back();
  function.blocks.back().guest_address = 0x5000;
  Builder b(function.blocks.back());
  auto x = b.read_gpr(3);
  auto zero = b.constant_i64(0);
  const ValueId args[] = {x, zero};
  auto result = b.emit(Op::Add, Type::I64, args);
  b.write_gpr(4, result);

  const auto report = Optimizer{}.run_with_report(function);
  assert(report.ir_before >= report.ir_after);
  assert(report.blocks_before == 1 && report.blocks_after == 1);
  const auto text = report.format();
  assert(text.find("Function 5000") != std::string::npos);
  assert(text.find("IR:") != std::string::npos);
  assert(text.find("state reads eliminated:") != std::string::npos);
}

}  // namespace

int main() {
  test_algebraic_and_select_simplification();
  test_local_value_numbering_and_dce();
  test_signed_constant_compare_uses_operand_width();
  test_extension_round_trip_elimination();
  test_report_is_available_without_release_hot_path_logging();
  std::cout << "xenon_cpu_v2_optimizer: ok\n";
  return 0;
}
