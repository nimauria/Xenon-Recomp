#include "xenon/cpu/function_compiler.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "xenon/cpu/ir_verifier.hpp"
#include "xenon/cpu/optimizer.hpp"

namespace xenon::cpu {
namespace {

[[nodiscard]] bool in_function(GuestAddress base, std::size_t word_count,
                               GuestAddress target) noexcept {
  const auto begin = std::uint64_t{base};
  const auto end = begin + word_count * 4ull;
  return (target & 3u) == 0u && std::uint64_t{target} >= begin &&
         std::uint64_t{target} < end;
}

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
  // isync is the only integer XL-form instruction in the current catalogue whose
  // execution intentionally leaves the translation to revalidate executable code.
  return instruction.info && instruction.info->group == InstructionGroup::Integer &&
         instruction.info->format == InstructionFormat::XL &&
         instruction.mnemonic() == "isync";
}

[[nodiscard]] bool branch_condition_is_unconditional(
    const DecodedInstruction& instruction) noexcept {
  if (instruction.info->format == InstructionFormat::I) return true;
  if (instruction.info->format == InstructionFormat::XL &&
      instruction.mnemonic() == "bcctrx") {
    // bcctr does not decrement/test CTR; BO[0] controls whether CR is ignored.
    return (instruction.bo() & 0b10000u) != 0u;
  }
  if (instruction.info->format == InstructionFormat::B ||
      instruction.info->format == InstructionFormat::XL) {
    // Ignore both CTR and CR tests.
    return (instruction.bo() & 0b10100u) == 0b10100u;
  }
  return false;
}

[[nodiscard]] bool ends_basic_block(const DecodedInstruction& instruction) noexcept {
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
  FunctionCompileResult out{};
  out.function.guest_address = base;
  if (words.empty()) {
    out.error_address = base;
    out.error = "empty guest function";
    return out;
  }

  // Decode once during recompilation. Runtime execution never performs guest
  // opcode decoding.
  std::vector<DecodedInstruction> decoded;
  decoded.reserve(words.size());
  for (std::size_t n = 0; n < words.size(); ++n) {
    const auto address = static_cast<GuestAddress>(base + n * 4u);
    const auto word = words[n];
    auto instruction = decoder_.decode(address, word);
    if (!instruction.valid()) {
      out.error_address = address;
      out.error_word = word;
      out.error = "unknown Xenon/PPC instruction";
      return out;
    }
    decoded.push_back(instruction);
  }

  // Discover real guest basic-block leaders before lifting. Function boundaries
  // come from the analysis/game layer; this pass only partitions the supplied
  // static function body.
  std::vector<bool> leader(words.size(), false);
  leader.front() = true;
  for (std::size_t n = 0; n < decoded.size(); ++n) {
    const auto& instruction = decoded[n];
    if (is_direct_branch(instruction)) {
      const auto target = instruction.direct_branch_target();
      if (in_function(base, words.size(), target)) {
        leader[(target - base) / 4u] = true;
      }
    }
    if (ends_basic_block(instruction) && n + 1u < decoded.size()) {
      leader[n + 1u] = true;
    }
  }

  std::vector<std::size_t> starts;
  starts.reserve(words.size());
  for (std::size_t n = 0; n < leader.size(); ++n) {
    if (leader[n]) starts.push_back(n);
  }
  out.function.blocks.reserve(starts.size());

  for (std::size_t block_index = 0; block_index < starts.size(); ++block_index) {
    const auto begin = starts[block_index];
    const auto end = block_index + 1u < starts.size() ? starts[block_index + 1u]
                                                      : decoded.size();
    ir::Block block{};
    block.guest_address = decoded[begin].address;
    block.end_address = static_cast<GuestAddress>(base + end * 4u);
    ir::Builder builder(block);

    for (std::size_t n = begin; n < end; ++n) {
      const auto before = block.instructions.size();
      if (!lifter_.lift(decoded[n], builder) || block.instructions.size() == before) {
        out.error_address = decoded[n].address;
        out.error_word = decoded[n].word;
        out.error = "recognized instruction has no Xenon IR lowering";
        return out;
      }
    }
    out.function.blocks.push_back(std::move(block));
  }

  std::unordered_map<GuestAddress, std::size_t> block_by_address;
  block_by_address.reserve(out.function.blocks.size());
  for (std::size_t i = 0; i < out.function.blocks.size(); ++i) {
    block_by_address.emplace(out.function.blocks[i].guest_address, i);
  }

  // Populate explicit CFG metadata. Calls are recorded as call edges but are not
  // successor/predecessor edges because the containing guest block continues on
  // return. Direct call linking is a later CPU V2 phase.
  for (auto& block : out.function.blocks) {
    const auto begin = (block.guest_address - base) / 4u;
    const auto end = (block.end_address - base) / 4u;

    for (std::size_t n = begin; n < end; ++n) {
      const auto& instruction = decoded[n];
      if (is_direct_branch(instruction) && instruction.lk()) {
        const auto target = instruction.direct_branch_target();
        add_edge(block, target, ir::EdgeKind::Call,
                 block_by_address.contains(target));
      } else if (is_indirect_branch(instruction) && instruction.lk()) {
        block.has_indirect_call = true;
      }
    }

    const auto& last = decoded[end - 1u];
    const auto next = end < decoded.size()
                          ? decoded[end].address
                          : static_cast<GuestAddress>(base + decoded.size() * 4u);

    if (is_direct_branch(last) && !last.lk()) {
      const auto target = last.direct_branch_target();
      const bool local = block_by_address.contains(target);
      add_edge(block, target, ir::EdgeKind::Branch, local);
      block.has_external_exit |= !local;
      if (!branch_condition_is_unconditional(last)) {
        if (end < decoded.size())
          add_edge(block, next, ir::EdgeKind::Fallthrough, true);
        else
          block.has_external_exit = true;
      }
    } else if (is_indirect_branch(last) && !last.lk()) {
      block.has_indirect_exit = true;
      if (!branch_condition_is_unconditional(last)) {
        if (end < decoded.size())
          add_edge(block, next, ir::EdgeKind::Fallthrough, true);
        else
          block.has_external_exit = true;
      }
    } else if (is_syscall(last)) {
      block.has_external_exit = true;
    } else if (is_instruction_sync(last)) {
      // isync leaves the native translation even when the next guest address is
      // inside the same discovered function so executable-page generations are
      // revalidated before continuing.
      block.has_external_exit = true;
      add_edge(block, next, ir::EdgeKind::Branch, false);
    } else if (is_trap(last)) {
      block.has_external_exit = true;  // taken trap
      if (end < decoded.size())
        add_edge(block, next, ir::EdgeKind::Fallthrough, true);
    } else if (end < decoded.size()) {
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
                    block.guest_address) == predecessors.end()) {
        predecessors.push_back(block.guest_address);
      }
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
