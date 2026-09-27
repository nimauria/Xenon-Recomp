#include "xenon/cpu/function_compiler.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#include "xenon/cpu/ir_verifier.hpp"
#include "xenon/cpu/optimizer.hpp"

namespace xenon::cpu {
namespace {

[[nodiscard]] bool is_direct_branch(const DecodedInstruction& instruction) noexcept {
  if (!instruction.info || instruction.info->group != InstructionGroup::Branch)
    return false;
  return instruction.info->format == InstructionFormat::I ||
         instruction.info->format == InstructionFormat::B;
}

[[nodiscard]] bool is_indirect_branch(const DecodedInstruction& instruction) noexcept {
  return instruction.info && instruction.info->group == InstructionGroup::Branch &&
         instruction.info->format == InstructionFormat::XL;
}

[[nodiscard]] bool is_syscall(const DecodedInstruction& instruction) noexcept {
  return instruction.info && instruction.info->format == InstructionFormat::SC;
}

[[nodiscard]] bool is_trap(const DecodedInstruction& instruction) noexcept {
  if (!instruction.info || instruction.info->group != InstructionGroup::Branch)
    return false;
  if (is_direct_branch(instruction) || is_indirect_branch(instruction) ||
      is_syscall(instruction))
    return false;
  return instruction.info->format == InstructionFormat::D ||
         instruction.info->format == InstructionFormat::X;
}

[[nodiscard]] bool is_instruction_sync(const DecodedInstruction& instruction) noexcept {
  return instruction.info && instruction.info->group == InstructionGroup::Integer &&
         instruction.info->format == InstructionFormat::XL &&
         instruction.mnemonic() == "isync";
}

[[nodiscard]] bool branch_condition_is_unconditional(
    const DecodedInstruction& instruction) noexcept {
  if (instruction.info->format == InstructionFormat::I) return true;
  if (instruction.info->format == InstructionFormat::XL &&
      instruction.mnemonic() == "bcctrx") {
    return (instruction.bo() & 0b10000u) != 0u;
  }
  if (instruction.info->format == InstructionFormat::B ||
      instruction.info->format == InstructionFormat::XL) {
    return (instruction.bo() & 0b10100u) == 0b10100u;
  }
  return false;
}

[[nodiscard]] bool ends_basic_block(const DecodedInstruction& instruction) noexcept {
  // Calls are block boundaries too. This is semantically neutral for ordinary
  // calls and guarantees a setjmp continuation has an explicit local label to
  // which a propagated guest LongJump can return.
  if (instruction.info && instruction.info->group == InstructionGroup::Branch &&
      instruction.lk())
    return true;
  if ((is_direct_branch(instruction) || is_indirect_branch(instruction)) &&
      !instruction.lk())
    return true;
  return is_syscall(instruction) || is_trap(instruction) ||
         is_instruction_sync(instruction);
}

void add_edge(ir::Block& block, GuestAddress target, ir::EdgeKind kind,
              bool local) {
  const auto duplicate = std::any_of(
      block.successors.begin(), block.successors.end(),
      [&](const ir::ControlFlowEdge& edge) {
        return edge.target == target && edge.kind == kind && edge.local == local;
      });
  if (!duplicate) block.successors.push_back({target, kind, local});
}

}  // namespace

FunctionCompileResult StaticFunctionCompiler::compile(
    GuestAddress base, std::span<const std::uint32_t> words) const {
  const FunctionCodeRange range{base, words};
  return compile_ranges(base, std::span<const FunctionCodeRange>(&range, 1u));
}

FunctionCompileResult StaticFunctionCompiler::compile_ranges(
    GuestAddress entry, std::span<const FunctionCodeRange> ranges) const {
  FunctionCompileResult out{};
  out.function.guest_address = entry;
  if (ranges.empty()) {
    out.error_address = entry;
    out.error = "empty guest function";
    return out;
  }

  // Decode every declared range into one sparse address map. Duplicate or
  // overlapping ranges are rejected here even though schema validation should
  // normally catch them earlier; the compiler remains safe for direct callers.
  std::map<GuestAddress, DecodedInstruction> decoded;
  std::vector<GuestAddress> range_starts;
  for (const auto& range : ranges) {
    if (range.words.empty()) {
      out.error_address = range.base;
      out.error = "empty guest function range";
      return out;
    }
    if ((range.base & 3u) != 0u) {
      out.error_address = range.base;
      out.error = "unaligned guest function range";
      return out;
    }
    range_starts.push_back(range.base);
    for (std::size_t n = 0; n < range.words.size(); ++n) {
      const auto address = static_cast<GuestAddress>(range.base + n * 4u);
      const auto word = range.words[n];
      auto instruction = decoder_.decode(address, word);
      if (!instruction.valid()) {
        out.error_address = address;
        out.error_word = word;
        out.error = "unknown Xenon/PPC instruction";
        return out;
      }
      if (!decoded.emplace(address, instruction).second) {
        out.error_address = address;
        out.error_word = word;
        out.error = "overlapping guest function ranges";
        return out;
      }
    }
  }

  if (!decoded.contains(entry)) {
    out.error_address = entry;
    out.error = "logical function entry is outside declared ranges";
    return out;
  }

  const auto in_declared_range = [&](GuestAddress address) {
    return std::any_of(ranges.begin(), ranges.end(), [address](const auto& range) {
      const auto end = static_cast<std::uint64_t>(range.base) +
                       static_cast<std::uint64_t>(range.words.size()) * 4u;
      return address >= range.base && static_cast<std::uint64_t>(address) < end;
    });
  };
  for (const auto& [address, instruction] : decoded) {
    if (!is_direct_branch(instruction) || instruction.lk()) continue;
    const auto target = instruction.direct_branch_target();
    if (in_declared_range(target) && !decoded.contains(target)) {
      out.error_address = address;
      out.error_word = instruction.word;
      out.error = "direct branch target is inside a declared range but has no decoded local block";
      return out;
    }
  }

  std::map<GuestAddress, bool> leader;
  leader[entry] = true;
  for (const auto start : range_starts) leader[start] = true;

  for (const auto& [address, instruction] : decoded) {
    if (is_direct_branch(instruction)) {
      const auto target = instruction.direct_branch_target();
      if (decoded.contains(target)) leader[target] = true;
    }
    const auto next = static_cast<GuestAddress>(address + 4u);
    if (ends_basic_block(instruction) && decoded.contains(next)) leader[next] = true;
  }

  // Partition the sparse address map into contiguous guest basic blocks. A
  // discontinuity always starts a block even if no branch targets it.
  auto it = decoded.begin();
  while (it != decoded.end()) {
    const auto block_start = it->first;
    ir::Block block{};
    block.guest_address = block_start;
    ir::Builder builder(block);

    auto current = it;
    while (current != decoded.end()) {
      const auto before = block.instructions.size();
      if (!lifter_.lift(current->second, builder) ||
          block.instructions.size() == before) {
        out.error_address = current->first;
        out.error_word = current->second.word;
        out.error = "recognized instruction has no Xenon IR lowering";
        return out;
      }

      const auto next_address = static_cast<GuestAddress>(current->first + 4u);
      auto next = std::next(current);
      const bool contiguous = next != decoded.end() && next->first == next_address;
      const bool next_is_leader = contiguous && leader.contains(next->first);
      current = next;
      if (!contiguous || next_is_leader) break;
    }

    const auto last_address = static_cast<GuestAddress>(
        block.instructions.empty() ? block_start :
        block.instructions.back().guest_address);
    block.end_address = last_address + 4u;
    out.function.blocks.push_back(std::move(block));
    it = current;
  }

  std::unordered_map<GuestAddress, std::size_t> block_by_address;
  block_by_address.reserve(out.function.blocks.size());
  for (std::size_t i = 0; i < out.function.blocks.size(); ++i)
    block_by_address.emplace(out.function.blocks[i].guest_address, i);

  for (auto& block : out.function.blocks) {
    for (auto address = block.guest_address; address < block.end_address;
         address += 4u) {
      const auto decoded_it = decoded.find(address);
      if (decoded_it == decoded.end()) break;
      const auto& instruction = decoded_it->second;
      if (is_direct_branch(instruction) && instruction.lk()) {
        const auto target = instruction.direct_branch_target();
        add_edge(block, target, ir::EdgeKind::Call,
                 block_by_address.contains(target));
      } else if (is_indirect_branch(instruction) && instruction.lk()) {
        block.has_indirect_call = true;
      }
    }

    const auto last_address = static_cast<GuestAddress>(block.end_address - 4u);
    const auto& last = decoded.at(last_address);
    const auto next = static_cast<GuestAddress>(last_address + 4u);
    const bool has_next = block_by_address.contains(next);

    if ((is_direct_branch(last) || is_indirect_branch(last)) && last.lk()) {
      if (has_next) add_edge(block, next, ir::EdgeKind::Fallthrough, true);
      else block.has_external_exit = true;
    } else if (is_direct_branch(last) && !last.lk()) {
      const auto target = last.direct_branch_target();
      const bool local = block_by_address.contains(target);
      add_edge(block, target, ir::EdgeKind::Branch, local);
      block.has_external_exit |= !local;
      if (!branch_condition_is_unconditional(last)) {
        if (has_next) add_edge(block, next, ir::EdgeKind::Fallthrough, true);
        else block.has_external_exit = true;
      }
    } else if (is_indirect_branch(last) && !last.lk()) {
      block.has_indirect_exit = true;
      if (!branch_condition_is_unconditional(last)) {
        if (has_next) add_edge(block, next, ir::EdgeKind::Fallthrough, true);
        else block.has_external_exit = true;
      }
    } else if (is_syscall(last)) {
      block.has_external_exit = true;
    } else if (is_instruction_sync(last)) {
      block.has_external_exit = true;
      add_edge(block, next, ir::EdgeKind::Branch, false);
    } else if (is_trap(last)) {
      block.has_external_exit = true;
      if (has_next) add_edge(block, next, ir::EdgeKind::Fallthrough, true);
    } else if (has_next) {
      add_edge(block, next, ir::EdgeKind::Fallthrough, true);
    } else {
      block.has_external_exit = true;
    }
  }

  for (const auto& block : out.function.blocks) {
    for (const auto& edge : block.successors) {
      if (!edge.local || edge.kind == ir::EdgeKind::Call) continue;
      const auto target = block_by_address.find(edge.target);
      if (target == block_by_address.end()) continue;
      auto& predecessors = out.function.blocks[target->second].predecessors;
      if (std::find(predecessors.begin(), predecessors.end(),
                    block.guest_address) == predecessors.end())
        predecessors.push_back(block.guest_address);
    }
  }

  ir::Verifier verifier;
  if (const auto validation = verifier.verify(out.function); !validation.ok) {
    out.error_address = validation.block_address;
    out.error = "Xenon IR verification failed before optimization: " +
                validation.message;
    return out;
  }

  ir::Optimizer optimizer;
  (void)optimizer.run(out.function);

  if (const auto validation = verifier.verify(out.function); !validation.ok) {
    out.error_address = validation.block_address;
    out.error = "Xenon IR verification failed after optimization: " +
                validation.message;
    return out;
  }
  out.ok = true;
  return out;
}

}  // namespace xenon::cpu
