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
  std::unique_ptr<std::thread> host_thread_;
  ThreadState state_{ThreadState::Initialized};
  ThreadPriority priority_{ThreadPriority::Normal};
  std::uint32_t processor_affinity_{0xFFFFFFFF};
  std::uint32_t exit_code_{0};
  std::atomic<std::uint32_t> suspend_count_{0};

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
