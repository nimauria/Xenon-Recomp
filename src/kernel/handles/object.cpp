#include "xenon/kernel/object.hpp"

#include <atomic>

namespace xenon::kernel {
namespace {

std::atomic<std::uint64_t> g_next_object_id{1};

}  // namespace

KernelObject::KernelObject(ObjectType type)
    : type_(type), object_id_(g_next_object_id.fetch_add(1, std::memory_order_relaxed)) {}

void KernelObject::retain_handle() noexcept {
  handle_count_.fetch_add(1, std::memory_order_acq_rel);
}

void KernelObject::release_handle() noexcept {
  const auto previous = handle_count_.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 1) on_last_handle_closed();
}

}  // namespace xenon::kernel
