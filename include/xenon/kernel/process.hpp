#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xenon/kernel/heap.hpp"

#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/module.hpp"
#include "xenon/kernel/object.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/timer_manager.hpp"

namespace xenon::kernel {

// Minimal process state required by retail games
class KernelProcess final : public KernelObject {
 public:
  explicit KernelProcess(std::shared_ptr<KernelMemory> memory);

  [[nodiscard]] ThreadManager& thread_manager() { return thread_manager_; }
  [[nodiscard]] const ThreadManager& thread_manager() const { return thread_manager_; }

  [[nodiscard]] ModuleManager& module_manager() { return module_manager_; }
  [[nodiscard]] const ModuleManager& module_manager() const { return module_manager_; }

  [[nodiscard]] KernelMemory& memory() { return *memory_; }
  [[nodiscard]] const KernelMemory& memory() const { return *memory_; }

  [[nodiscard]] GuestHeapManager& guest_heap() noexcept { return guest_heap_; }
  [[nodiscard]] const GuestHeapManager& guest_heap() const noexcept { return guest_heap_; }

  // Shared dispatcher-object handle table: threads, events, semaphores,
  // mutants and timers created via the xboxkrnl thread/sync/timer exports
  // (ExCreateThread, NtCreateEvent, NtCreateSemaphore, NtCreateMutant, ...)
  // all publish their guest-visible Handle through this one table, matching
  // real Xbox 360 semantics where every kernel object - not just files -
  // shares one per-process handle namespace. KernelIoManager keeps its own
  // separate HandleTable for file objects (see io_manager.hpp) rather than
  // sharing this one, since it predates this table and its handle values are
  // already load-bearing for existing filesystem callers.
  [[nodiscard]] HandleTable& handle_table() noexcept { return handle_table_; }
  [[nodiscard]] const HandleTable& handle_table() const noexcept { return handle_table_; }

  // Real timer-dispatch thread backing KernelTimer objects with a nonzero
  // due time - see timer_manager.hpp. Owned per-process (like
  // thread_manager()/handle_table()) so it is torn down with the process
  // rather than needing separate lifecycle management.
  [[nodiscard]] TimerManager& timer_manager() noexcept { return timer_manager_; }

  [[nodiscard]] std::shared_ptr<KernelThread> main_thread() const;
  void set_main_thread(std::shared_ptr<KernelThread> thread);

  [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }
  [[nodiscard]] std::uint32_t exit_code() const noexcept;

  void set_exit_code(std::uint32_t exit_code);
  void terminate(std::uint32_t exit_code);

  // Real, mutable KeGetCurrentProcessType/KeSetCurrentProcessType state
  // (X_PROCTYPE_IDLE=0/X_PROCTYPE_USER=1/X_PROCTYPE_SYSTEM=2 - verified
  // against xenia-project/xenia's kernel_state, including its default of
  // X_PROCTYPE_USER for a running title process), not a hardcoded constant:
  // a title that calls Set then Get must observe its own change.
  [[nodiscard]] std::uint32_t process_type() const noexcept { return process_type_; }
  void set_process_type(std::uint32_t process_type) noexcept { process_type_ = process_type; }

  // Guards the guest-visible X_RTL_CRITICAL_SECTION lock_count/
  // recursion_count/owning_thread field updates (Rtl*CriticalSection
  // exports) across every critical section in this process - deliberately
  // one process-wide mutex rather than per-address atomic guest-memory
  // operations: it makes Enter/TryEnter/Leave's read-modify-write sequences
  // trivially race-free across Xenon's 1:1 guest-thread:host-thread model
  // without needing atomic compare-exchange primitives on guest memory
  // itself. Held only for the brief field read-modify-write, never across an
  // actual blocking wait - see xboxkrnl_rtl_critical_section_exports.cpp.
  [[nodiscard]] std::mutex& critical_section_mutex() noexcept { return critical_section_mutex_; }

  // Environment variables (simplified)
  [[nodiscard]] std::string get_env(const std::string& name) const;
  void set_env(std::string name, std::string value);

 private:
  std::uint32_t process_id_;
  std::uint32_t exit_code_{0};
  // X_PROCTYPE_USER (1) - see process_type()'s doc comment.
  std::uint32_t process_type_{1};
  std::mutex critical_section_mutex_{};
  std::shared_ptr<KernelMemory> memory_;
  GuestHeapManager guest_heap_;
  ThreadManager thread_manager_;
  ModuleManager module_manager_;
  std::shared_ptr<KernelThread> main_thread_;
  HandleTable handle_table_{};
  TimerManager timer_manager_{};

  mutable std::mutex env_mutex_;
  std::unordered_map<std::string, std::string> environment_;
};

}  // namespace xenon::kernel
