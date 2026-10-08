// Common-subexpression and dead-code elimination.

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

#include "cpu/optimizer/passes/optimizer_passes.hpp"

namespace xenon::cpu::ir::detail {
namespace {

bool commutative(Op op) noexcept {
  switch (op) {
    case Op::Add:
    case Op::Mul:
    case Op::And:
    case Op::Or:
    case Op::Xor:
    case Op::CompareEq:
    case Op::CompareNe:
      return true;
    default:
      return false;
  }
}

struct ExpressionKey {
  Op op{};
  Type type{};
  std::array<ValueId, OperandList::kCapacity> args{};
  std::uint8_t argc{};
  std::uint64_t imm0{};
  std::uint64_t imm1{};

  bool operator<(const ExpressionKey& other) const noexcept {
    return std::tie(op, type, argc, args, imm0, imm1) <
           std::tie(other.op, other.type, other.argc, other.args, other.imm0,
                    other.imm1);
  }
};

}  // namespace

OptimizationStats eliminate_common_subexpressions(Block& block) {
  OptimizationStats stats{};
  if (block.instructions.empty()) return stats;
  const auto maximum = max_value_id(block);
  std::vector<ValueId> aliases(maximum + 1u);
  for (std::size_t i = 0; i < aliases.size(); ++i)
    aliases[i] = static_cast<ValueId>(i);

  auto resolve = [&](ValueId value) {
    if (value == kNoValue || value >= aliases.size()) return value;
    ValueId current = value;
    while (current < aliases.size() && aliases[current] != current)
      current = aliases[current];
    return current;
  };

  std::map<ExpressionKey, ValueId> expressions;
  std::vector<Instruction> output;
  output.reserve(block.instructions.size());

  for (auto instruction : block.instructions) {
    for (std::size_t n = 0; n < instruction.args.size(); ++n)
      instruction.args.set(n, resolve(instruction.args[n]));

    if (instruction.result != kNoValue && effects(instruction.op) == Effect::None) {
      ExpressionKey key{};
      key.op = instruction.op;
      key.type = instruction.type;
      key.argc = static_cast<std::uint8_t>(instruction.args.size());
      key.imm0 = instruction.imm0;
      key.imm1 = instruction.imm1;
      for (std::size_t n = 0; n < instruction.args.size(); ++n)
        key.args[n] = instruction.args[n];
      if (commutative(instruction.op) && key.argc == 2 && key.args[1] < key.args[0])
        std::swap(key.args[0], key.args[1]);

      const auto found = expressions.find(key);
      if (found != expressions.end()) {
        aliases[instruction.result] = resolve(found->second);
        ++stats.common_subexpressions_eliminated;
        ++stats.instructions_removed;
        continue;
      }
      expressions.emplace(key, instruction.result);
    }
    output.push_back(std::move(instruction));
  }

  for (auto& instruction : output)
    for (std::size_t n = 0; n < instruction.args.size(); ++n)
      instruction.args.set(n, resolve(instruction.args[n]));
  block.instructions = std::move(output);
  return stats;
}

OptimizationStats eliminate_dead_code(Block& block) {
  OptimizationStats stats{};
  if (block.instructions.empty()) return stats;
  const auto maximum = max_value_id(block);
  std::vector<std::size_t> uses(maximum + 1u, 0);
  for (const auto& instruction : block.instructions)
    for (const auto arg : instruction.args)
      if (arg != kNoValue && arg < uses.size()) ++uses[arg];

  std::vector<bool> keep(block.instructions.size(), true);
  for (std::size_t index = block.instructions.size(); index-- > 0;) {
    const auto& instruction = block.instructions[index];
    if (instruction.result == kNoValue || instruction.result >= uses.size() ||
        uses[instruction.result] != 0 || effects(instruction.op) != Effect::None)
      continue;
    keep[index] = false;
    ++stats.dead_code_eliminated;
    ++stats.instructions_removed;
    for (const auto arg : instruction.args)
      if (arg != kNoValue && arg < uses.size() && uses[arg]) --uses[arg];
  }

  std::vector<Instruction> output;
  output.reserve(block.instructions.size() - stats.dead_code_eliminated);
  for (std::size_t index = 0; index < block.instructions.size(); ++index)
    if (keep[index]) output.push_back(std::move(block.instructions[index]));
  block.instructions = std::move(output);
  return stats;
}

}  // namespace xenon::cpu::ir::detail
