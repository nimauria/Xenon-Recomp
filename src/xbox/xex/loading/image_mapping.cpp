#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace xenon::xbox {

using namespace detail;

bool map_xex_image(memory::AddressSpace& memory, const XexImage& image, LoadedXex& out_loaded,
                   memory::GuestAddress preferred_base, std::string* error) {
  out_loaded = {};
  if (!memory.initialize()) {
    if (error) *error = "Memory V2 cannot initialize for XEX mapping.";
    return false;
  }

  memory::GuestAddress image_base = preferred_base;
  if (image.image_base != 0u) {
    image_base = static_cast<memory::GuestAddress>(image.image_base);
  }
  out_loaded.image_base = image_base;
  out_loaded.image = image;

  if (image.sections.empty()) {
    if (error) *error = "No PE or section data were extracted from this XEX image.";
    return false;
  }

  struct SectionMappingPlan {
    const XexSection* section{};
    memory::GuestAddress section_begin{};
    std::uint64_t section_end{};  // Exclusive, kept wide for overflow checks.
    memory::GuestAddress mapped_begin{};
    std::uint64_t mapped_end{};   // Exclusive.
    std::uint32_t page_size{};
  };

  struct AllocationPlan {
    memory::GuestAddress begin{};
    std::uint64_t end{};  // Exclusive.
    std::uint32_t page_size{};
  };

  std::vector<SectionMappingPlan> section_plans;
  section_plans.reserve(image.sections.size());

  const auto set_section_error = [&](std::string_view stage,
                                     const XexSection& section,
                                     memory::GuestAddress mapped_begin,
                                     std::uint64_t mapped_end,
                                     std::uint32_t page_size) {
    if (!error) return;
    std::ostringstream stream;
    stream << "Failed to " << stage << " PE section '"
           << (section.name.empty() ? "<unnamed>" : section.name)
           << "' in Xenon Memory V2: address=0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(section.virtual_address)
           << " virtual_size=0x" << section.virtual_size
           << " raw_size=0x" << section.raw_size
           << " mapped_base=0x" << mapped_begin
           << " mapped_size=0x" << (mapped_end >= mapped_begin ? mapped_end - mapped_begin : 0u)
           << " page_size=0x" << page_size << '.';
    *error = stream.str();
  };

  // Plan every section before reserving anything. Xbox PE sections are byte
  // ranges, while Memory V2 reserves the native Xbox allocation granularity
  // (64 KiB in the 0x8... XEX aperture, 4 KiB in the 0x9... aperture). Real
  // titles may therefore have distinct PE sections that share one or more
  // allocation pages. Those pages must be reserved once for the image, not
  // once per section.
  for (const auto& section : image.sections) {
    const std::uint64_t section_size = std::max<std::uint64_t>(
        std::max(section.virtual_size, section.raw_size), section.bytes.size());
    if (section_size == 0u) continue;

    const auto section_begin =
        static_cast<memory::GuestAddress>(section.virtual_address);
    const auto page_size = xex_page_size_for(section_begin);
    const std::uint64_t section_end =
        static_cast<std::uint64_t>(section_begin) + section_size;
    if (section_end > (std::uint64_t{1} << 32u) || section_end <= section_begin) {
      set_section_error("size", section, section_begin, section_end, page_size);
      return false;
    }

    const auto last_byte = static_cast<memory::GuestAddress>(section_end - 1u);
    if (xex_page_size_for(last_byte) != page_size) {
      set_section_error("map across incompatible XEX page regions for", section,
                        section_begin, section_end, page_size);
      return false;
    }

    const auto mapped_begin = align_down(section_begin, page_size);
    const auto mapped_end = align_up(section_end, page_size);
    if (mapped_end <= mapped_begin || mapped_end > (std::uint64_t{1} << 32u)) {
      set_section_error("align", section, mapped_begin, mapped_end, page_size);
      return false;
    }

    section_plans.push_back(
        {&section, section_begin, section_end, mapped_begin, mapped_end, page_size});
  }

  if (section_plans.empty()) {
    if (error) *error = "XEX image contains no non-empty PE sections to map.";
    return false;
  }

  std::vector<AllocationPlan> allocations;
  allocations.reserve(section_plans.size());
  std::vector<std::size_t> order(section_plans.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
    const auto& a = section_plans[lhs];
    const auto& b = section_plans[rhs];
    if (a.mapped_begin != b.mapped_begin) return a.mapped_begin < b.mapped_begin;
    if (a.mapped_end != b.mapped_end) return a.mapped_end < b.mapped_end;
    return a.page_size < b.page_size;
  });

  for (const auto index : order) {
    const auto& plan = section_plans[index];
    if (!allocations.empty() && allocations.back().page_size == plan.page_size &&
        static_cast<std::uint64_t>(plan.mapped_begin) <= allocations.back().end) {
      allocations.back().end = std::max(allocations.back().end, plan.mapped_end);
      continue;
    }
    allocations.push_back({plan.mapped_begin, plan.mapped_end, plan.page_size});
  }

  std::vector<memory::GuestAddress> mapped_allocation_bases;
  mapped_allocation_bases.reserve(allocations.size());

  const auto rollback_mappings = [&] {
    for (auto it = mapped_allocation_bases.rbegin();
         it != mapped_allocation_bases.rend(); ++it) {
      (void)memory.release(*it);
    }
    mapped_allocation_bases.clear();
    out_loaded.mapped_sections.clear();
    out_loaded.executable_ranges.clear();
    out_loaded.loaded = false;
  };

  const auto describe_allocation_failure = [&](std::string_view stage,
                                               const AllocationPlan& allocation) {
    if (!error) return;
    std::ostringstream stream;
    stream << "Failed to " << stage
           << " merged PE allocation in Xenon Memory V2: mapped_base=0x"
           << std::hex << std::uppercase << allocation.begin
           << " mapped_size=0x" << (allocation.end - allocation.begin)
           << " page_size=0x" << allocation.page_size << " sections=";
    bool first = true;
    for (const auto& plan : section_plans) {
      if (plan.mapped_end <= allocation.begin ||
          static_cast<std::uint64_t>(plan.mapped_begin) >= allocation.end) {
        continue;
      }
      if (!first) stream << ',';
      stream << '\'' << (plan.section->name.empty() ? "<unnamed>" : plan.section->name)
             << '\'';
      first = false;
    }
    stream << '.';
    *error = stream.str();
  };

  // Reserve/commit each merged allocation exactly once. Keep it writable only
  // while the PE bytes are copied; final guest protections are applied below.
  for (const auto& allocation : allocations) {
    const auto size64 = allocation.end - allocation.begin;
    if (size64 == 0u || size64 > std::numeric_limits<std::uint32_t>::max()) {
      describe_allocation_failure("size", allocation);
      rollback_mappings();
      return false;
    }
    const auto size = static_cast<std::uint32_t>(size64);
    if (!memory.reserve_fixed(allocation.begin, size, memory::kReadWrite)) {
      describe_allocation_failure("reserve", allocation);
      rollback_mappings();
      return false;
    }
    mapped_allocation_bases.push_back(allocation.begin);
    // PE virtual tails are required to start zeroed. Committing the merged
    // image ranges with zero initialization also makes bytes between sections
    // deterministic instead of exposing recycled physical RAM contents.
    if (!memory.commit_fixed(allocation.begin, size, memory::kReadWrite, true)) {
      describe_allocation_failure("commit", allocation);
      rollback_mappings();
      return false;
    }
  }

  // Copy each section at its real guest virtual address. Sharing allocation
  // pages is now harmless because the backing range was created once above.
  for (const auto& plan : section_plans) {
    const auto& section = *plan.section;
    if (!section.bytes.empty()) {
      try {
        memory.write_bytes(plan.section_begin, section.bytes);
      } catch (...) {
        set_section_error("write", section, plan.mapped_begin, plan.mapped_end,
                          plan.page_size);
        rollback_mappings();
        return false;
      }
    }
    out_loaded.mapped_sections.push_back(section);
  }

  // Native import records are intentionally left byte-for-byte as loaded here.
  // Type-0 records paired with a type-1 function thunk are metadata/address
  // slots, not function pointers that should be rewritten to the thunk. True
  // unpaired type-0 variable imports are bound later by XenonSession once the
  // active system-module variable registry exists.

  // Resolve the final protection for one Memory V2 native XEX page. XEX page
  // descriptors remain authoritative when present. If a title has no
  // descriptor coverage for that native page, PE-section protection is the
  // fallback. Multiple descriptor sub-pages are ORed because Memory V2's
  // 0x8... aperture intentionally exposes 64 KiB protection granularity.
  const auto protection_for_native_page = [&](memory::GuestAddress page_begin,
                                              std::uint32_t native_page_size) {
    memory::Protect descriptor_protect = memory::Protect::None;
    bool descriptor_covered = false;
    if (!image.security.page_descriptors.empty()) {
      const auto descriptor_page_size =
          (image.security.image_flags & 0x10000000u) != 0u
              ? memory::kBasePageSize
              : memory::kLargePageSize;
      const std::uint64_t page_end =
          static_cast<std::uint64_t>(page_begin) + native_page_size;
      for (std::uint64_t address = page_begin; address < page_end;
           address += descriptor_page_size) {
        const auto protect = protect_for_address(
            image.security, static_cast<std::uint32_t>(address));
        if (protect != memory::Protect::None) {
          descriptor_covered = true;
          descriptor_protect |= protect;
        }
      }
    }
    if (descriptor_covered) return descriptor_protect;

    memory::Protect section_protect = memory::Protect::None;
    const std::uint64_t page_end =
        static_cast<std::uint64_t>(page_begin) + native_page_size;
    for (const auto& plan : section_plans) {
      if (plan.section_end <= page_begin ||
          static_cast<std::uint64_t>(plan.section_begin) >= page_end) {
        continue;
      }
      section_protect |= plan.section->protect;
    }
    return section_protect;
  };

  out_loaded.executable_ranges.clear();
  for (const auto& allocation : allocations) {
    for (std::uint64_t page64 = allocation.begin; page64 < allocation.end;
         page64 += allocation.page_size) {
      const auto page = static_cast<memory::GuestAddress>(page64);
      const auto final_protect = protection_for_native_page(page, allocation.page_size);
      if (!memory.protect(page, allocation.page_size, final_protect)) {
        if (error) {
          std::ostringstream stream;
          stream << "Failed to apply final XEX page protection in Memory V2: address=0x"
                 << std::hex << std::uppercase << page
                 << " size=0x" << allocation.page_size
                 << " protect=0x" << static_cast<unsigned>(final_protect) << '.';
          *error = stream.str();
        }
        rollback_mappings();
        return false;
      }

      if (memory::has(final_protect, memory::Protect::Execute)) {
        if (!out_loaded.executable_ranges.empty()) {
          auto& previous = out_loaded.executable_ranges.back();
          if (previous.end == page && previous.page_size == allocation.page_size &&
              previous.protect == final_protect) {
            previous.end = page + allocation.page_size;
            continue;
          }
        }
        out_loaded.executable_ranges.push_back(
            {page, page + allocation.page_size, allocation.page_size, final_protect});
      }
    }
  }

  out_loaded.loaded = true;
  out_loaded.error.clear();
  return true;
}

bool load_xex(memory::AddressSpace& memory, std::span<const std::byte> file_bytes, LoadedXex& out_loaded,
             memory::GuestAddress preferred_base, std::string* error) {
  XexImage image{};
  if (!parse_xex_image(file_bytes, image, error)) {
    out_loaded = {};
    return false;
  }
  return map_xex_image(memory, image, out_loaded, preferred_base, error);
}

}  // namespace xenon::xbox
