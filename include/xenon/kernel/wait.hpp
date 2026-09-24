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

// Wait for a single object to be signaled. waiting_thread_id identifies the
// calling guest thread and is only meaningful for ObjectType::Mutant (a
// mutant's ownership is thread-identity-scoped); pass the real calling
// thread's id whenever the caller has one (e.g. ExportCallContext::thread_id
// from a KeWaitForSingleObject/NtWaitForSingleObject export handler) rather
// than the default of 0, which acquire()s the mutant as "owned by no
// specific thread" and is only correct for callers that generically don't
// know their own thread identity.
[[nodiscard]] WaitResult wait_for_single_object(
    const std::shared_ptr<KernelObject>& object,
    std::chrono::milliseconds timeout,
    std::uint32_t waiting_thread_id = 0);

// Wait for multiple objects. See wait_for_single_object() for
// waiting_thread_id.
[[nodiscard]] WaitResult wait_for_multiple_objects(
    std::span<const std::shared_ptr<KernelObject>> objects,
    bool wait_all,
    std::chrono::milliseconds timeout,
    std::uint32_t* signaled_index = nullptr,
    std::uint32_t waiting_thread_id = 0);

// Helper to check if an object is waitable
[[nodiscard]] bool is_waitable_object(ObjectType type);

// Helper to wait on specific object types. See wait_for_single_object() for
// waiting_thread_id.
[[nodiscard]] bool wait_on_object(KernelObject& object,
                                  std::chrono::milliseconds timeout,
                                  std::uint32_t waiting_thread_id = 0);

}  // namespace xenon::kernel
