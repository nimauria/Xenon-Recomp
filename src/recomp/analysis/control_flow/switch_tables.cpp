#include <sstream>

#include "recomp/analysis/analysis_internal.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

// Resolves a SwitchTableHint's target addresses (Part 1.6/1.11): explicit
// targets always win when supplied; otherwise decodes `entry_count` entries
// of `entry_format` starting at `table_address` directly from the image's
// bytes, rather than relying solely on the nearby-aligned-word heuristic
// difficult commercial binaries defeat. Skips (with a warning, not silently)
// any decoded entry that does not land in an executable section - a
// malformed/misdescribed hint must be visible, never swallowed.
std::vector<std::uint32_t> resolve_switch_targets(const analysis::SwitchTableHint& table,
                                                   const ExecutableRangeIndex& index,
                                                   std::vector<std::string>& warnings) {
  if (!table.explicit_targets.empty()) return table.explicit_targets;
  std::vector<std::uint32_t> targets;
  if (!table.table_address || !table.entry_count) return targets;
  // Jump tables normally live in a read-only data section rather than the
  // executable one - look up the table's containing section directly
  // (Part 7's indexed lookup already covers both executable and
  // non-executable sections in one query, so no separate fallback scan is
  // needed here the way the old image.sections linear scan required).
  const auto* data_section = index.containing_section(*table.table_address);
  if (!data_section) {
    warnings.push_back("switch table hint at 0x" +
                       [&] { std::ostringstream s; s << std::hex << table.site; return s.str(); }() +
                       ": table_address is not inside any known section");
    return targets;
  }
  const std::size_t entry_size = table.entry_format == analysis::SwitchEntryFormat::RelativeInt16 ? 2u : 4u;
  const auto base_offset =
      static_cast<std::size_t>(*table.table_address - data_section->virtual_address);
  for (std::uint32_t i = 0; i < *table.entry_count; ++i) {
    const auto offset = base_offset + static_cast<std::size_t>(i) * entry_size;
    if (offset + entry_size > data_section->bytes.size()) break;
    std::uint32_t target = 0;
    switch (table.entry_format) {
      case analysis::SwitchEntryFormat::AbsoluteWord32:
        target = be32(data_section->bytes, offset);
        break;
      case analysis::SwitchEntryFormat::RelativeWord32:
        target = *table.table_address +
                 static_cast<std::int32_t>(be32(data_section->bytes, offset));
        break;
      case analysis::SwitchEntryFormat::RelativeInt16: {
        const auto raw = static_cast<std::uint16_t>(
            (std::to_integer<std::uint32_t>(data_section->bytes[offset]) << 8) |
            std::to_integer<std::uint32_t>(data_section->bytes[offset + 1]));
        target = *table.table_address + static_cast<std::int16_t>(raw);
        break;
      }
    }
    if (!index.is_executable_address(target)) {
      warnings.push_back("switch table hint at 0x" +
                         [&] { std::ostringstream s; s << std::hex << table.site; return s.str(); }() +
                         ": decoded entry 0x" +
                         [&] { std::ostringstream s; s << std::hex << target; return s.str(); }() +
                         " is not executable, skipped");
      continue;
    }
    targets.push_back(target);
  }
  return targets;
}

}  // namespace xenon::recomp::detail
