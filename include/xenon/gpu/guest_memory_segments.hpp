#pragma once

// Binding guest physical memory on hosts whose storage-buffer descriptors
// cannot span all of it (XEN-001).
//
// Shaders address the 512 MiB guest-memory mirror by physical address. A
// Vulkan storage-buffer descriptor may cover at most maxStorageBufferRange
// bytes, which the specification only guarantees to be 128 MiB (and some
// mobile, integrated and translation-layer drivers report exactly that).
// Binding the whole mirror there is invalid.
//
// Two backend-neutral plans cover every access pattern:
//
//  - Segments, for guest shaders (vertex fetch, memory export), which may
//    touch any address: the aperture is split into `count` power-of-two
//    segments, each bound as its own descriptor that extends kSlackBytes into
//    the next segment, so an access of up to kSlackBytes starting anywhere in
//    segment i lies wholly inside descriptor i. Shaders select the segment
//    from the address's high bits with a branch (no descriptor-array
//    indexing, which such devices often lack). One segment means the plain
//    single binding and unchanged shader text.
//
//  - Windows, for passes whose footprint the CPU knows (texture loads,
//    resolves, scan-out): one descriptor of at most the device range, placed
//    by a dynamic offset over the bytes the pass reads or writes, with the
//    pass's guest base rebased to the window.

#include <cstdint>
#include <optional>

namespace xenon::gpu {

struct GuestMemorySegmentation {
  // An access of up to this many bytes never straddles two descriptors.
  static constexpr std::uint32_t kSlackBytes = 256;
  static constexpr std::uint32_t kMaxSegments = 8;

  std::uint64_t aperture_bytes{};
  std::uint64_t segment_bytes{};
  std::uint32_t count{1};
  // log2(segment_bytes) when segmented, 0 when one binding covers the
  // aperture.
  std::uint8_t shift{};

  [[nodiscard]] bool segmented() const noexcept { return count > 1u; }
  [[nodiscard]] std::uint64_t offset(std::uint32_t segment) const noexcept {
    return std::uint64_t{segment} * segment_bytes;
  }
  // The bytes descriptor `segment` covers: its segment and the slack into the
  // next, never past the aperture.
  [[nodiscard]] std::uint64_t range(std::uint32_t segment) const noexcept;
};

// The segmentation for a device whose descriptors span at most
// `max_descriptor_range` bytes. Null when even kMaxSegments segments plus
// slack cannot fit - below 64 MiB + kSlackBytes for a 512 MiB aperture, which
// no conformant Vulkan device reports.
[[nodiscard]] std::optional<GuestMemorySegmentation> plan_guest_memory_segments(
    std::uint64_t max_descriptor_range, std::uint64_t aperture_bytes);

struct GuestMemoryWindow {
  std::uint64_t offset{};  // the descriptor's (dynamic) offset into the mirror
  std::uint64_t range{};   // the descriptor's range
  // The pass's guest base, rebased to the window (32-bit wrapping
  // arithmetic: base - offset, exact for every address in [begin, end)).
  [[nodiscard]] std::uint32_t rebase(std::uint32_t guest_address) const noexcept {
    return guest_address - static_cast<std::uint32_t>(offset);
  }
};

// A window of at most `max_descriptor_range` bytes covering guest bytes
// [begin, end), its offset aligned to `offset_alignment` (a power of two) and
// kept inside the aperture. Null when the footprint is larger than any window.
[[nodiscard]] std::optional<GuestMemoryWindow> plan_guest_memory_window(
    std::uint64_t begin, std::uint64_t end, std::uint64_t max_descriptor_range,
    std::uint64_t offset_alignment, std::uint64_t aperture_bytes);

}  // namespace xenon::gpu
