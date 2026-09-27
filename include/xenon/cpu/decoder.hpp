// decoder.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "xenon/cpu/instruction.hpp"

namespace xenon::cpu {

struct OpcodeCatalogValidation {
  std::size_t entry_count{};
  std::size_t ambiguous_overlaps{};
  std::size_t duplicate_ids{};
  bool deterministic{};
};

class Decoder {
 public:
  [[nodiscard]] DecodedInstruction decode(GuestAddress address,
                                           std::uint32_t word) const noexcept;
  [[nodiscard]] static std::span<const OpcodeInfo> opcode_catalog() noexcept;
  [[nodiscard]] static const OpcodeInfo* opcode_info(OpcodeId id) noexcept;
  [[nodiscard]] static OpcodeCatalogValidation validate_catalog() noexcept;
};

}  // namespace xenon::cpu