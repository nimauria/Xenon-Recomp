#include <iostream>
#include <mutex>

#include "xenon/core/session.hpp"
#include "xenon/xam/content_graph.hpp"

#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

void XenonSession::shutdown() {
  if (state() == SessionState::Uninitialized) {
    return;
  }

  set_state(SessionState::Stopping, "Shutting down session...");

  // Ask the guest execution thread to stop and wait for it. There is no
  // preemption for already-running native compiled code; a caller that needs
  // a hard timeout should terminate the hosting process instead of blocking
  // here indefinitely.
  const auto shutdown_step = [this](const char* step) {
    if (!config_.enable_logging) return;
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] shutdown: " << step << std::endl;
  };
  shutdown_step("joining the main guest thread");
  stop_requested_.store(true);
  if (main_thread_) {
    static_cast<void>(main_thread_->join());
  }
  // The poll thread reads guest memory, so it must be gone before memory_ is.
  stop_memory_watch_poll();
  shutdown_step("stopping the GPU pump thread");

#if defined(XENON_HAS_AUDIO)
  // Stop the guest-callback pump and join the audio callback's own
  // KernelThread before unloading the title's compiled registry or
  // destroying kernel_process_/memory_ below - see Part 4.8's "no leaked
  // audio worker" requirement. Order matters: stop_guest_callback_pump()
  // must run before audio_thread_->join() (the pump loop only exits once it
  // observes callback_running_ false), and both must happen before
  // kernel_process_.reset() (which would otherwise try to join the same
  // thread again from ThreadManager::shutdown() while nothing is left
  // draining its work).
  if (audio_) {
    audio_->stop_guest_callback_pump();
  }
  if (audio_thread_) {
    static_cast<void>(audio_thread_->join());
    audio_thread_.reset();
  }
  if (memory_) {
    release_guest_thread_tls_context(*memory_, audio_thread_tls_);
  }
  audio_thread_tls_ = {};
  if (audio_) {
    audio_->shutdown();
    audio_.reset();
  }
  if (memory_ && audio_callback_stack_base_) {
    (void)memory_->release(audio_callback_stack_base_);
    audio_callback_stack_base_ = 0;
    audio_callback_stack_size_ = 0;
  }
#endif

  // Stop the GPU pump thread before unloading the compiled registry or
  // destroying kernel_process_/gpu_/graphics_system_/memory_ below - same
  // ordering rationale as the audio callback thread above: it must be
  // stopped and joined before kernel_process_.reset() (which would otherwise
  // try to join the same KernelThread again from ThreadManager::shutdown()
  // while nothing is left draining its work) and before gpu_.reset()/
  // graphics_system_.reset() (the pump thread is the only thread that ever
  // touches those objects after start-up, and it must not be mid-drain or
  // mid-present when they are destroyed - see the class-level doc comment on
  // start_gpu_pump_thread() for why this must be one thread, not two).
  stop_gpu_pump_thread();
  if (memory_) {
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
  }
  gpu_pump_thread_tls_ = {};
  if (memory_ && gpu_pump_callback_stack_base_) {
    (void)memory_->release(gpu_pump_callback_stack_base_);
    gpu_pump_callback_stack_base_ = 0;
    gpu_pump_callback_stack_size_ = 0;
  }

  // KNOWN ISSUE: guest worker threads parked in unbounded kernel waits are still
  // alive here, so unloading the extension can take the process down on stop.
  // Terminating them first (ThreadManager::shutdown) currently hangs because such
  // waits are not interruptible by terminate(); that needs its own fix.
  shutdown_step("unloading the native extension");
  unload_native_extension();

  // Shutdown in reverse order of initialization
  xam_.reset();

  input_bridge_.reset();
  if (input_) {
    input_->shutdown();
    input_.reset();
  }

  gpu_.reset();
  register_aperture_.reset();  // references graphics_system_'s register file
  // graphics_system_ holds a reference (not ownership) to memory_ and is only
  // ever touched by the GPU pump thread, already stopped/joined above -
  // reset it alongside gpu_ rather than leaving it dangling until the next
  // init_gpu() overwrites it or the session is destroyed.
  graphics_system_.reset();
  dynamic_fallback_.reset();
  code_cache_.reset();

  // Tear down the guest process/thread model before the memory it lives in.
  // main_thread_ was already joined above; resetting kernel_process_ (and
  // with it its ThreadManager) is then safe/idempotent.
  shutdown_step("terminating and joining guest threads");
  main_thread_.reset();
  if (kernel_io_) kernel_io_->share_handle_table(nullptr);
  kernel_process_.reset();
  kernel_memory_.reset();
  if (memory_) {
    release_guest_thread_tls_context(*memory_, main_thread_tls_);
  }
  main_thread_tls_ = {};

  io_bridge_.reset();
  kernel_io_.reset();
  filesystem_.reset();

  if (memory_) {
    memory_->reset();
    memory_.reset();
  }

  loaded_xex_.reset();
  effective_identity_.reset();
  content_graph_.reset();
  main_cpu_state_.reset();
  export_registry_.clear();

  // Every subsystem pointer above is now reset, so the session object is
  // back in the same state as right after construction (not merely
  // "stopped" - stop() leaves subsystems alive and also lands on Stopped,
  // so Stopped alone cannot mean "torn down"). Landing on Uninitialized
  // here is what makes is_initialized() correctly report false post-
  // shutdown, and lets initialize() be called again on the same object.
  set_state(SessionState::Uninitialized, "Session shut down");
}

}  // namespace xenon::core
