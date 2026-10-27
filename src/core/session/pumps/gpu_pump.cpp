#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

#include "core/session/session_internal.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

namespace {

// Creates a KernelThread with its own KPCR and static TLS on `stack`, named
// `name`, running `entry`. Returns false (with *error set) on failure, having
// released what it set up.
bool create_gpu_guest_thread(memory::AddressSpace& memory,
                             const std::optional<xbox::XexTls>& tls_info,
                             kernel::KernelProcess& process, memory::GuestAddress stack_base,
                             std::uint32_t stack_size, const char* name,
                             std::function<std::uint32_t()> entry,
                             GuestThreadTlsContext& tls,
                             std::shared_ptr<kernel::KernelThread>& thread, std::string* error) {
  if (!setup_guest_thread_tls_context(memory, tls_info, stack_base, stack_size, tls, error)) {
    return false;
  }
  kernel::ThreadCreationParams params{};
  params.stack_size = stack_size;
  params.name = name;
  thread = process.thread_manager().create_thread(std::move(entry), params);
  if (!thread) {
    if (error) *error = std::string("Failed to create ") + name + " KernelThread";
    release_guest_thread_tls_context(memory, tls);
    tls = {};
    return false;
  }
  write_guest_thread_id(memory, tls, thread->thread_id());
  thread->set_guest_kthread_address(tls.kthread_address);
  thread->set_guest_kpcr_address(tls.kpcr_address);
  return true;
}

}  // namespace

bool XenonSession::start_gpu_pump_thread() {
  if (!gpu_ || !graphics_system_ || !kernel_process_ || !memory_ || !loaded_xex_) {
    return false;
  }
  if (!gpu_pump_callback_stack_base_ || !gpu_pump_callback_stack_size_ ||
      !gpu_vsync_callback_stack_base_ || !gpu_vsync_callback_stack_size_) {
    set_error("GPU pump callback stacks were not allocated");
    return false;
  }

  // The GPU worker (ring, rendering, presentation, PM4 interrupts) and the
  // vsync thread each get their own KPCR + static-TLS block, independent of
  // main_thread_tls_/audio_thread_tls_ - real Xbox 360 interrupt dispatch
  // runs on its own kernel context, never the game's main thread's.
  std::string error;
  if (!create_gpu_guest_thread(*memory_, loaded_xex_->image.tls, *kernel_process_,
                               gpu_pump_callback_stack_base_, gpu_pump_callback_stack_size_,
                               "GpuPumpThread", [this] { return run_gpu_pump_thread(); },
                               gpu_pump_thread_tls_, gpu_pump_thread_, &error)) {
    set_error("Failed to set up the GPU pump thread: " + error);
    return false;
  }
  if (!create_gpu_guest_thread(*memory_, loaded_xex_->image.tls, *kernel_process_,
                               gpu_vsync_callback_stack_base_, gpu_vsync_callback_stack_size_,
                               "GpuVsyncThread", [this] { return run_gpu_vsync_thread(); },
                               gpu_vsync_thread_tls_, gpu_vsync_thread_, &error)) {
    set_error("Failed to set up the GPU vsync thread: " + error);
    gpu_pump_thread_.reset();
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
    gpu_pump_thread_tls_ = {};
    return false;
  }
  logging::append_probe_log("thread_identity_diag.log",
                            "gpu_pump_thread_ thread_id=%u gpu_vsync_thread_ thread_id=%u\n",
                            gpu_pump_thread_->thread_id(), gpu_vsync_thread_->thread_id());
  if (main_thread_) {
    logging::append_probe_log("thread_identity_diag.log", "main_thread_ thread_id=%u\n",
                              main_thread_->thread_id());
  }
#if defined(XENON_HAS_AUDIO)
  if (audio_thread_) {
    logging::append_probe_log("thread_identity_diag.log", "audio_thread_ thread_id=%u\n",
                              audio_thread_->thread_id());
  }
#endif

  // Mark the pump active before starting the threads, so there is no window
  // where a thread is running but gpu_pump_running_ has not been observed
  // true yet. Ring work published from now on wakes the worker.
  gpu_pump_running_.store(true);
  kernel_process_->set_gpu_work_notifier([this] { gpu_wake_.notify(kGpuWakeRingWork); });

  if (!gpu_pump_thread_->start() || !gpu_vsync_thread_->start()) {
    set_error("Failed to start the GPU pump threads");
    stop_gpu_pump_thread();
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
    release_guest_thread_tls_context(*memory_, gpu_vsync_thread_tls_);
    gpu_pump_thread_tls_ = {};
    gpu_vsync_thread_tls_ = {};
    return false;
  }
  return true;
}

void XenonSession::stop_gpu_pump_thread() noexcept {
  gpu_pump_running_.store(false);
  if (kernel_process_) kernel_process_->set_gpu_work_notifier({});
  gpu_wake_.notify(kGpuWakeStop);
  // Terminating ends a wait an interrupt callback may be blocked in; both are
  // terminated before either is joined, since one may be waiting for the
  // other's callback to release the interrupt lock.
  if (gpu_vsync_thread_) static_cast<void>(gpu_vsync_thread_->terminate(0));
  if (gpu_pump_thread_) static_cast<void>(gpu_pump_thread_->terminate(0));
  if (gpu_vsync_thread_) {
    static_cast<void>(gpu_vsync_thread_->join());
    gpu_vsync_thread_.reset();
  }
  if (gpu_pump_thread_) {
    static_cast<void>(gpu_pump_thread_->join());
    gpu_pump_thread_.reset();
  }
}

std::uint32_t XenonSession::run_gpu_pump_thread() {
  // Registers this host thread as gpu_pump_thread_ for anything that
  // resolves "current thread" via kernel::ThreadManager (thread_local) - the
  // same mechanism run_execution()/run_audio_callback_thread() use - so a
  // guest xboxkrnl export invoked from inside a PM4 interrupt callback sees
  // the pump thread's own identity.
  if (kernel_process_ && gpu_pump_thread_) {
    kernel_process_->thread_manager().set_current_thread(gpu_pump_thread_);
  }

  using Clock = std::chrono::steady_clock;
  // The worker sleeps until the guest publishes ring work, the vsync thread
  // asks for a frame, or the session stops. Two cases still poll: a drain
  // parked on WAIT_REG_MEM waits for memory a guest CPU store may change,
  // which raises no notification; and a bounded idle fallback.
  constexpr auto kParkedWaitPoll = std::chrono::milliseconds(1);
  constexpr auto kIdleFallback = std::chrono::milliseconds(50);

  while (gpu_pump_running_.load(std::memory_order_relaxed)) {
    const bool parked =
        graphics_system_ && graphics_system_->command_processor().parked_on_wait();
    const auto reasons =
        gpu_wake_.wait_until(Clock::now() + (parked ? kParkedWaitPoll : kIdleFallback));
    if (!gpu_pump_running_.load(std::memory_order_relaxed)) break;
    if (!graphics_system_ || !gpu_ || !kernel_process_) continue;

    const auto _tick_phase_start = Clock::now();
    const auto ring = kernel_process_->gpu_ring_buffer();
    if (ring.configured() && ring.write_index != ring.read_index) {
      try {
        // Executes the drained work in order with its fences and interrupts,
        // so the read pointer below is written back only once the commands
        // before it, and the ring words they used, are done with.
        const auto new_read_index = graphics_system_->submit_ring(
            *gpu_, ring.base_address, ring.capacity_dwords, ring.read_index,
            ring.write_index);
        kernel_process_->set_gpu_ring_buffer_read_index(new_read_index);
        // Real Xenos read-pointer write-back hardware semantics
        // (VdEnableRingBufferRPtrWriteBack): mirror the freshly-consumed
        // read index into guest memory so the guest can poll its own copy.
        // VdEnableRingBufferRPtrWriteBack hands the kernel a PHYSICAL address (as
        // does VdInitializeRingBuffer), so write it as physical memory: treating
        // it as a guest virtual address faulted on the free page every drain and
        // the title never saw the read pointer advance.
        if (ring.rptr_writeback_address != 0 && memory_) {
          const std::array<std::byte, 4> big_endian{
              static_cast<std::byte>(new_read_index >> 24), static_cast<std::byte>(new_read_index >> 16),
              static_cast<std::byte>(new_read_index >> 8), static_cast<std::byte>(new_read_index)};
          if (!memory_->write_physical(ring.rptr_writeback_address, big_endian) &&
              config_.enable_logging) {
            std::scoped_lock console_log_lock(console_log_mutex());
            std::cout << "[XenonSession] GPU pump: read-pointer writeback to physical 0x" << std::hex
                      << ring.rptr_writeback_address << std::dec << " has no RAM backing" << std::endl;
          }
        }
      } catch (const std::exception& ex) {
        // Malformed/guest-corrupted ring content must not kill the pump
        // thread - drop this drain attempt (read_index stays where it was;
        // the next tick will retry from there) rather than crash or silently
        // pretend nothing happened.
        if (config_.enable_logging) {
          std::scoped_lock console_log_lock(console_log_mutex());
          std::cout << "[XenonSession] GPU pump: ring buffer decode failed: "
                    << ex.what() << std::endl;
        }
      }
    }

    const auto _after_submit_ring = Clock::now();

    // Graphics IR a failed drain left decoded is still played into the
    // backend (see GraphicsSystem::execute_ir()).
    try {
      graphics_system_->execute_ir(*gpu_);
    } catch (const std::exception& ex) {
      if (config_.enable_logging) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] GPU pump: IR execution failed: " << ex.what()
                  << std::endl;
      }
    }

    {
      const auto _after_execute_ir = Clock::now();
      const auto _submit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   _after_submit_ring - _tick_phase_start)
                                   .count();
      const auto _ir_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               _after_execute_ir - _after_submit_ring)
                               .count();
      if (_submit_ms >= 20 || _ir_ms >= 20) {
        static std::atomic<int> _tick_phase_diag_count{0};
        if (_tick_phase_diag_count.fetch_add(1) < 40) {
          logging::append_probe_log("tick_phase_diag.log", "slow tick phase: submit_ring=%lldms execute_ir=%lldms\n",
                       (long long)_submit_ms, (long long)_ir_ms);
        }
      }
    }

    if ((reasons & kGpuWakePresent) == 0u) continue;
    const auto front_buffer = kernel_process_->gpu_front_buffer();
    if (front_buffer.base_address != 0) {
      gpu::TextureDescriptor texture{};
      texture.base_address = front_buffer.base_address;
      texture.width = front_buffer.width;
      texture.height = front_buffer.height;
      texture.pitch = front_buffer.pitch;
      texture.format = front_buffer.format;
      gpu::PresentationFrame frame{};
      frame.texture = texture;
      frame.visible_width = front_buffer.width;
      frame.visible_height = front_buffer.height;
      const auto _present_start = Clock::now();
      const auto status = graphics_system_->present(*gpu_, frame);
      gpu_presents_.fetch_add(1u, std::memory_order_relaxed);
      const auto status_index = static_cast<std::size_t>(status);
      if (status_index < gpu_present_results_.size())
        gpu_present_results_[status_index].fetch_add(1u, std::memory_order_relaxed);
      const auto _present_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - _present_start)
                                    .count();
      if (_present_ms >= 20) {
        static std::atomic<int> _present_diag_count{0};
        if (_present_diag_count.fetch_add(1) < 40) {
          logging::append_probe_log("tick_phase_diag.log", "slow present: %lldms\n",
                                    (long long)_present_ms);
        }
      }
    }
  }
  return 0;
}

std::uint32_t XenonSession::run_gpu_vsync_thread() {
  if (kernel_process_ && gpu_vsync_thread_) {
    kernel_process_->thread_manager().set_current_thread(gpu_vsync_thread_);
  }

  using Clock = std::chrono::steady_clock;
  constexpr auto kVsyncInterval = std::chrono::nanoseconds(16'666'667);  // ~60Hz
  auto next_vsync = Clock::now() + kVsyncInterval;

  while (gpu_pump_running_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_until(next_vsync);
    if (!gpu_pump_running_.load(std::memory_order_relaxed)) break;
    if (!kernel_process_) continue;
    // Catch up rather than deliver a burst of back-to-back vsyncs after a
    // callback that ran long.
    const auto now = Clock::now();
    do {
      next_vsync += kVsyncInterval;
    } while (next_vsync <= now);

    // The worker presents the current front buffer at its next wake.
    gpu_wake_.notify(kGpuWakePresent);

    const auto interrupt = kernel_process_->gpu_interrupt_callback();
    {
      static std::atomic<int> _vsync_diag_count{0};
      const int _n = _vsync_diag_count.fetch_add(1) + 1;
      if (_n <= 5 || (_n % 300) == 0) {
        logging::append_probe_log("vsync_tick_diag.log", "vsync tick #%d: interrupt_callback_address=0x%08X front_buffer_base=0x%08X\n",
                     _n, interrupt.callback_address, kernel_process_->gpu_front_buffer().base_address);
      }
    }

    // Title-specific investigation probes sampled once per vsync.
    if (memory_) detail::sample_title_vsync_probes(*memory_, interrupt.context);

    detail::snapshot_thread_export_history(export_trace_);
    if (interrupt.callback_address != 0) {
      {
        static std::atomic<int> _vsync_cb_diag_count{0};
        const int _n = _vsync_cb_diag_count.fetch_add(1) + 1;
        if (_n <= 10) {
          logging::append_probe_log("vsync_cb_diag.log", "vsync callback dispatch #%d: BEFORE invoke_gpu_interrupt_callback(0x%08X)\n",
                       _n, interrupt.callback_address);
        }
      }
      const auto _vsync_cb_start = Clock::now();
      const bool _vsync_cb_ok = invoke_gpu_interrupt_callback(interrupt.callback_address, interrupt.context);
      const auto _vsync_cb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - _vsync_cb_start)
                                     .count();
      {
        static std::atomic<int> _vsync_cb_after_diag_count{0};
        const int _n = _vsync_cb_after_diag_count.fetch_add(1) + 1;
        if (_n <= 10 || _vsync_cb_ms >= 20) {
          logging::append_probe_log("vsync_cb_diag.log", "vsync callback dispatch #%d: AFTER invoke_gpu_interrupt_callback ok=%d elapsed=%lldms\n",
                       _n, _vsync_cb_ok ? 1 : 0, (long long)_vsync_cb_ms);
        }
      }
      if (_vsync_cb_ok) {
        gpu_interrupts_delivered_.fetch_add(1u, std::memory_order_relaxed);
      } else if (gpu_interrupts_failed_.fetch_add(1u, std::memory_order_relaxed) == 0u &&
                 config_.enable_logging) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] GPU vsync interrupt callback 0x" << std::hex
                  << interrupt.callback_address << std::dec
                  << " failed to run (first failure; later ones are only counted)" << std::endl;
      }
    }
  }
  return 0;
}

bool XenonSession::invoke_gpu_interrupt_callback(cpu::GuestAddress callback,
                                                 cpu::GuestAddress context,
                                                 std::uint32_t source,
                                                 std::uint32_t cpu) {
  if (!callback || !memory_ || !compiled_registry_binder_ || !kernel_process_) return false;
  // The calling thread's own guest context: the vsync thread's for a vsync,
  // the worker's for a PM4_INTERRUPT it decoded.
  const bool on_vsync_thread =
      gpu_vsync_thread_ && kernel::KernelThread::current() == gpu_vsync_thread_.get();
  const auto& thread = on_vsync_thread ? gpu_vsync_thread_ : gpu_pump_thread_;
  const auto& tls = on_vsync_thread ? gpu_vsync_thread_tls_ : gpu_pump_thread_tls_;
  const auto stack_base =
      on_vsync_thread ? gpu_vsync_callback_stack_base_ : gpu_pump_callback_stack_base_;
  const auto stack_size =
      on_vsync_thread ? gpu_vsync_callback_stack_size_ : gpu_pump_callback_stack_size_;
  auto& watch = on_vsync_thread ? gpu_vsync_callback_watch_ : gpu_callback_watch_;
  if (!stack_base || !stack_size) return false;

  cpu::CpuState state{};
  state.cia = callback;
  state.gpr[1] = static_cast<std::uint64_t>(stack_base) + stack_size - 64u;
  // Real VdSetGraphicsInterruptCallback ABI, verified against xenia-project/
  // xenia's xboxkrnl_video.cc (callback registration comment: "r3 = bool
  // 0/1 - 0 is normal interrupt, 1 is some acquire/lock") and
  // gpu::GraphicsSystem::MarkVblank(), which dispatches the real vsync case
  // with DispatchInterruptCallback(0, ...). r4 is the guest-supplied context
  // pointer from the registration call (VdSetGraphicsInterruptCallback's own
  // second argument).
  state.gpr[3] = source;
  state.gpr[4] = context;
  state.gpr[13] = tls.kpcr_address;

  // One graphics interrupt at a time (see gpu_interrupt_mutex_).
  std::scoped_lock interrupt_lock(gpu_interrupt_mutex_);
  // The title reads its current hardware-thread number from r13+0x10C (its interrupt
  // handler clears this CPU's bit in the GPU fence word). Xenia runs the callback with
  // the thread's active CPU set to the interrupt's target, defaulting to CPU 2.
  try {
    memory_->write8(tls.kpcr_address + GuestKpcrLayout::kCurrentCpuOffset,
                    static_cast<std::uint8_t>(cpu));
  } catch (const memory::MemoryFault&) {
    return false;
  }

  return run_guest_callback(state, callback, thread, watch,
                            source == 0u ? "vsync interrupt" : "graphics interrupt");
}

}  // namespace xenon::core
