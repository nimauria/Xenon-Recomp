#include "xenon/core/session.hpp"
#include "xenon/cpu/call_trace.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

SessionResult XenonSession::start() {
  if (!loaded_xex_) {
    return SessionResult::failure("No game loaded");
  }

  const auto current = state();
  if (current != SessionState::Ready && current != SessionState::Paused) {
    return SessionResult::failure("Cannot start from current state");
  }
  // A prior run's thread object stays joinable until joined even after the
  // OS thread has finished; state() already gates genuine concurrent runs
  // (it only reaches Ready/Paused again once run_execution() has returned),
  // so this reclaims that thread object rather than signaling "still busy".
  if (main_thread_) {
    static_cast<void>(main_thread_->join());
  }
  if (!native_extension_bound_) {
    return SessionResult::failure(
        "No compiled game code is available to execute: " +
        (native_extension_error_.empty()
             ? std::string("this game's module supplied no native extension")
             : native_extension_error_));
  }
  if (!kernel_process_) {
    return SessionResult::failure("No guest process available (create_guest_process() did not run)");
  }

  stop_requested_.store(false);

  // A kernel::KernelThread cannot restart after terminating (see
  // create_guest_process()'s comment), so each start() gets a fresh thread
  // object bound to the same process/module/TLS state created once at
  // load_game() time.
  kernel::ThreadCreationParams thread_params{};
  thread_params.stack_size = stack_size_;
  thread_params.name = "MainThread";
  main_thread_ = kernel_process_->thread_manager().create_thread(
      [this]() -> std::uint32_t { return run_execution(); }, thread_params);
  if (!main_thread_) {
    set_error("Failed to create main KernelThread");
    return SessionResult::failure(last_error_);
  }
  write_guest_thread_id(*memory_, main_thread_tls_, main_thread_->thread_id());
  main_thread_->set_guest_kthread_address(main_thread_tls_.kthread_address);
  main_thread_->set_guest_kpcr_address(main_thread_tls_.kpcr_address);
  kernel_process_->set_main_thread(main_thread_);
  logging::append_probe_log("thread_identity_diag.log", "main_thread_ assigned thread_id=%u\n",
                            main_thread_->thread_id());
  // Take the watch baseline before any guest code runs.
  start_memory_watch_poll();
  start_guest_profile();
  if (!main_thread_->start()) {
    set_error("Failed to start main KernelThread");
    return SessionResult::failure(last_error_);
  }

  set_state(SessionState::Running, "Starting game execution...");
  return SessionResult::ok("Game started", SessionState::Running);
}

SessionResult XenonSession::pause() {
  if (state() != SessionState::Running) {
    return SessionResult::failure("Cannot pause - not running");
  }

  // There is no interpreter/yield point to suspend already-running native
  // compiled code at, so "pause" only affects state reporting today. A real
  // pause needs a cooperative checkpoint in the generated code (e.g. at
  // frame boundaries), which is native-extension work, not session wiring.
  set_state(SessionState::Paused, "Game paused (execution continues; pause is state-only)");
  return SessionResult::ok("Game paused", SessionState::Paused);
}

SessionResult XenonSession::resume() {
  if (state() != SessionState::Paused) {
    return SessionResult::failure("Cannot resume - not paused");
  }

  set_state(SessionState::Running, "Resuming game...");
  return SessionResult::ok("Game resumed", SessionState::Running);
}

SessionResult XenonSession::stop() {
  const auto current = state();
  if (current != SessionState::Running && current != SessionState::Paused) {
    return SessionResult::failure("Cannot stop - not running or paused");
  }

  set_state(SessionState::Stopping, "Stop requested...");

  if (!execution_active_.load()) {
    // The execution thread never actually got into guest code (e.g. it
    // failed immediately), so it is safe to join synchronously here.
    stop_requested_.store(true);
    if (main_thread_) static_cast<void>(main_thread_->join());
    set_state(SessionState::Stopped, "Game stopped");
    return SessionResult::ok("Game stopped", SessionState::Stopped);
  }

  // A stop that finds the guest still executing usually means it is stalled
  // (blocked in a kernel wait or spinning). Say what each thread last asked the
  // kernel for, so the stall can be diagnosed from the log alone. This runs
  // before stop_requested_ is set: the main thread's dispatch loop exits on
  // that flag and unregisters its registers, and a stall usually involves
  // the main thread.
  report_stop_diagnostics();
  cpu::call_trace::write_report();
  stop_requested_.store(true);

  // Ends the kernel waits and delays guest threads are blocked in, after the
  // report above has recorded them. Already-running native compiled code
  // still cannot be preempted: a thread stops at its next block boundary.
  // The caller (runtime host) owns the actual hard-stop policy: wait for the
  // guest to exit cooperatively, then terminate the process if it does not.
  // Report "Stopping" rather than blocking.
  terminate_guest_threads(/*include_pumps=*/false);
  return SessionResult::ok(
      "Stop requested; waiting for guest execution to end cooperatively",
      SessionState::Stopping);
}

void XenonSession::terminate_guest_threads(bool include_pumps) noexcept {
  if (!kernel_process_) return;
  for (const auto& thread : kernel_process_->thread_manager().enumerate_threads()) {
    if (!include_pumps && (thread == gpu_pump_thread_ || thread == gpu_vsync_thread_ ||
                           thread == audio_thread_))
      continue;
    static_cast<void>(thread->terminate(0));
  }
}

}  // namespace xenon::core
