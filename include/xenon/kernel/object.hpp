#pragma once

#include <atomic>
#include <cstdint>

#include "xenon/kernel/io_types.hpp"

namespace xenon::kernel {

class HandleTable;

namespace detail {
// Multi-object wait access to waitable objects' private state; defined in
// src/kernel/synchronization/wait.cpp.
struct WaitAccess;
}  // namespace detail

class KernelObject {
 public:
  virtual ~KernelObject() = default;

  KernelObject(const KernelObject&) = delete;
  KernelObject& operator=(const KernelObject&) = delete;

  [[nodiscard]] ObjectType type() const noexcept { return type_; }
  [[nodiscard]] std::uint64_t object_id() const noexcept { return object_id_; }
  [[nodiscard]] std::uint32_t handle_count() const noexcept {
    return handle_count_.load(std::memory_order_acquire);
  }

  // The most recent guest signal of this object, kept on the object so a
  // stalled waiter's report can name who last released it however long ago
  // that was (the diagnostic event ring is bounded and rotates). Fields are
  // individually atomic; a reader racing a signal may see a mix of two.
  struct SignalNote {
    std::uint64_t count{};  // signals noted so far; 0 = never
    std::uint32_t guest_thread_id{};
    std::uint32_t guest_caller{};
    std::int64_t host_time_ns{};  // steady_clock
    const char* source{};         // static storage, such as "NtSetEvent"
  };
  void note_signal(std::uint32_t guest_thread_id, std::uint32_t guest_caller,
                   const char* source) const noexcept;
  [[nodiscard]] SignalNote last_signal() const noexcept;

 protected:
  explicit KernelObject(ObjectType type);

  virtual void on_last_handle_closed() noexcept {}

 private:
  friend class HandleTable;

  void retain_handle() noexcept;
  void release_handle() noexcept;

  ObjectType type_{ObjectType::Unknown};
  std::uint64_t object_id_{};
  std::atomic<std::uint32_t> handle_count_{0};
  mutable std::atomic<std::uint64_t> signal_count_{0};
  mutable std::atomic<std::uint32_t> signal_thread_{0};
  mutable std::atomic<std::uint32_t> signal_caller_{0};
  mutable std::atomic<std::int64_t> signal_time_ns_{0};
  mutable std::atomic<const char*> signal_source_{nullptr};
};

}  // namespace xenon::kernel
