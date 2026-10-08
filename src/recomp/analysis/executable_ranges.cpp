#include <algorithm>

#include "recomp/analysis/analysis_internal.hpp"

namespace xenon::recomp {

namespace detail {

const xbox::XexSection* executable_section(const ExecutableRangeIndex& index, GuestAddress address) {
  const auto* section = index.containing_section(address);
  return (section != nullptr && section->executable) ? section : nullptr;
}

}  // namespace detail

ExecutableRangeIndex::ExecutableRangeIndex(const xbox::XexImage& image) {
  entries_.reserve(image.sections.size());
  for (const auto& section : image.sections) {
    const auto size = std::max(section.virtual_size, section.raw_size);
    if (size == 0) continue;
    const auto begin = section.virtual_address;
    const auto end = std::min<std::uint64_t>(static_cast<std::uint64_t>(begin) + size, 0xFFFFFFFFull);
    entries_.push_back(Entry{begin, static_cast<std::uint32_t>(end), &section});
  }
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) { return a.begin < b.begin; });
}

const xbox::XexSection* ExecutableRangeIndex::containing_section(std::uint32_t address) const noexcept {
  // Binary search for the last entry whose `begin` is <= address, then a
  // direct range check against its `end` - O(log section_count) instead of
  // the previous O(section_count) linear scan (Part 7). Real XEX images have
  // few, non-overlapping sections, so this matters less for correctness than
  // for call volume: this lookup runs many times per candidate address
  // across a 10k-function analysis.
  auto it = std::upper_bound(entries_.begin(), entries_.end(), address,
                             [](std::uint32_t value, const Entry& entry) { return value < entry.begin; });
  if (it == entries_.begin()) return nullptr;
  --it;
  return (address >= it->begin && address < it->end) ? it->section : nullptr;
}

bool ExecutableRangeIndex::is_executable_address(std::uint32_t address) const noexcept {
  const auto* section = containing_section(address);
  return section != nullptr && section->executable;
}

bool ExecutableRangeIndex::is_mapped_address(std::uint32_t address) const noexcept {
  return containing_section(address) != nullptr;
}

bool ExecutableRangeIndex::is_aligned_ppc_address(std::uint32_t address) noexcept {
  return (address & 3u) == 0u;
}

}  // namespace xenon::recomp
