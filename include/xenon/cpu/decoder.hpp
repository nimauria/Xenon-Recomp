#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "xenon/cpu/instruction.hpp"

namespace xenon::cpu {

class Decoder {
 public:
  [[nodiscard]] DecodedInstruction decode(GuestAddress address,
                                           std::uint32_t word) const noexcept;
  [[nodiscard]] static std::span<const OpcodeInfo> opcode_catalog() noexcept;
};

}  // namespace xenon::cpu
