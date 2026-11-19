// Guest-memory segmentation and windows (XEN-001): every access of up to
// kSlackBytes lies inside the descriptor its segment selects, on devices
// whose storage-buffer range is anywhere from the Vulkan minimum (128 MiB) up;
// windows cover their footprint, stay aligned and inside the aperture, and
// rebase addresses exactly.

#include "xenon/gpu/guest_memory_segments.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace xenon::gpu;

namespace {

constexpr std::uint64_t kMiB = 1024u * 1024u;
constexpr std::uint64_t kAperture = 512u * kMiB;

void check_covers_every_access(const GuestMemorySegmentation& plan, std::uint64_t max_range) {
  for (std::uint32_t segment = 0; segment < plan.count; ++segment)
    assert(plan.range(segment) <= max_range && plan.range(segment) > 0u);
  // Addresses around every boundary and spread through the aperture.
  for (std::uint64_t address = 0; address < kAperture; address += 7919u * 4096u + 4u) {
    for (std::uint64_t width : {4u, 16u, 64u, 256u}) {
      if (address + width > kAperture) continue;
      const auto segment = static_cast<std::uint32_t>(address >> plan.shift);
      assert(segment < plan.count);
      assert(address >= plan.offset(segment));
      assert(address + width <= plan.offset(segment) + plan.range(segment));
    }
  }
  for (std::uint32_t segment = 1; segment < plan.count; ++segment) {
    const auto boundary = plan.offset(segment);
    for (std::uint64_t address = boundary - 256u; address < boundary; address += 4u) {
      const auto owner = static_cast<std::uint32_t>(address >> plan.shift);
      assert(owner == segment - 1u);
      assert(address + 256u <= plan.offset(owner) + plan.range(owner));
    }
  }
}

}  // namespace

int main() {
  // Devices that span the aperture keep the single binding.
  // One element type: kMiB is unsigned long on Linux, unsigned long long with
  // MSVC, and a braced list of mixed types deduces nothing.
  for (const std::uint64_t range : {std::uint64_t{kAperture}, std::uint64_t{4096u * kMiB},
                                    std::uint64_t{~0ull}}) {
    const auto plan = plan_guest_memory_segments(range, kAperture);
    assert(plan && !plan->segmented() && plan->count == 1u && plan->shift == 0u);
    assert(plan->range(0) == kAperture);
  }

  // The Vulkan minimum: eight 64 MiB segments, each with its slack.
  const auto minimum = plan_guest_memory_segments(128u * kMiB, kAperture);
  assert(minimum && minimum->segmented());
  assert(minimum->count == 8u && minimum->segment_bytes == 64u * kMiB && minimum->shift == 26u);
  assert(minimum->range(0) == 64u * kMiB + GuestMemorySegmentation::kSlackBytes);
  assert(minimum->range(7) == 64u * kMiB);  // the last ends at the aperture
  check_covers_every_access(*minimum, 128u * kMiB);

  // A range just short of a power of two still leaves room for the slack.
  const auto tight = plan_guest_memory_segments(256u * kMiB, kAperture);
  assert(tight && tight->count == 4u && tight->segment_bytes == 128u * kMiB);
  check_covers_every_access(*tight, 256u * kMiB);
  const auto odd = plan_guest_memory_segments(300u * kMiB, kAperture);
  assert(odd && odd->count == 2u && odd->segment_bytes == 256u * kMiB);
  check_covers_every_access(*odd, 300u * kMiB);

  // Below what eight segments can cover: refused, never silently truncated.
  assert(!plan_guest_memory_segments(64u * kMiB, kAperture));
  assert(!plan_guest_memory_segments(256u, kAperture));
  assert(!plan_guest_memory_segments(0u, kAperture));

  // Windows: unrestricted devices bind from 0, as before.
  const auto whole = plan_guest_memory_window(300u * kMiB, 301u * kMiB, ~0ull, 64u, kAperture);
  assert(whole && whole->offset == 0u && whole->range == kAperture);
  assert(whole->rebase(0x12345678u) == 0x12345678u);
  // Restricted: aligned onto the footprint.
  const auto low = plan_guest_memory_window(0x1000123u, 0x1200000u, 128u * kMiB, 256u, kAperture);
  assert(low && low->offset == 0x1000100u && low->range == 128u * kMiB);
  assert(low->offset % 256u == 0u && low->offset <= 0x1000123u);
  assert(0x1200000u <= low->offset + low->range);
  // Near the top: pulled down so a full window fits the aperture.
  const auto top = plan_guest_memory_window(500u * kMiB, 501u * kMiB, 128u * kMiB, 64u, kAperture);
  assert(top && top->offset == kAperture - 128u * kMiB && top->offset + top->range <= kAperture);
  // Rebasing is exact for every address of the footprint, also when the base
  // lies below the window (a resolve's tiles may start past its base).
  const std::uint32_t base = 0x10000000u;
  const auto past = plan_guest_memory_window(base + 0x40000u, base + 0x80000u, 128u * kMiB, 256u,
                                             kAperture);
  assert(past && past->offset > base);
  for (std::uint32_t offset = 0x40000u; offset < 0x80000u; offset += 0x1000u)
    assert(past->rebase(base) + offset == base + offset - static_cast<std::uint32_t>(past->offset));
  // A footprint larger than any window, a reversed or outside range, or a
  // non-power-of-two alignment: refused.
  assert(!plan_guest_memory_window(0u, 129u * kMiB, 128u * kMiB, 256u, kAperture));
  assert(!plan_guest_memory_window(10u, 5u, 128u * kMiB, 256u, kAperture));
  assert(!plan_guest_memory_window(0u, kAperture + 1u, 128u * kMiB, 256u, kAperture));
  assert(!plan_guest_memory_window(0u, 16u, 128u * kMiB, 48u, kAperture));

  std::cout << "xenon_guest_memory_segments_tests: ok\n";
  return 0;
}
