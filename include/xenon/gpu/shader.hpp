#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xenon/gpu/types.hpp"

namespace xenon::gpu {


enum class ControlFlowOpcode : std::uint8_t {
  Nop = 0,
  Exec = 1,
  ExecEnd = 2,
  CondExec = 3,
  CondExecEnd = 4,
  CondExecPred = 5,
  CondExecPredEnd = 6,
  LoopStart = 7,
  LoopEnd = 8,
  CondCall = 9,
  Return = 10,
  CondJump = 11,
  Alloc = 12,
  CondExecPredClean = 13,
  CondExecPredCleanEnd = 14,
  MarkVsFetchDone = 15,
};

struct ControlFlowInstruction48 {
  std::uint32_t word0{};
  std::uint16_t word1{};
  ControlFlowOpcode opcode{ControlFlowOpcode::Nop};

  [[nodiscard]] bool is_exec() const noexcept {
    const auto o = static_cast<std::uint8_t>(opcode);
    return (o >= 1 && o <= 6) || o == 13 || o == 14;
  }
  [[nodiscard]] std::uint32_t exec_address() const noexcept { return word0 & 0xFFFu; }
  [[nodiscard]] std::uint32_t exec_count() const noexcept { return (word0 >> 12) & 0x7u; }
  [[nodiscard]] bool exec_yield() const noexcept { return ((word0 >> 15) & 1u) != 0; }
  [[nodiscard]] std::uint32_t exec_sequence() const noexcept { return (word0 >> 16) & 0xFFFu; }
  [[nodiscard]] bool absolute_addressing() const noexcept { return ((word1 >> 11) & 1u) != 0; }
};

struct ControlFlowPair {
  std::array<ControlFlowInstruction48, 2> instructions{};
};

struct ShaderInstruction96 {
  std::array<std::uint32_t, 3> words{};
};

class ShaderProgram {
 public:
  ShaderProgram() = default;
  ShaderProgram(ShaderStage stage, std::span<const std::uint32_t> dwords,
                std::uint32_t start_slot = 0);

  [[nodiscard]] ShaderStage stage() const noexcept { return stage_; }
  [[nodiscard]] std::uint32_t start_slot() const noexcept { return start_slot_; }
  [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
  [[nodiscard]] bool complete_instruction_stream() const noexcept {
    return (dwords_.size() % 3u) == 0;
  }
  [[nodiscard]] std::size_t instruction_count() const noexcept {
    return dwords_.size() / 3u;
  }
  [[nodiscard]] std::span<const std::uint32_t> dwords() const noexcept {
    return dwords_;
  }
  [[nodiscard]] ShaderInstruction96 instruction(std::size_t index) const;
  // Every 96-bit microcode group can also contain two packed 48-bit control-
  // flow instructions. Whether the group is part of the active CF stream is
  // determined by the shader control flow itself.
  [[nodiscard]] ControlFlowPair control_flow_pair(std::size_t index) const;

 private:
  static std::uint64_t calculate_hash(ShaderStage stage, std::uint32_t start_slot,
                                      std::span<const std::uint32_t> dwords) noexcept;

  ShaderStage stage_{ShaderStage::Vertex};
  std::uint32_t start_slot_{};
  std::vector<std::uint32_t> dwords_{};
  std::uint64_t hash_{};
};

}  // namespace xenon::gpu
