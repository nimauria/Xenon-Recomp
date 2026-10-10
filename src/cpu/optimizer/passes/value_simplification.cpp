// Constant folding and algebraic/copy simplification of block IR.

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

struct ConstantValue {
  Type type{};
  std::uint64_t bits{};
};

unsigned integer_width(Type type) noexcept {
  switch (type) {
    case Type::I1: return 1;
    case Type::I8: return 8;
    case Type::I16: return 16;
    case Type::I32: return 32;
    case Type::I64: return 64;
    default: return 0;
  }
}

std::uint64_t width_mask(unsigned width) noexcept {
  return width >= 64 ? ~0ull : ((1ull << width) - 1ull);
}

std::uint64_t normalize(std::uint64_t value, Type type) noexcept {
  const auto width = integer_width(type);
  return width ? (value & width_mask(width)) : value;
}

bool signed_less(std::uint64_t a, std::uint64_t b, unsigned width) noexcept {
  const auto mask = width_mask(width);
  const auto sign = 1ull << (width - 1u);
  return ((a & mask) ^ sign) < ((b & mask) ^ sign);
}

std::optional<std::uint64_t> eval_binary(Op op, std::uint64_t a,
                                         std::uint64_t b, Type operand_type,
                                         Type result_type) {
  const unsigned width = integer_width(operand_type);
  if (!width) return std::nullopt;
  const auto mask = width_mask(width);
  a &= mask;
  b &= mask;

  switch (op) {
    case Op::Add: return (a + b) & width_mask(integer_width(result_type));
    case Op::Sub: return (a - b) & width_mask(integer_width(result_type));
    case Op::Mul: return (a * b) & width_mask(integer_width(result_type));
    case Op::And: return (a & b) & width_mask(integer_width(result_type));
    case Op::Or: return (a | b) & width_mask(integer_width(result_type));
    case Op::Xor: return (a ^ b) & width_mask(integer_width(result_type));
    case Op::Shl: {
      const auto out_width = integer_width(result_type);
      return b >= out_width ? 0ull : ((a << b) & width_mask(out_width));
    }
    case Op::ShrLogical:
      return b >= width ? 0ull : (a >> b);
    case Op::ShrArithmetic: {
      if (b == 0) return a;
      const bool negative = (a & (1ull << (width - 1u))) != 0;
      if (b >= width) return negative ? mask : 0ull;
      auto result = a >> b;
      if (negative) {
        const auto high = width == 64
                              ? (~0ull << (64u - static_cast<unsigned>(b)))
                              : (mask & (~0ull << (width - static_cast<unsigned>(b))));
        result |= high;
      }
      return result & mask;
    }
    case Op::Rotl: {
      const unsigned amount = static_cast<unsigned>(b % width);
      if (!amount) return a;
      return ((a << amount) | (a >> (width - amount))) & mask;
    }
    case Op::CompareEq: return a == b;
    case Op::CompareNe: return a != b;
    case Op::CompareUlt: return a < b;
    case Op::CompareUgt: return a > b;
    case Op::CompareSlt: return signed_less(a, b, width);
    case Op::CompareSgt: return signed_less(b, a, width);
    default: return std::nullopt;
  }
}

}  // namespace

OptimizationStats fold_constants(Block& block) {
  OptimizationStats stats{};
  std::map<ValueId, ConstantValue> constants;

  auto make_constant = [&](Instruction& instruction, std::uint64_t value) {
    instruction.op = Op::Constant;
    instruction.args.clear();
    instruction.imm0 = normalize(value, instruction.type);
    instruction.imm1 = 0;
    constants[instruction.result] = {instruction.type, instruction.imm0};
    ++stats.constants_folded;
  };

  for (auto& instruction : block.instructions) {
    if (instruction.op == Op::Constant && instruction.result != kNoValue) {
      constants[instruction.result] = {instruction.type,
                                       normalize(instruction.imm0, instruction.type)};
      continue;
    }
    if (instruction.result == kNoValue) continue;

    if (instruction.args.size() == 1) {
      const auto found = constants.find(instruction.args[0]);
      if (found == constants.end()) continue;
      const auto source = found->second;
      switch (instruction.op) {
        case Op::Not:
          make_constant(instruction,
                        ~source.bits & width_mask(integer_width(instruction.type)));
          break;
        case Op::AddImmediate:
          make_constant(instruction, source.bits + instruction.imm0);
          break;
        case Op::Neg:
          make_constant(instruction, 0ull - source.bits);
          break;
        case Op::ZeroExtend:
        case Op::Truncate:
        case Op::Bitcast:
          if (integer_width(instruction.type)) make_constant(instruction, source.bits);
          break;
        case Op::SignExtend: {
          const auto source_width = integer_width(source.type);
          const auto target_width = integer_width(instruction.type);
          if (!source_width || !target_width) break;
          auto value = source.bits & width_mask(source_width);
          if (value & (1ull << (source_width - 1u))) value |= ~width_mask(source_width);
          make_constant(instruction, value & width_mask(target_width));
          break;
        }
        default:
          break;
      }
      continue;
    }

    if (instruction.op == Op::Select && instruction.args.size() == 3) {
      const auto condition = constants.find(instruction.args[0]);
      if (condition != constants.end()) {
        const auto chosen = condition->second.bits ? instruction.args[1]
                                                   : instruction.args[2];
        const auto chosen_constant = constants.find(chosen);
        if (chosen_constant != constants.end())
          make_constant(instruction, chosen_constant->second.bits);
      }
      continue;
    }

    if (instruction.args.size() != 2) continue;
    const auto lhs = constants.find(instruction.args[0]);
    const auto rhs = constants.find(instruction.args[1]);
    if (lhs == constants.end() || rhs == constants.end()) continue;
    if (lhs->second.type != rhs->second.type) continue;
    if (const auto value = eval_binary(instruction.op, lhs->second.bits,
                                       rhs->second.bits, lhs->second.type,
                                       instruction.type)) {
      make_constant(instruction, *value);
    }
  }
  return stats;
}

OptimizationStats simplify_values(Block& block) {
  OptimizationStats stats{};
  if (block.instructions.empty()) return stats;

  const auto maximum = max_value_id(block);
  std::vector<ValueId> aliases(maximum + 1u);
  std::vector<Type> value_types(maximum + 1u, Type::Void);
  std::vector<std::optional<Instruction>> definitions(maximum + 1u);
  std::map<ValueId, ConstantValue> constants;
  for (std::size_t i = 0; i < aliases.size(); ++i)
    aliases[i] = static_cast<ValueId>(i);

  auto resolve = [&](ValueId value) {
    if (value == kNoValue || value >= aliases.size()) return value;
    ValueId current = value;
    while (current < aliases.size() && aliases[current] != current)
      current = aliases[current];
    ValueId cursor = value;
    while (cursor < aliases.size() && aliases[cursor] != cursor) {
      const auto next = aliases[cursor];
      aliases[cursor] = current;
      cursor = next;
    }
    return current;
  };

  auto is_const = [&](ValueId value, std::uint64_t* bits = nullptr) {
    const auto found = constants.find(resolve(value));
    if (found == constants.end()) return false;
    if (bits) *bits = found->second.bits;
    return true;
  };

  std::vector<Instruction> output;
  output.reserve(block.instructions.size());

  for (auto instruction : block.instructions) {
    for (std::size_t n = 0; n < instruction.args.size(); ++n)
      instruction.args.set(n, resolve(instruction.args[n]));

    auto alias_result = [&](ValueId replacement, std::size_t* category = nullptr) {
      if (instruction.result != kNoValue && instruction.result < aliases.size())
        aliases[instruction.result] = resolve(replacement);
      ++stats.copies_propagated;
      ++stats.instructions_removed;
      if (category) ++(*category);
    };

    bool removed = false;
    if (instruction.result != kNoValue && instruction.args.size() == 1) {
      const auto input = instruction.args[0];
      const auto input_type = input < value_types.size() ? value_types[input] : Type::Void;
      if (instruction.op == Op::AddImmediate && instruction.imm0 == 0) {
        alias_result(input, &stats.algebraic_simplifications);
        removed = true;
      } else if ((instruction.op == Op::Bitcast || instruction.op == Op::ZeroExtend ||
                  instruction.op == Op::SignExtend || instruction.op == Op::Truncate) &&
                 input_type == instruction.type) {
        alias_result(input, &stats.extensions_eliminated);
        removed = true;
      } else if (instruction.op == Op::Truncate && input < definitions.size() &&
                 definitions[input].has_value()) {
        const auto& producer = *definitions[input];
        if ((producer.op == Op::ZeroExtend || producer.op == Op::SignExtend) &&
            producer.args.size() == 1) {
          const auto original = resolve(producer.args[0]);
          const auto original_type = original < value_types.size()
                                         ? value_types[original]
                                         : Type::Void;
          if (original_type == instruction.type) {
            alias_result(original, &stats.extensions_eliminated);
            removed = true;
          }
        }
      }
    }

    if (!removed && instruction.result != kNoValue && instruction.args.size() == 2) {
      const auto a = instruction.args[0];
      const auto b = instruction.args[1];
      std::uint64_t ca = 0, cb = 0;
      const bool a_const = is_const(a, &ca);
      const bool b_const = is_const(b, &cb);
      const auto width = integer_width(instruction.type);
      const auto all_ones = width ? width_mask(width) : 0;

      auto alias_a = [&] { alias_result(a, &stats.algebraic_simplifications); removed = true; };
      auto alias_b = [&] { alias_result(b, &stats.algebraic_simplifications); removed = true; };

      switch (instruction.op) {
        case Op::Add:
          if (b_const && cb == 0) alias_a();
          else if (a_const && ca == 0) alias_b();
          break;
        case Op::Sub:
          if (b_const && cb == 0) alias_a();
          break;
        case Op::Mul:
          if (b_const && cb == 1) alias_a();
          else if (a_const && ca == 1) alias_b();
          break;
        case Op::And:
          if (b_const && cb == all_ones) alias_a();
          else if (a_const && ca == all_ones) alias_b();
          break;
        case Op::Or:
        case Op::Xor:
          if (b_const && cb == 0) alias_a();
          else if (a_const && ca == 0) alias_b();
          break;
        case Op::Shl:
        case Op::ShrLogical:
        case Op::ShrArithmetic:
        case Op::Rotl:
          if (b_const && cb == 0) alias_a();
          break;
        case Op::CompareEq:
        case Op::CompareUlt:
        case Op::CompareUgt:
        case Op::CompareSlt:
        case Op::CompareSgt:
        case Op::CompareNe:
          if (a == b) {
            const bool result = instruction.op == Op::CompareEq;
            instruction.op = Op::Constant;
            instruction.args.clear();
            instruction.imm0 = result ? 1u : 0u;
            instruction.imm1 = 0;
            ++stats.comparisons_simplified;
            ++stats.algebraic_simplifications;
          }
          break;
        default:
          break;
      }
    }

    if (!removed && instruction.op == Op::Select && instruction.args.size() == 3) {
      const auto condition = instruction.args[0];
      const auto yes = instruction.args[1];
      const auto no = instruction.args[2];
      std::uint64_t condition_value = 0;
      if (yes == no) {
        alias_result(yes, &stats.selects_simplified);
        removed = true;
      } else if (is_const(condition, &condition_value)) {
        alias_result(condition_value ? yes : no, &stats.selects_simplified);
        removed = true;
      }
    }

    if (removed) continue;

    if (instruction.result != kNoValue && instruction.result < value_types.size()) {
      value_types[instruction.result] = instruction.type;
      definitions[instruction.result] = instruction;
      if (instruction.op == Op::Constant)
        constants[instruction.result] = {instruction.type,
                                         normalize(instruction.imm0, instruction.type)};
    }
    output.push_back(std::move(instruction));
  }

  // Resolve aliases on all surviving users after the pass.
  for (auto& instruction : output)
    for (std::size_t n = 0; n < instruction.args.size(); ++n)
      instruction.args.set(n, resolve(instruction.args[n]));

  block.instructions = std::move(output);
  return stats;
}

}  // namespace xenon::cpu::ir::detail
