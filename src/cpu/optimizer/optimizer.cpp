#include "xenon/cpu/optimizer.hpp"

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

namespace xenon::cpu::ir {
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

std::size_t max_value_id(const Block& block) noexcept {
  std::size_t maximum = 0;
  bool any = false;
  for (const auto& i : block.instructions) {
    if (i.result != kNoValue) {
      maximum = std::max(maximum, static_cast<std::size_t>(i.result));
      any = true;
    }
    for (const auto arg : i.args) {
      if (arg != kNoValue) {
        maximum = std::max(maximum, static_cast<std::size_t>(arg));
        any = true;
      }
    }
  }
  return any ? maximum : 0;
}

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

struct TrackedValue {
  bool valid{};
  bool dirty{};
  ValueId value{kNoValue};
};

struct PromotedState {
  std::array<TrackedValue, 32> gpr{};
  std::array<TrackedValue, 32> fpr{};
  std::array<TrackedValue, 128> vector{};
  TrackedValue lr{};
  TrackedValue ctr{};
  TrackedValue xer_ca{};
  TrackedValue xer_ov{};
  TrackedValue xer_so{};
};

TrackedValue* slot_for(PromotedState& state, const Instruction& i) noexcept {
  switch (i.op) {
    case Op::ReadGpr:
    case Op::WriteGpr:
      return i.imm0 < state.gpr.size() ? &state.gpr[i.imm0] : nullptr;
    case Op::ReadFprBits:
    case Op::WriteFprBits:
      return i.imm0 < state.fpr.size() ? &state.fpr[i.imm0] : nullptr;
    case Op::ReadVector:
    case Op::WriteVector:
      return i.imm0 < state.vector.size() ? &state.vector[i.imm0] : nullptr;
    case Op::ReadLR:
    case Op::WriteLR:
      return &state.lr;
    case Op::ReadCTR:
    case Op::WriteCTR:
      return &state.ctr;
    case Op::ReadXerCA:
    case Op::SetXerCA:
      return &state.xer_ca;
    case Op::ReadXerOV:
    case Op::SetXerOV:
      return &state.xer_ov;
    case Op::ReadXerSO:
    case Op::SetXerSO:
      return &state.xer_so;
    default:
      return nullptr;
  }
}

bool promoted_read(Op op) noexcept {
  return op == Op::ReadGpr || op == Op::ReadFprBits || op == Op::ReadVector ||
         op == Op::ReadLR || op == Op::ReadCTR || op == Op::ReadXerCA ||
         op == Op::ReadXerOV || op == Op::ReadXerSO;
}

bool promoted_write(Op op) noexcept {
  return op == Op::WriteGpr || op == Op::WriteFprBits ||
         op == Op::WriteVector || op == Op::WriteLR || op == Op::WriteCTR ||
         op == Op::SetXerCA || op == Op::SetXerOV || op == Op::SetXerSO;
}

void copy_source(Instruction& destination, const Instruction& source) noexcept {
  destination.guest_address = source.guest_address;
  destination.guest_word = source.guest_word;
  destination.guest_opcode = source.guest_opcode;
}

void materialize(std::vector<Instruction>& out, Op op, std::uint64_t index,
                 ValueId value, const Instruction& source,
                 OptimizationStats& stats) {
  Instruction write{};
  write.op = op;
  write.type = Type::Void;
  write.args = {value};
  write.imm0 = index;
  copy_source(write, source);
  out.push_back(write);
  ++stats.state_materializations;
}

void flush(PromotedState& state, std::vector<Instruction>& out,
           const Instruction& source, OptimizationStats& stats) {
  for (std::size_t i = 0; i < state.gpr.size(); ++i)
    if (state.gpr[i].dirty) {
      materialize(out, Op::WriteGpr, i, state.gpr[i].value, source, stats);
      state.gpr[i].dirty = false;
    }
  for (std::size_t i = 0; i < state.fpr.size(); ++i)
    if (state.fpr[i].dirty) {
      materialize(out, Op::WriteFprBits, i, state.fpr[i].value, source, stats);
      state.fpr[i].dirty = false;
    }
  for (std::size_t i = 0; i < state.vector.size(); ++i)
    if (state.vector[i].dirty) {
      materialize(out, Op::WriteVector, i, state.vector[i].value, source, stats);
      state.vector[i].dirty = false;
    }
  if (state.lr.dirty) {
    materialize(out, Op::WriteLR, 0, state.lr.value, source, stats);
    state.lr.dirty = false;
  }
  if (state.ctr.dirty) {
    materialize(out, Op::WriteCTR, 0, state.ctr.value, source, stats);
    state.ctr.dirty = false;
  }
  if (state.xer_ca.dirty) {
    materialize(out, Op::SetXerCA, 0, state.xer_ca.value, source, stats);
    state.xer_ca.dirty = false;
  }
  if (state.xer_ov.dirty) {
    materialize(out, Op::SetXerOV, 0, state.xer_ov.value, source, stats);
    state.xer_ov.dirty = false;
  }
  if (state.xer_so.dirty) {
    materialize(out, Op::SetXerSO, 0, state.xer_so.value, source, stats);
    state.xer_so.dirty = false;
  }
}

void flush_xer_flags(PromotedState& state, std::vector<Instruction>& out,
                     const Instruction& source, OptimizationStats& stats) {
  if (state.xer_ca.dirty) {
    materialize(out, Op::SetXerCA, 0, state.xer_ca.value, source, stats);
    state.xer_ca.dirty = false;
  }
  if (state.xer_ov.dirty) {
    materialize(out, Op::SetXerOV, 0, state.xer_ov.value, source, stats);
    state.xer_ov.dirty = false;
  }
  if (state.xer_so.dirty) {
    materialize(out, Op::SetXerSO, 0, state.xer_so.value, source, stats);
    state.xer_so.dirty = false;
  }
}

void invalidate_xer_flags(PromotedState& state) noexcept {
  state.xer_ca = {};
  state.xer_ov = {};
  state.xer_so = {};
}

void invalidate(PromotedState& state) noexcept { state = {}; }

bool requires_materialization(const Instruction& i) noexcept {
  const auto effect = effects(i.op);
  if (has_effect(effect, Effect::MayFault) || has_effect(effect, Effect::Trap) ||
      has_effect(effect, Effect::ControlFlow) ||
      has_effect(effect, Effect::Barrier) ||
      has_effect(effect, Effect::Synchronization))
    return true;
  return i.op == Op::Call || i.op == Op::CallIndirect || i.op == Op::WriteSPR;
}

bool invalidates_after(const Instruction& i) noexcept {
  switch (i.op) {
    case Op::Call:
    case Op::CallIndirect:
    case Op::WriteSPR:
    case Op::StringLoad:
      return true;
    case Op::BranchIf:
    case Op::BranchIndirect:
      return i.imm0 != 0u;
    default:
      return false;
  }
}

OptimizationStats promote_architectural_state(Block& block) {
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
    ValueId cursor = value;
    while (cursor < aliases.size() && aliases[cursor] != cursor) {
      auto next = aliases[cursor];
      aliases[cursor] = current;
      cursor = next;
    }
    return current;
  };

  PromotedState state{};
  std::vector<Instruction> out;
  out.reserve(block.instructions.size() + 8u);
  const Instruction end_source = block.instructions.back();

  for (auto instruction : block.instructions) {
    for (std::size_t a = 0; a < instruction.args.size(); ++a)
      instruction.args.set(a, resolve(instruction.args[a]));

    if (promoted_read(instruction.op)) {
      auto* slot = slot_for(state, instruction);
      if (slot && slot->valid) {
        if (instruction.result != kNoValue && instruction.result < aliases.size())
          aliases[instruction.result] = resolve(slot->value);
        ++stats.state_reads_eliminated;
        if (instruction.op == Op::ReadXerCA || instruction.op == Op::ReadXerOV ||
            instruction.op == Op::ReadXerSO)
          ++stats.xer_reads_eliminated;
        ++stats.instructions_removed;
        continue;
      }
      if (slot) {
        slot->valid = true;
        slot->dirty = false;
        slot->value = instruction.result;
      }
      out.push_back(std::move(instruction));
      continue;
    }

    if (promoted_write(instruction.op)) {
      auto* slot = slot_for(state, instruction);
      if (slot && !instruction.args.empty()) {
        slot->valid = true;
        slot->dirty = true;
        slot->value = resolve(instruction.args[0]);
        ++stats.state_writes_deferred;
        if (instruction.op == Op::SetXerCA || instruction.op == Op::SetXerOV ||
            instruction.op == Op::SetXerSO)
          ++stats.xer_writes_deferred;
        ++stats.instructions_removed;
        continue;
      }
    }

    if (instruction.op == Op::ReadXER || instruction.op == Op::MoveXERToCR)
      flush_xer_flags(state, out, instruction, stats);
    if (requires_materialization(instruction)) flush(state, out, instruction, stats);
    out.push_back(std::move(instruction));
    if (out.back().op == Op::WriteXER || out.back().op == Op::MoveXERToCR)
      invalidate_xer_flags(state);
    if (invalidates_after(out.back())) invalidate(state);
  }

  flush(state, out, end_source, stats);
  block.instructions = std::move(out);
  return stats;
}

struct PendingCRCompare {
  bool valid{};
  Instruction write{};
};

OptimizationStats forward_cr_compare_state(Block& block) {
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
    ValueId cursor = value;
    while (cursor < aliases.size() && aliases[cursor] != cursor) {
      auto next = aliases[cursor];
      aliases[cursor] = current;
      cursor = next;
    }
    return current;
  };

  std::array<PendingCRCompare, 8> pending{};
  std::vector<Instruction> out;
  out.reserve(block.instructions.size() + 4u);

  auto flush_field = [&](unsigned field) {
    field &= 7u;
    auto& p = pending[field];
    if (!p.valid) return;
    out.push_back(std::move(p.write));
    p = {};
    ++stats.state_materializations;
  };
  auto flush_all = [&] {
    for (unsigned f = 0; f < 8; ++f) flush_field(f);
  };
  auto drop_field = [&](unsigned field) { pending[field & 7u] = {}; };
  auto drop_all = [&] {
    for (auto& p : pending) p = {};
  };

  for (auto instruction : block.instructions) {
    for (std::size_t a = 0; a < instruction.args.size(); ++a)
      instruction.args.set(a, resolve(instruction.args[a]));

    if (instruction.op == Op::WriteCRCompare) {
      auto& p = pending[static_cast<unsigned>(instruction.imm0) & 7u];
      p.valid = true;
      p.write = std::move(instruction);
      ++stats.cr_writes_deferred;
      ++stats.instructions_removed;
      continue;
    }

    if (instruction.op == Op::ReadCRBit) {
      const unsigned bit = static_cast<unsigned>(instruction.imm0) & 31u;
      auto& p = pending[bit / 4u];
      if (p.valid && p.write.args.size() == 4u) {
        const auto replacement = resolve(p.write.args[bit % 4u]);
        if (instruction.result != kNoValue && instruction.result < aliases.size())
          aliases[instruction.result] = replacement;
        ++stats.cr_reads_eliminated;
        ++stats.state_reads_eliminated;
        ++stats.instructions_removed;
        continue;
      }
    }

    if (instruction.op == Op::ReadCR)
      flush_all();
    else if (instruction.op == Op::ReadCRField)
      flush_field(static_cast<unsigned>(instruction.imm0));
    else if (instruction.op == Op::ReadCRBit)
      flush_field(static_cast<unsigned>(instruction.imm0) / 4u);

    switch (instruction.op) {
      case Op::WriteCR:
        drop_all();
        break;
      case Op::WriteCRField:
        drop_field(static_cast<unsigned>(instruction.imm0));
        break;
      case Op::WriteCRBit:
        flush_field(static_cast<unsigned>(instruction.imm0) / 4u);
        break;
      case Op::MoveXERToCR:
        drop_field(static_cast<unsigned>(instruction.imm0));
        break;
      case Op::UpdateCR0Signed:
      case Op::WriteCR0StoreConditional:
        drop_field(0);
        break;
      case Op::UpdateCR1FromFPSCR:
        drop_field(1);
        break;
      case Op::MoveFPSCRFieldToCR:
        drop_field(static_cast<unsigned>(instruction.imm0));
        break;
      case Op::VUpdateCR6:
        drop_field(6);
        break;
      case Op::MoveCRFields:
        for (unsigned f = 0; f < 8; ++f)
          if (instruction.imm0 & (0x80u >> f)) drop_field(f);
        break;
      default:
        break;
    }

    if (requires_materialization(instruction)) flush_all();
    out.push_back(std::move(instruction));
  }

  flush_all();
  block.instructions = std::move(out);
  return stats;
}

}  // namespace

std::string OptimizationReport::format() const {
  std::ostringstream out;
  out << "Function " << std::hex << std::uppercase << function << std::dec
      << "\n"
      << "guest instructions: " << guest_instructions << "\n"
      << "blocks: " << blocks_before << " -> " << blocks_after << "\n"
      << "IR: " << ir_before << " -> " << ir_after << "\n"
      << "constants folded: " << stats.constants_folded << "\n"
      << "algebraic simplifications: " << stats.algebraic_simplifications << "\n"
      << "copies propagated: " << stats.copies_propagated << "\n"
      << "CSE eliminated: " << stats.common_subexpressions_eliminated << "\n"
      << "dead IR eliminated: " << stats.dead_code_eliminated << "\n"
      << "state reads eliminated: " << stats.state_reads_eliminated << "\n"
      << "state writes deferred: " << stats.state_writes_deferred << "\n"
      << "CR reads eliminated: " << stats.cr_reads_eliminated << "\n"
      << "XER reads eliminated: " << stats.xer_reads_eliminated;
  return out.str();
}

OptimizationStats simplify_constant_control_flow(Function& function) {
  OptimizationStats stats{};
  if (function.blocks.empty()) return stats;

  for (auto& block : function.blocks) {
    std::map<ValueId, std::uint64_t> constants;
    for (const auto& instruction : block.instructions) {
      if (instruction.op == Op::Constant && instruction.result != kNoValue)
        constants[instruction.result] = instruction.imm0;
    }

    for (std::size_t index = 0; index < block.instructions.size();) {
      auto& instruction = block.instructions[index];
      if (instruction.op != Op::BranchIf || instruction.args.size() != 2u) {
        ++index;
        continue;
      }
      const auto condition = constants.find(instruction.args[0]);
      if (condition == constants.end()) {
        ++index;
        continue;
      }

      const bool taken = condition->second != 0u;
      const auto target_value = instruction.args[1];
      const auto target_constant = constants.find(target_value);
      const auto edge_kind = instruction.imm0 ? EdgeKind::Call : EdgeKind::Branch;

      if (taken) {
        instruction.op = instruction.imm0 ? Op::Call : Op::Branch;
        instruction.args = {target_value};
        instruction.imm0 = 0;
        instruction.imm1 = 0;
        if (edge_kind == EdgeKind::Branch) {
          block.successors.erase(
              std::remove_if(block.successors.begin(), block.successors.end(),
                  [&](const ControlFlowEdge& edge) {
                    if (edge.kind == EdgeKind::Fallthrough) return true;
                    if (target_constant != constants.end())
                      return edge.kind == EdgeKind::Branch &&
                             edge.target != static_cast<GuestAddress>(target_constant->second);
                    return false;
                  }),
              block.successors.end());
        }
        ++stats.branches_simplified;
        ++index;
      } else {
        if (target_constant != constants.end()) {
          const auto target = static_cast<GuestAddress>(target_constant->second);
          block.successors.erase(
              std::remove_if(block.successors.begin(), block.successors.end(),
                  [&](const ControlFlowEdge& edge) {
                    return edge.kind == edge_kind && edge.target == target;
                  }),
              block.successors.end());
        }
        block.instructions.erase(block.instructions.begin() +
                                 static_cast<std::ptrdiff_t>(index));
        ++stats.branches_simplified;
        ++stats.instructions_removed;
      }
    }
  }

  // Branch simplification may remove local incoming edges. Rebuild predecessor
  // metadata from the surviving CFG so later phases can trust it exactly.
  for (auto& block : function.blocks) block.predecessors.clear();
  std::map<GuestAddress, Block*> by_address;
  for (auto& block : function.blocks) by_address.emplace(block.guest_address, &block);
  for (const auto& source : function.blocks) {
    for (const auto& edge : source.successors) {
      if (!edge.local || edge.kind == EdgeKind::Call) continue;
      const auto target = by_address.find(edge.target);
      if (target == by_address.end()) continue;
      auto& predecessors = target->second->predecessors;
      if (std::find(predecessors.begin(), predecessors.end(), source.guest_address) ==
          predecessors.end())
        predecessors.push_back(source.guest_address);
    }
  }
  return stats;
}

OptimizationStats Optimizer::run(Block& block) const {
  OptimizationStats stats{};
  // Fixed deterministic pipeline. The second scalar cleanup wave consumes
  // aliases/constants exposed by architectural-state and CR/XER promotion.
  stats += fold_constants(block);
  stats += simplify_values(block);
  stats += eliminate_common_subexpressions(block);
  stats += eliminate_dead_code(block);
  stats += promote_architectural_state(block);
  stats += forward_cr_compare_state(block);
  stats += fold_constants(block);
  stats += simplify_values(block);
  stats += eliminate_common_subexpressions(block);
  stats += eliminate_dead_code(block);
  return stats;
}

OptimizationReport Optimizer::run_with_report(Function& function) const {
  OptimizationReport report{};
  report.function = function.guest_address;
  report.blocks_before = function.blocks.size();
  std::set<GuestAddress> guest_addresses;
  for (const auto& block : function.blocks) {
    report.ir_before += block.instructions.size();
    for (const auto& instruction : block.instructions)
      if (instruction.guest_opcode.valid()) guest_addresses.insert(instruction.guest_address);
  }
  report.guest_instructions = guest_addresses.size();
  for (auto& block : function.blocks) report.stats += run(block);
  report.stats += simplify_constant_control_flow(function);
  report.blocks_after = function.blocks.size();
  for (const auto& block : function.blocks) report.ir_after += block.instructions.size();
  return report;
}

OptimizationStats Optimizer::run(Function& function) const {
  return run_with_report(function).stats;
}

}  // namespace xenon::cpu::ir
