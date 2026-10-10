#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

#include "core/session/session_internal.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

bool XenonSession::start_gpu_pump_thread() {
  if (!gpu_ || !graphics_system_ || !kernel_process_ || !memory_ || !loaded_xex_) {
    return false;
  }
  if (!gpu_pump_callback_stack_base_ || !gpu_pump_callback_stack_size_) {
    set_error("GPU pump callback stack was not allocated");
    return false;
  }

  // A dedicated KPCR + static-TLS block for the GPU pump thread, independent
  // of main_thread_tls_/audio_thread_tls_ - real Xbox 360 vsync interrupt
  // dispatch runs on its own kernel context, never the game's main thread's.
  std::string tls_error;
  if (!setup_guest_thread_tls_context(*memory_, loaded_xex_->image.tls,
                                      gpu_pump_callback_stack_base_,
                                      gpu_pump_callback_stack_size_,
                                      gpu_pump_thread_tls_, &tls_error)) {
    set_error("Failed to set up GPU pump thread TLS: " + tls_error);
    return false;
  }

  kernel::ThreadCreationParams thread_params{};
  thread_params.stack_size = gpu_pump_callback_stack_size_;
  thread_params.name = "GpuPumpThread";
  gpu_pump_thread_ = kernel_process_->thread_manager().create_thread(
      [this]() -> std::uint32_t { return run_gpu_pump_thread(); }, thread_params);
  if (!gpu_pump_thread_) {
    set_error("Failed to create GPU pump KernelThread");
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
    gpu_pump_thread_tls_ = {};
    return false;
  }
  write_guest_thread_id(*memory_, gpu_pump_thread_tls_, gpu_pump_thread_->thread_id());
  gpu_pump_thread_->set_guest_kthread_address(gpu_pump_thread_tls_.kthread_address);
  logging::append_probe_log("thread_identity_diag.log", "gpu_pump_thread_ assigned thread_id=%u\n", gpu_pump_thread_->thread_id());
  if (main_thread_) {
    logging::append_probe_log("thread_identity_diag.log", "main_thread_ thread_id=%u\n", main_thread_->thread_id());
  }
#if defined(XENON_HAS_AUDIO)
  if (audio_thread_) {
    logging::append_probe_log("thread_identity_diag.log", "audio_thread_ thread_id=%u\n", audio_thread_->thread_id());
  }
#endif

  // Mark the pump active before starting the thread, so there is no window
  // where the thread is running but gpu_pump_running_ has not been observed
  // true yet.
  gpu_pump_running_.store(true);

  if (!gpu_pump_thread_->start()) {
    set_error("Failed to start GPU pump KernelThread");
    gpu_pump_running_.store(false);
    gpu_pump_thread_.reset();
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
    gpu_pump_thread_tls_ = {};
    return false;
  }
  return true;
}

void XenonSession::stop_gpu_pump_thread() noexcept {
  gpu_pump_running_.store(false);
  if (gpu_pump_thread_) {
    static_cast<void>(gpu_pump_thread_->join());
    gpu_pump_thread_.reset();
  }
}

std::uint32_t XenonSession::run_gpu_pump_thread() {
  // Registers this host thread as gpu_pump_thread_ for anything that
  // resolves "current thread" via kernel::ThreadManager (thread_local) - the
  // same mechanism run_execution()/run_audio_callback_thread() use - so a
  // guest xboxkrnl export invoked from inside the vsync interrupt callback
  // sees the pump thread's own identity.
  if (kernel_process_ && gpu_pump_thread_) {
    kernel_process_->thread_manager().set_current_thread(gpu_pump_thread_);
  }

  using Clock = std::chrono::steady_clock;
  // Short poll interval so ring-buffer drain latency stays low; a separate
  // accumulator (next_vsync) triggers vsync-rate work (present + interrupt)
  // at ~60Hz regardless of how often this tighter loop actually wakes.
  constexpr auto kPollInterval = std::chrono::milliseconds(3);
  constexpr auto kVsyncInterval = std::chrono::nanoseconds(16'666'667);  // ~60Hz
  auto next_vsync = Clock::now() + kVsyncInterval;

  while (gpu_pump_running_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(kPollInterval);
    if (!gpu_pump_running_.load(std::memory_order_relaxed)) break;
    if (!graphics_system_ || !gpu_ || !kernel_process_) continue;

    const auto _tick_phase_start = Clock::now();
    const auto ring = kernel_process_->gpu_ring_buffer();
    if (ring.configured() && ring.write_index != ring.read_index) {
      try {
        const auto new_read_index = graphics_system_->submit_ring(
            ring.base_address, ring.capacity_dwords, ring.read_index,
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

    // Drain whatever graphics IR is now pending every tick, even when
    // submit_ring() above produced nothing new this time - IR decoded by a
    // previous tick's partial submission can still be waiting to be played
    // into the backend (see GraphicsSystem::execute_ir()).
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

    const auto now = Clock::now();
    if (now < next_vsync) continue;
    // Catch up rather than let a slow tick cause a burst of back-to-back
    // vsyncs once it finally returns.
    do {
      next_vsync += kVsyncInterval;
    } while (next_vsync <= now);

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
      static_cast<void>(graphics_system_->present(*gpu_, frame));
      const auto _present_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - _present_start)
                                    .count();
      if (_present_ms >= 20) {
        static std::atomic<int> _present_diag_count{0};
        if (_present_diag_count.fetch_add(1) < 40) {
          logging::append_probe_log("tick_phase_diag.log", "slow present: %lldms\n", (long long)_present_ms);
        }
      }
    }

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
    detail::sample_title_vsync_probes(*memory_, interrupt.context);

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
  if (!callback || !memory_ || !compiled_registry_binder_ || !kernel_process_ ||
      !gpu_pump_callback_stack_base_ || !gpu_pump_callback_stack_size_) {
    return false;
  }

  cpu::CpuState state{};
  state.cia = callback;
  state.gpr[1] = static_cast<std::uint64_t>(gpu_pump_callback_stack_base_) +
                 gpu_pump_callback_stack_size_ - 64u;
  // Real VdSetGraphicsInterruptCallback ABI, verified against xenia-project/
  // xenia's xboxkrnl_video.cc (callback registration comment: "r3 = bool
  // 0/1 - 0 is normal interrupt, 1 is some acquire/lock") and
  // gpu::GraphicsSystem::MarkVblank(), which dispatches the real vsync case
  // with DispatchInterruptCallback(0, ...) - Xenon's pump thread only ever
  // fires the normal vsync interrupt, so r3 is always 0 here. r4 is the
  // guest-supplied context pointer from the registration call
  // (VdSetGraphicsInterruptCallback's own second argument).
  state.gpr[3] = source;
  state.gpr[4] = context;
  state.gpr[13] = gpu_pump_thread_tls_.kpcr_address;
  // The title reads its current hardware-thread number from r13+0x10C (its interrupt
  // handler clears this CPU's bit in the GPU fence word). Xenia runs the callback with
  // the thread's active CPU set to the interrupt's target, defaulting to CPU 2.
  try {
    memory_->write8(gpu_pump_thread_tls_.kpcr_address + GuestKpcrLayout::kCurrentCpuOffset,
                    static_cast<std::uint8_t>(cpu));
  } catch (const memory::MemoryFault&) {
    return false;
  }

  return run_guest_callback(state, callback, gpu_pump_thread_);
}

}  // namespace xenon::core
