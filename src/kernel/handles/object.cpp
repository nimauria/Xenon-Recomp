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

void KernelObject::note_signal(std::uint32_t guest_thread_id, std::uint32_t guest_caller,
                               const char* source) const noexcept {
  signal_thread_.store(guest_thread_id, std::memory_order_relaxed);
  signal_caller_.store(guest_caller, std::memory_order_relaxed);
  signal_source_.store(source, std::memory_order_relaxed);
  signal_time_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count(),
                        std::memory_order_relaxed);
  signal_count_.fetch_add(1, std::memory_order_release);
}

KernelObject::SignalNote KernelObject::last_signal() const noexcept {
  SignalNote note{};
  note.count = signal_count_.load(std::memory_order_acquire);
  note.guest_thread_id = signal_thread_.load(std::memory_order_relaxed);
  note.guest_caller = signal_caller_.load(std::memory_order_relaxed);
  note.host_time_ns = signal_time_ns_.load(std::memory_order_relaxed);
  note.source = signal_source_.load(std::memory_order_relaxed);
  return note;
}

}  // namespace xenon::kernel
