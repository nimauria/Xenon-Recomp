#include "xenon/gpu/shader.hpp"

#include <stdexcept>

namespace xenon::gpu {

ShaderProgram::ShaderProgram(ShaderStage stage,
                             std::span<const std::uint32_t> dwords,
                             std::uint32_t start_slot)
    : stage_(stage), start_slot_(start_slot), dwords_(dwords.begin(), dwords.end()),
      hash_(calculate_hash(stage, start_slot, dwords)) {}

ShaderInstruction96 ShaderProgram::instruction(std::size_t index) const {
  if (index >= instruction_count()) {
    throw std::out_of_range("Xenos shader instruction index out of range");
  }
  const auto offset = index * 3u;
  return ShaderInstruction96{{dwords_[offset], dwords_[offset + 1],
                              dwords_[offset + 2]}};
}

ControlFlowPair ShaderProgram::control_flow_pair(std::size_t index) const {
  const auto group = instruction(index);
  const std::uint32_t d0 = group.words[0];
  const std::uint32_t d1 = group.words[1];
  const std::uint32_t d2 = group.words[2];

  ControlFlowPair result{};
  result.instructions[0].word0 = d0;
  result.instructions[0].word1 = static_cast<std::uint16_t>(d1 & 0xFFFFu);
  result.instructions[1].word0 = (d1 >> 16) | (d2 << 16);
  result.instructions[1].word1 = static_cast<std::uint16_t>(d2 >> 16);
  for (auto& instruction : result.instructions) {
    instruction.opcode = static_cast<ControlFlowOpcode>((instruction.word1 >> 12) & 0xFu);
  }
  return result;
}

std::uint64_t ShaderProgram::calculate_hash(
    ShaderStage stage, std::uint32_t start_slot,
    std::span<const std::uint32_t> dwords) noexcept {
  // Stable FNV-1a over the architecture-visible shader identity. No host
  // pointers, backend options or process-random state participate.
  std::uint64_t hash = 14695981039346656037ull;
  auto add_byte = [&hash](std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  add_byte(static_cast<std::uint8_t>(stage));
  for (unsigned shift = 0; shift < 32; shift += 8) {
    add_byte(static_cast<std::uint8_t>(start_slot >> shift));
  }
  for (auto word : dwords) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      add_byte(static_cast<std::uint8_t>(word >> shift));
    }
  }
  return hash;
}

}  // namespace xenon::gpu
