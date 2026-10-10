// Architectural-state promotion (GPR/XER/CR) and CR compare forwarding.

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

struct PendingCRCompare {
  bool valid{};
  Instruction write{};
};

}  // namespace

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

}  // namespace xenon::cpu::ir::detail
