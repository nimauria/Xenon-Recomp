#include "recomp/analysis/analysis_internal.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

// Generic static vtable/function-pointer recovery. We scan non-executable image
// sections for runs of aligned 32-bit big-endian pointers into executable
// ranges. A short run is accepted only when at least two targets independently
// look like compiler function entries (or are already known starts); long runs
// are themselves strong table structure. This deliberately recovers ENTRY
// facts, not hard function boundaries - PointerTable is a weak semantic source
// and may later be absorbed as an alternate block entry.
PointerTableScanResult scan_static_pointer_tables(
    const xbox::XexImage& image, const ExecutableRangeIndex& range_index,
    const std::map<GuestAddress, DiscoverySource>& known_starts) {
  PointerTableScanResult out;
  cpu::Decoder decoder;

  const auto credible_target = [&](GuestAddress target) {
    if (known_starts.contains(target)) return true;
    for (const auto& metadata : image.function_metadata)
      if (metadata.valid && metadata.begin == target) return true;
    const auto* section = executable_section(range_index, target);
    if (!section || target < section->virtual_address) return false;
    const auto offset = static_cast<std::size_t>(target - section->virtual_address);
    if (offset + 4u > section->bytes.size()) return false;
    const auto instruction = decoder.decode(target, be32(section->bytes, offset));
    return looks_like_function_prologue(instruction);
  };

  for (const auto& section : image.sections) {
    if (section.executable || section.bytes.size() < 12u) continue;
    std::size_t offset = 0;
    while (offset + 4u <= section.bytes.size()) {
      std::vector<GuestAddress> run;
      std::size_t cursor = offset;
      while (cursor + 4u <= section.bytes.size()) {
        const auto target = be32(section.bytes, cursor);
        if (!ExecutableRangeIndex::is_aligned_ppc_address(target) ||
            !range_index.is_executable_address(target))
          break;
        run.push_back(target);
        cursor += 4u;
      }

      if (run.size() >= 3u) {
        // Repeated copies of one pointer are common constants and are not a
        // function table. Require structural diversity as well as executable
        // pointer shape. Short tables need two independently credible unique
        // entries; long tables may establish structure through three unique
        // executable entries even if only some have obvious prologues.
        std::set<GuestAddress> unique_targets(run.begin(), run.end());
        std::set<GuestAddress> credible_unique;
        for (const auto target : unique_targets)
          if (credible_target(target)) credible_unique.insert(target);
        if (unique_targets.size() >= 2u &&
            (credible_unique.size() >= 2u ||
             (run.size() >= 6u && unique_targets.size() >= 3u))) {
          ++out.tables;
          out.targets.insert(unique_targets.begin(), unique_targets.end());
        }
      }
      offset = cursor > offset ? cursor : offset + 4u;
    }
  }
  return out;
}

}  // namespace xenon::recomp::detail
