#include "xenon/cpu/decoder.hpp"

#include <array>
#include <bit>

namespace xenon::cpu {

// The catalog lives in opcode_catalog.inc as declarative architectural data.
// X(pattern, mnemonic, format, group, type)
#define X(PATTERN, NAME, FORMAT, GROUP, TYPE) \
  OpcodeInfo{PATTERN, mask_for_format(InstructionFormat::FORMAT), NAME, \
             InstructionFormat::FORMAT, InstructionGroup::GROUP, InstructionType::TYPE},
static constexpr OpcodeInfo kCatalog[] = {
#include "opcode_catalog.inc"
};
#undef X

std::span<const OpcodeInfo> Decoder::opcode_catalog() noexcept {
  return kCatalog;
}

DecodedInstruction Decoder::decode(GuestAddress address,
                                   std::uint32_t word) const noexcept {
  const std::uint32_t primary = word >> 26;
  const OpcodeInfo* best = nullptr;
  unsigned best_specificity = 0;

  // Static recompilation is decode-time work, not runtime work. The simple
  // linear implementation is intentionally correctness-first; a generated
  // 64-bucket lookup can replace this without changing the public contract.
  for (const auto& op : kCatalog) {
    if ((op.pattern >> 26) != primary) continue;
    if ((word & op.mask) != (op.pattern & op.mask)) continue;
    const unsigned specificity = static_cast<unsigned>(std::popcount(op.mask));
    if (!best || specificity > best_specificity) {
      best = &op;
      best_specificity = specificity;
    }
  }
  return {address, word, best};
}

GuestAddress DecodedInstruction::direct_branch_target() const noexcept {
  if (!info) return 0;
  if (info->format == InstructionFormat::I) {
    const auto d = branch_i_displacement();
    return aa() ? static_cast<GuestAddress>(d)
                : static_cast<GuestAddress>(address + d);
  }
  if (info->format == InstructionFormat::B) {
    const auto d = branch_b_displacement();
    return aa() ? static_cast<GuestAddress>(d)
                : static_cast<GuestAddress>(address + d);
  }
  return 0;
}

}  // namespace xenon::cpu
