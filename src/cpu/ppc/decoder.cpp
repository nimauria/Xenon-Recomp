// decoder.cpp
#include "xenon/cpu/decoder.hpp"

#include <array>
#include <bit>
#include <cstddef>

namespace xenon::cpu {
namespace {

struct RawOpcodeInfo {
  std::uint32_t pattern{};
  std::uint32_t mask{};
  std::string_view mnemonic{};
  InstructionFormat format{};
  InstructionGroup group{};
  InstructionType type{};
};

// The catalog lives in opcode_catalog.inc as declarative architectural data.
// X(pattern, mnemonic, format, group, type)
#define X(PATTERN, NAME, FORMAT, GROUP, TYPE) \
  RawOpcodeInfo{PATTERN, mask_for_format(InstructionFormat::FORMAT), NAME, \
                InstructionFormat::FORMAT, InstructionGroup::GROUP, \
                InstructionType::TYPE},
static constexpr RawOpcodeInfo kRawCatalog[] = {
#include "opcode_catalog.inc"
};
#undef X

constexpr std::size_t kCatalogSize = std::size(kRawCatalog);

constexpr std::uint32_t opcode_id_for(std::string_view mnemonic) noexcept {
  // Stable generated identity: changing catalogue order does not change IDs.
  // FNV-1a provides a fast, deterministic identifier for typed frontend passes.
  std::uint32_t hash = 2166136261u;
  for (const unsigned char c : mnemonic) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash ? hash : 1u;
}

consteval auto make_catalog() {
  std::array<OpcodeInfo, kCatalogSize> catalog{};
  for (std::size_t i = 0; i < kCatalogSize; ++i) {
    const auto& source = kRawCatalog[i];
    catalog[i] = OpcodeInfo{
        OpcodeId{opcode_id_for(source.mnemonic)}, source.pattern,
        source.mask, source.mnemonic, source.format, source.group, source.type};
  }
  return catalog;
}

static constexpr auto kCatalog = make_catalog();

struct DecodeBucket {
  std::uint16_t begin{};
  std::uint16_t count{};
};

struct DecodeIndex {
  std::array<std::uint16_t, kCatalogSize> indices{};
  std::array<DecodeBucket, 64> buckets{};
};

consteval DecodeIndex make_decode_index() {
  DecodeIndex index{};
  std::size_t cursor = 0;
  for (std::size_t primary = 0; primary < index.buckets.size(); ++primary) {
    index.buckets[primary].begin = static_cast<std::uint16_t>(cursor);
    
    // Group instructions by their primary 6-bit opcode
    for (std::size_t i = 0; i < kCatalog.size(); ++i) {
      if ((kCatalog[i].pattern >> 26u) == primary) {
        index.indices[cursor++] = static_cast<std::uint16_t>(i);
      }
    }
    index.buckets[primary].count = static_cast<std::uint16_t>(
        cursor - index.buckets[primary].begin);

    // Sort indices within the bucket by mask popcount (specificity) descending.
    // This optimization guarantees that the most specific mask is evaluated first,
    // allowing the runtime decoder to early-exit on the first valid match without
    // checking the entire bucket.
    for (std::size_t i = 0; i < index.buckets[primary].count; ++i) {
      for (std::size_t j = i + 1; j < index.buckets[primary].count; ++j) {
        const auto idx_i = index.indices[index.buckets[primary].begin + i];
        const auto idx_j = index.indices[index.buckets[primary].begin + j];
        if (std::popcount(kCatalog[idx_j].mask) > std::popcount(kCatalog[idx_i].mask)) {
          index.indices[index.buckets[primary].begin + i] = idx_j;
          index.indices[index.buckets[primary].begin + j] = idx_i;
        }
      }
    }
  }
  return index;
}

static constexpr auto kDecodeIndex = make_decode_index();

[[nodiscard]] constexpr bool patterns_overlap(const OpcodeInfo& a,
                                              const OpcodeInfo& b) noexcept {
  const auto common = a.mask & b.mask;
  return (a.pattern & common) == (b.pattern & common);
}

}  // namespace

std::span<const OpcodeInfo> Decoder::opcode_catalog() noexcept {
  return kCatalog;
}

const OpcodeInfo* Decoder::opcode_info(OpcodeId id) noexcept {
  if (!id.valid()) return nullptr;
  for (const auto& opcode : kCatalog) {
    if (opcode.id == id) return &opcode;
  }
  return nullptr;
}

OpcodeCatalogValidation Decoder::validate_catalog() noexcept {
  OpcodeCatalogValidation result{};
  result.entry_count = kCatalog.size();
  for (std::size_t i = 0; i < kCatalog.size(); ++i) {
    if (!kCatalog[i].id.valid()) ++result.duplicate_ids;

    for (std::size_t j = i + 1; j < kCatalog.size(); ++j) {
      if (kCatalog[i].id == kCatalog[j].id) ++result.duplicate_ids;
      const auto& a = kCatalog[i];
      const auto& b = kCatalog[j];
      
      // We only care about overlap if they share the same primary bucket
      if ((a.pattern >> 26u) != (b.pattern >> 26u)) continue;
      if (!patterns_overlap(a, b)) continue;

      // A more-specific mask is deterministic because the bucket is pre-sorted
      // by specificity. Equal specificity overlapping entries would make
      // catalog ordering decide the decode result, which is forbidden.
      if (std::popcount(a.mask) == std::popcount(b.mask)) {
        ++result.ambiguous_overlaps;
      }
    }
  }
  result.deterministic = result.duplicate_ids == 0u &&
                         result.ambiguous_overlaps == 0u;
  return result;
}

DecodedInstruction Decoder::decode(GuestAddress address,
                                   std::uint32_t word) const noexcept {
  const std::uint32_t primary = word >> 26u;
  const auto bucket = kDecodeIndex.buckets[primary];

  // Because make_decode_index sorts the bucket by mask specificity descending
  // at compile time, the first match we encounter is mathematically guaranteed
  // to be the most specific operation. The slow linear scan is eliminated.
  for (std::uint16_t n = 0; n < bucket.count; ++n) {
    const auto& op = kCatalog[kDecodeIndex.indices[bucket.begin + n]];
    if ((word & op.mask) == (op.pattern & op.mask)) {
      return {address, word, &op};
    }
  }
  return {address, word, nullptr};
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