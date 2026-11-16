#pragma once

// The storage-buffer limits that decide how the guest-memory mirror is bound
// (xenon/gpu/guest_memory_segments.hpp, XEN-001).
//
// XENON_VULKAN_MAX_STORAGE_BUFFER_RANGE=<bytes> lowers the range below what
// the device reports, so the segmented and windowed bindings restricted
// devices need can be exercised on any GPU (tests run with 128 MiB, the
// specification's guaranteed minimum). It can only lower the limit.

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <vulkan/vulkan.h>

#include "xenon/gpu/guest_memory_segments.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::gpu::vulkan {

struct GuestMemoryLimits {
  VkDeviceSize max_storage_range{};
  VkDeviceSize storage_offset_alignment{1};
};

[[nodiscard]] inline GuestMemoryLimits query_guest_memory_limits(VkPhysicalDevice physical_device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physical_device, &properties);
  GuestMemoryLimits limits{};
  limits.max_storage_range = properties.limits.maxStorageBufferRange;
  limits.storage_offset_alignment =
      properties.limits.minStorageBufferOffsetAlignment
          ? properties.limits.minStorageBufferOffsetAlignment
          : 1u;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // read during device setup, never written
#endif
  const char* forced = std::getenv("XENON_VULKAN_MAX_STORAGE_BUFFER_RANGE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  if (forced && *forced) {
    const auto value = std::strtoull(forced, nullptr, 0);
    if (value && value < limits.max_storage_range) limits.max_storage_range = value;
  }
  return limits;
}

// A window over guest bytes [begin, end) for a pass bound through a dynamic
// storage-buffer descriptor of `limits.max_storage_range` bytes (capped to the
// aperture).
[[nodiscard]] inline std::optional<GuestMemoryWindow> guest_memory_window(
    const GuestMemoryLimits& limits, std::uint64_t begin, std::uint64_t end) {
  return plan_guest_memory_window(begin, end, limits.max_storage_range,
                                  limits.storage_offset_alignment,
                                  memory::kPhysicalMemorySize);
}

// The descriptor range such a pass binds.
[[nodiscard]] inline VkDeviceSize guest_memory_window_range(const GuestMemoryLimits& limits) {
  return limits.max_storage_range < memory::kPhysicalMemorySize ? limits.max_storage_range
                                                                : memory::kPhysicalMemorySize;
}

}  // namespace xenon::gpu::vulkan
