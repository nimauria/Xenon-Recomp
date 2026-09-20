#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "xenon/kernel/io_types.hpp"
#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

enum class WaitResult : std::uint32_t {
  Success = 0,
  Timeout = 0x00000102,
  Abandoned = 0x00000080,
  Failed = 0xFFFFFFFF,
};

// Wait for a single object to be signaled
[[nodiscard]] WaitResult wait_for_single_object(
    const std::shared_ptr<KernelObject>& object,
    std::chrono::milliseconds timeout);

// Wait for multiple objects
[[nodiscard]] WaitResult wait_for_multiple_objects(
    std::span<const std::shared_ptr<KernelObject>> objects,
    bool wait_all,
    std::chrono::milliseconds timeout,
    std::uint32_t* signaled_index = nullptr);

// Helper to check if an object is waitable
[[nodiscard]] bool is_waitable_object(ObjectType type);

// Helper to wait on specific object types
[[nodiscard]] bool wait_on_object(KernelObject& object, std::chrono::milliseconds timeout);

}  // namespace xenon::kernel
