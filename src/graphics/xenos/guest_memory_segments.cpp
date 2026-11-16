#include "xenon/gpu/guest_memory_segments.hpp"

#include <algorithm>
#include <bit>

namespace xenon::gpu {

std::uint64_t GuestMemorySegmentation::range(std::uint32_t segment) const noexcept {
  const auto start = offset(segment);
  if (start >= aperture_bytes) return 0u;
  if (!segmented()) return aperture_bytes;
  return (std::min)(segment_bytes + kSlackBytes, aperture_bytes - start);
}

std::optional<GuestMemorySegmentation> plan_guest_memory_segments(
    std::uint64_t max_descriptor_range, std::uint64_t aperture_bytes) {
  if (!aperture_bytes) return std::nullopt;
  GuestMemorySegmentation plan{};
  plan.aperture_bytes = aperture_bytes;
  if (max_descriptor_range >= aperture_bytes) {
    plan.segment_bytes = aperture_bytes;
    return plan;
  }
  if (max_descriptor_range <= GuestMemorySegmentation::kSlackBytes) return std::nullopt;
  // The largest power of two that still leaves room for the slack.
  const auto segment =
      std::bit_floor(max_descriptor_range - GuestMemorySegmentation::kSlackBytes);
  const auto count = (aperture_bytes + segment - 1u) / segment;
  if (count > GuestMemorySegmentation::kMaxSegments) return std::nullopt;
  plan.segment_bytes = segment;
  plan.count = static_cast<std::uint32_t>(count);
  plan.shift = static_cast<std::uint8_t>(std::countr_zero(segment));
  return plan;
}

std::optional<GuestMemoryWindow> plan_guest_memory_window(std::uint64_t begin, std::uint64_t end,
                                                          std::uint64_t max_descriptor_range,
                                                          std::uint64_t offset_alignment,
                                                          std::uint64_t aperture_bytes) {
  if (end < begin || end > aperture_bytes || !std::has_single_bit(offset_alignment))
    return std::nullopt;
  const auto range = (std::min)(max_descriptor_range, aperture_bytes);
  // Aligned down onto the footprint, but no further than the aperture allows
  // a full-range window to start.
  auto offset = begin & ~(offset_alignment - 1u);
  const auto last_start = (aperture_bytes - range) & ~(offset_alignment - 1u);
  offset = (std::min)(offset, last_start);
  if (end > offset + range) return std::nullopt;
  return GuestMemoryWindow{offset, range};
}

}  // namespace xenon::gpu
