#include <cassert>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include <stdexcept>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/ir_verifier.hpp"
#include "xenon/cpu/optimizer.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::ir;

namespace {

OpcodeId valid_opcode() { return Decoder{}.decode(0, 0x60000000u).opcode_id(); }

Instruction constant(ValueId id, Type type, std::uint64_t value) {
  Instruction i{};
  i.op = Op::Constant;
  i.type = type;
  i.result = id;
  i.imm0 = value;
  return i;
}

void test_compact_operands() {
  static_assert(OperandList::kCapacity == 4);
  // CPU V1 Phase-1 measurement on this host was 88 bytes. Phase 5 keeps the
  // common IR node within one 64-byte cache line on the measured x86-64 ABI.
  assert(sizeof(Instruction) <= 64);
  OperandList operands;
  operands = {1, 2, 3, 4};
  assert(operands.size() == 4 && operands[3] == 4);
  bool overflow = false;
  try { operands.assign({1, 2, 3, 4, 5}); }
  catch (const std::length_error&) { overflow = true; }
  assert(overflow);
}

void test_effect_model() {
  assert(effects(Op::Add) == Effect::None);
  assert(has_effect(effects(Op::ReadGpr), Effect::StateRead));
  assert(has_effect(effects(Op::WriteGpr), Effect::StateWrite));
  assert(has_effect(effects(Op::Load), Effect::MemoryRead));
  assert(has_effect(effects(Op::Load), Effect::MayFault));
  assert(has_effect(effects(Op::Store), Effect::MemoryWrite));
  assert(has_effect(effects(Op::ReserveLoad), Effect::Synchronization));
  assert(has_effect(effects(Op::Barrier), Effect::Barrier));
  assert(has_effect(effects(Op::Call), Effect::Call));
  assert(has_effect(effects(Op::Call), Effect::ControlFlow));
  assert(has_effect(effects(Op::Trap), Effect::Trap));
  assert(has_effect(effects(Op::Syscall), Effect::ControlFlow));
}

void test_use_before_definition() {
  Block block{};
  block.guest_address = 0x1000;
  Instruction add{};
  add.op = Op::Add;
  add.type = Type::I64;
  add.result = 0;
  add.args = {1, 1};
  block.instructions.push_back(add);
  auto result = Verifier{}.verify(block);
  assert(!result.ok && result.error == VerifyError::UseBeforeDefinition);
}

void test_memory_metadata() {
  Block block{};
  block.guest_address = 0x2000;
  block.instructions.push_back(constant(0, Type::I32, 0x100));
  Instruction load{};
  load.op = Op::Load;
  load.type = Type::I32;
  load.result = 1;
  load.args = {0};
  load.imm0 = 99; // not a valid Endian
  block.instructions.push_back(load);
  auto result = Verifier{}.verify(block);
  assert(!result.ok && result.error == VerifyError::InvalidMemoryMetadata);
}

void test_terminator_ordering() {
  Block block{};
  block.guest_address = 0x3000;
  block.instructions.push_back(constant(0, Type::I64, 0x4000));
  Instruction branch{};
  branch.op = Op::Branch;
  branch.args = {0};
  block.instructions.push_back(branch);
  block.instructions.push_back(constant(1, Type::I64, 1));
  auto result = Verifier{}.verify(block);
  assert(!result.ok && result.error == VerifyError::InvalidSideEffectOrdering);
}

void test_cfg_validation() {
  Function function{};
  function.guest_address = 0x5000;
  Block block{};
  block.guest_address = 0x5000;
  block.end_address = 0x5004;
  block.successors.push_back({0x6000, EdgeKind::Branch, true});
  Instruction i = constant(0, Type::I64, 1);
  i.guest_address = 0x5000;
  i.guest_word = 0x60000000u;
  i.guest_opcode = valid_opcode();
  block.instructions.push_back(std::move(i));
  function.blocks.push_back(std::move(block));
  auto result = Verifier{}.verify(function);
  assert(!result.ok && result.error == VerifyError::InvalidLocalEdge);
}

void test_guest_source_validation() {
  Function function{};
  function.guest_address = 0x7000;
  Block block{};
  block.guest_address = 0x7000;
  block.end_address = 0x7004;
  block.has_external_exit = true;
  block.instructions.push_back(constant(0, Type::I64, 1));
  function.blocks.push_back(std::move(block));
  auto result = Verifier{}.verify(function);
  assert(!result.ok && result.error == VerifyError::InvalidGuestSource);
}

void test_cross_guest_optimization_support() {
  Block block{};
  block.guest_address = 0x8000;
  block.end_address = 0x8008;
  Builder b(block);
  DecodedInstruction first{};
  first.address = 0x8000;
  first.word = 0x60000000u;
  DecodedInstruction second{};
  second.address = 0x8004;
  second.word = 0x60000000u;
  // Builder source metadata normally also carries OpcodeInfo. For this
  // optimizer-only test we set the source fields directly after construction.
  const auto a = b.constant_i64(40);
  const auto c = b.constant_i64(2);
  const ValueId args[] = {a, c};
  auto sum = b.emit(Op::Add, Type::I64, args);
  b.write_gpr(3, sum); // keep the folded value architecturally observable.
  for (std::size_t n = 0; n < block.instructions.size(); ++n) {
    block.instructions[n].guest_address = n < 2 ? 0x8000 : 0x8004;
    block.instructions[n].guest_opcode = valid_opcode();
  }
  const auto stats = Optimizer{}.run(block);
  assert(stats.constants_folded >= 1);
  const auto folded = std::find_if(block.instructions.begin(), block.instructions.end(),
      [](const Instruction& i) { return i.op == Op::Constant && i.type == Type::I64 && i.imm0 == 42; });
  assert(folded != block.instructions.end());
}

}  // namespace

int main() {
  test_compact_operands();
  test_effect_model();
  test_use_before_definition();
  test_memory_metadata();
  test_terminator_ordering();
  test_cfg_validation();
  test_guest_source_validation();
  test_cross_guest_optimization_support();
  std::cout << "xenon_cpu_v2_ir: ok\n";
  return 0;
}
