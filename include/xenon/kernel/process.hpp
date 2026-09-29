#pragma once

#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xenon/kernel/heap.hpp"

#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/module.hpp"
#include "xenon/kernel/pool.hpp"
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

  // Kernel pool backing ExAllocatePool*/ExFreePool (see pool.hpp).
  [[nodiscard]] KernelPool& pool() noexcept { return pool_; }

  // FscGet/SetCacheElementCount: the file-system cache element count a title has
  // configured. Xenon's filesystem has no element-based cache to resize, so the
  // value is stored and read back (which is the whole guest-visible contract).
  [[nodiscard]] std::atomic<std::uint32_t>& fsc_cache_element_count() noexcept {
    return fsc_cache_element_count_;
  }
  [[nodiscard]] const KernelPool& pool() const noexcept { return pool_; }

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

  // KeTlsAlloc/KeTlsFree: a process-wide slot-allocation bitmap (real Win32
  // TlsAlloc/TlsFree semantics - allocation state is shared across every
  // thread in the process, even though each thread's slot *values* are
  // independent, stored in that KernelThread's own get_tls()/set_tls()
  // array). Bounded by KernelThread::tls_slot_count() (64), a documented
  // simplification of the real Xbox 360 kernel's 256-slot bitmap - AC6's own
  // boot-time allocation only ever needs a handful of low slot indices.
  // Returns kTlsOutOfIndexes (UINT32_MAX, verified against xenia-project/
  // xenia's X_TLS_OUT_OF_INDEXES) if every slot is already allocated.
  static constexpr std::uint32_t kTlsOutOfIndexes = 0xFFFFFFFFu;
  [[nodiscard]] std::uint32_t allocate_tls_slot() noexcept;
  void free_tls_slot(std::uint32_t slot) noexcept;

  // Environment variables (simplified)
  [[nodiscard]] std::string get_env(const std::string& name) const;
  void set_env(std::string name, std::string value);

  // Xenos GPU command-ring-buffer state, set up by the guest's VdInitializeRingBuffer/
  // VdEnableRingBufferRPtrWriteBack/VdSwap calls (see xboxkrnl_video_exports.cpp) and
  // drained by XenonSession's dedicated GPU pump thread. Guarded by one process-wide
  // mutex, mirroring tls_mutex_/tls_free_slots_ above: an export handler (any guest
  // thread) is the writer, the GPU pump thread is the reader, and neither ever holds
  // this lock across a blocking call.
  struct GpuRingBufferState {
    std::uint32_t base_address{0};
    std::uint32_t capacity_dwords{0};
    // Advanced by VdSwap (and any guest code that writes PM4 packets directly) each
    // time it appends commands; consumed by the GPU pump thread via
    // CommandProcessor::execute_ring(), which returns the new read position.
    std::uint32_t write_index{0};
    std::uint32_t read_index{0};
    // Guest address the pump thread writes the current read_index to after each
    // drain, matching real Xenos read-pointer write-back hardware semantics
    // (VdEnableRingBufferRPtrWriteBack). 0 = disabled/not configured.
    std::uint32_t rptr_writeback_address{0};
    [[nodiscard]] bool configured() const noexcept { return capacity_dwords != 0; }
  };
  [[nodiscard]] GpuRingBufferState gpu_ring_buffer() const noexcept;
  void configure_gpu_ring_buffer(std::uint32_t base_address, std::uint32_t capacity_dwords) noexcept;
  void set_gpu_ring_buffer_write_index(std::uint32_t write_index) noexcept;
  void set_gpu_ring_buffer_read_index(std::uint32_t read_index) noexcept;
  void set_gpu_ring_buffer_rptr_writeback(std::uint32_t address) noexcept;

  // Guest graphics-interrupt callback registered via VdSetGraphicsInterruptCallback -
  // invoked by the GPU pump thread at vsync (via a bespoke invoke_gpu_interrupt_callback()
  // helper mirroring XenonSession::invoke_audio_callback()). callback_address==0 means
  // no callback is registered (real Xbox 360 games may run briefly before registering
  // one during boot).
  struct GpuInterruptCallbackState {
    std::uint32_t callback_address{0};
    std::uint32_t context{0};
  };
  [[nodiscard]] GpuInterruptCallbackState gpu_interrupt_callback() const noexcept;
  void set_gpu_interrupt_callback(std::uint32_t callback_address, std::uint32_t context) noexcept;

  // Current scanout front buffer, set by VdSwap (xboxkrnl_video_exports.cpp)
  // each time the guest requests a flip, and read by the GPU pump thread at
  // vsync to build a gpu::resource_ir::TextureDescriptor for
  // GraphicsSystem::present() - deliberately plain fields rather than that
  // GPU-side type itself, so this kernel-layer header does not need to
  // depend on include/xenon/gpu/resource_ir.hpp. `format` is the raw Xenos
  // hardware texture-format code (same numeric space TextureDescriptor::
  // format uses) - see xboxkrnl_video_exports.cpp's doc comment on VdSwap
  // for which format codes are actually produced. `generation` increments on
  // every VdSwap call so the pump thread can tell a genuinely new frame from
  // "nothing changed since last vsync" if it chooses to (present()-ing the
  // same frame again every tick is also a valid, simpler choice).
  struct GpuFrontBufferState {
    std::uint32_t base_address{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t pitch{0};
    std::uint8_t format{0};
    std::uint64_t generation{0};
  };
  [[nodiscard]] GpuFrontBufferState gpu_front_buffer() const noexcept;
  void set_gpu_front_buffer(std::uint32_t base_address, std::uint32_t width,
                            std::uint32_t height, std::uint32_t pitch,
                            std::uint8_t format) noexcept;

 private:
  std::uint32_t process_id_;
  std::uint32_t exit_code_{0};
  // X_PROCTYPE_USER (1) - see process_type()'s doc comment.
  std::uint32_t process_type_{1};
  std::mutex critical_section_mutex_{};
  std::mutex tls_mutex_{};
  // One bit per slot; true = free. All 64 start free.
  std::bitset<64> tls_free_slots_{~static_cast<unsigned long long>(0)};
  std::shared_ptr<KernelMemory> memory_;
  GuestHeapManager guest_heap_;
  KernelPool pool_;
  std::atomic<std::uint32_t> fsc_cache_element_count_{0};
  ThreadManager thread_manager_;
  ModuleManager module_manager_;
  std::shared_ptr<KernelThread> main_thread_;
  HandleTable handle_table_{};
  TimerManager timer_manager_{};

  mutable std::mutex env_mutex_;
  std::unordered_map<std::string, std::string> environment_;

  mutable std::mutex gpu_mutex_;
  GpuRingBufferState gpu_ring_buffer_{};
  GpuInterruptCallbackState gpu_interrupt_callback_{};
  GpuFrontBufferState gpu_front_buffer_{};
};

}  // namespace xenon::kernel
