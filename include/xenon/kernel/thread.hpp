#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

enum class ThreadPriority : std::int32_t {
  Idle = 0,
  Lowest = 1,
  BelowNormal = 2,
  Normal = 3,
  AboveNormal = 4,
  Highest = 5,
  TimeCritical = 6,
};

enum class ThreadState : std::uint8_t {
  Initialized = 0,
  Ready,
  Running,
  Suspended,
  Terminated,
};

struct ThreadCreationParams {
  std::uint32_t stack_size{0};
  std::uint32_t creation_flags{0};
  ThreadPriority priority{ThreadPriority::Normal};
  std::uint32_t processor_affinity{0xFFFFFFFF};  // All processors by default
  std::string name{};
  // Mirrors real Xbox 360/Win32 CREATE_SUSPENDED: when true, the host thread
  // is spawned by start() but parks before ever calling entry_() until
  // resume() is called (or the thread is terminated while still parked).
  bool create_suspended{false};
};

using ThreadEntry = std::function<std::uint32_t()>;

class KernelThread final : public KernelObject {
 public:
  explicit KernelThread(ThreadEntry entry, const ThreadCreationParams& params);
  ~KernelThread() override;

  [[nodiscard]] std::uint32_t thread_id() const noexcept { return thread_id_; }
  [[nodiscard]] ThreadState state() const noexcept;
  [[nodiscard]] ThreadPriority priority() const noexcept;
  [[nodiscard]] std::uint32_t exit_code() const noexcept;
  [[nodiscard]] std::uint32_t processor_affinity() const noexcept;
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  void set_priority(ThreadPriority priority) noexcept;
  void set_processor_affinity(std::uint32_t affinity) noexcept;
  void set_name(std::string name);

  [[nodiscard]] bool start();
  [[nodiscard]] bool suspend();
  [[nodiscard]] bool resume();
  [[nodiscard]] bool terminate(std::uint32_t exit_code);

  // Preemptive safepoint primitives (Phase 2 of the AC6 Runtime Readiness
  // pass). A guest dispatch loop (see XenonSession::dispatch_guest_thread())
  // calls these at every compiled-block boundary (not per-instruction - that
  // would violate the "cheap in release" constraint) so suspend()/
  // terminate() are honored within roughly one block's latency for ANY
  // running guest thread, not only one blocked in a wait/sleep of its own.

  // Blocks the calling thread if currently suspended, returning once
  // resumed or terminated; a cheap no-op (single relaxed atomic load, no
  // lock) when not suspended, so calling this on every dispatch-loop
  // iteration does not add lock contention to the common case. Also used by
  // thread_main() itself to honor CREATE_SUSPENDED at entry - both share the
  // same suspend_count_/suspend_condition_ mechanism.
  void wait_while_suspended();

  // Cheap (lock-free), for the same per-iteration safepoint use: true once
  // terminate() has been called or entry_() has returned naturally. Prefer
  // this over state() == ThreadState::Terminated on a hot path - state()
  // takes mutex_ on every call, this does not.
  [[nodiscard]] bool is_terminated() const noexcept {
    return terminated_.load(std::memory_order_acquire);
  }

  // Waits up to timeout_ms for the host thread to finish running entry_()
  // (or, for a still-parked create_suspended thread, to be terminated
  // without ever running it) - a real, honored timeout via a completion
  // future set exactly once by thread_main(), not the previous stub that
  // always blocked unconditionally regardless of the requested timeout.
  // 0xFFFFFFFF waits forever, matching the existing INFINITE convention used
  // elsewhere in this codebase (see kernel::WaitResult/wait.cpp).
  [[nodiscard]] bool join(std::uint32_t timeout_ms = 0xFFFFFFFF);

  // TLS support
  [[nodiscard]] std::optional<std::uint64_t> get_tls(std::uint32_t slot) const;
  [[nodiscard]] bool set_tls(std::uint32_t slot, std::uint64_t value);

  // Thread-local kernel state
  void* kernel_data() const noexcept { return kernel_data_; }
  void set_kernel_data(void* data) noexcept { kernel_data_ = data; }

 private:
  void thread_main();

  ThreadEntry entry_;
  std::string name_;
  std::uint32_t thread_id_;
  std::uint32_t stack_size_;
  std::uint32_t creation_flags_;
  void* kernel_data_{nullptr};

  mutable std::mutex mutex_;
  // Guards the create_suspended entry-time park in thread_main(): resume()
  // and terminate() both notify it so a parked thread wakes up promptly
  // instead of only being observed the next time something happens to poll
  // suspend_count_.
  std::condition_variable suspend_condition_;
  std::unique_ptr<std::thread> host_thread_;
  ThreadState state_{ThreadState::Initialized};
  ThreadPriority priority_{ThreadPriority::Normal};
  std::uint32_t processor_affinity_{0xFFFFFFFF};
  std::uint32_t exit_code_{0};
  std::atomic<std::uint32_t> suspend_count_{0};
  // Lock-free mirror of state_ == ThreadState::Terminated, for
  // is_terminated()'s cheap hot-path safepoint check. Set under mutex_
  // alongside every state_ = ThreadState::Terminated transition (terminate()
  // and thread_main()'s natural-completion path).
  std::atomic<bool> terminated_{false};
  const bool create_suspended_{false};
  // Set exactly once, on every thread_main() exit path (entry_() returned
  // naturally, or the thread was terminated before ever running entry_()),
  // right before thread_main() returns. join() waits on the shared_future
  // this produces instead of unconditionally blocking on host_thread_
  // itself, so a timeout is real and multiple concurrent waiters (e.g. two
  // guest threads both calling NtWaitForSingleObjectEx on the same thread
  // handle) are both well-defined or use it.
  std::promise<void> completion_promise_;
  std::shared_future<void> completion_future_;

  // TLS storage - Xbox 360 supports 64 TLS slots
  static constexpr std::uint32_t kTlsSlotCount = 64;
  std::array<std::uint64_t, kTlsSlotCount> tls_storage_{};
};

// Thread manager for the runtime
class ThreadManager {
 public:
  ThreadManager() = default;
  ~ThreadManager();

  ThreadManager(const ThreadManager&) = delete;
  ThreadManager& operator=(const ThreadManager&) = delete;

  [[nodiscard]] std::shared_ptr<KernelThread> create_thread(
      ThreadEntry entry, const ThreadCreationParams& params);
  [[nodiscard]] std::shared_ptr<KernelThread> get_thread(std::uint32_t thread_id) const;
  [[nodiscard]] std::shared_ptr<KernelThread> current_thread() const;

  void set_current_thread(std::shared_ptr<KernelThread> thread);
  void remove_thread(std::uint32_t thread_id);
  void shutdown();

  [[nodiscard]] std::vector<std::shared_ptr<KernelThread>> enumerate_threads() const;
  [[nodiscard]] std::size_t thread_count() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::uint32_t, std::shared_ptr<KernelThread>> threads_;
  std::uint32_t next_thread_id_{1};

  // Thread-local storage for current thread
  static thread_local std::shared_ptr<KernelThread> current_thread_;
};

}  // namespace xenon::kernel
