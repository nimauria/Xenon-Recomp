#include <atomic>

#include "xenon/core/session.hpp"
#include "xenon/logging/probe_log.hpp"

#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

#if defined(XENON_HAS_AUDIO)
bool XenonSession::start_audio_guest_thread() {
  if (!audio_ || !kernel_process_ || !memory_ || !loaded_xex_) return false;
  if (!audio_callback_stack_base_ || !audio_callback_stack_size_) {
    set_error("Audio callback stack was not allocated");
    return false;
  }

  // A dedicated KPCR + static-TLS block for the audio callback thread. Real
  // Xbox 360 render-driver callbacks execute on their own kernel thread with
  // their own TLS instance, never the game's main thread's - reusing the same
  // image.tls template that seeded main_thread_tls_ but allocating an
  // independent block guarantees the two threads observe distinct TLS state
  // (see tests/core/session_tests.cpp's TLS-independence test).
  std::string tls_error;
  if (!setup_guest_thread_tls_context(*memory_, loaded_xex_->image.tls,
                                      audio_callback_stack_base_,
                                      audio_callback_stack_size_,
                                      audio_thread_tls_, &tls_error)) {
    set_error("Failed to set up audio callback thread TLS: " + tls_error);
    return false;
  }

  kernel::ThreadCreationParams thread_params{};
  thread_params.stack_size = audio_callback_stack_size_;
  thread_params.name = "AudioCallbackThread";
  audio_thread_ = kernel_process_->thread_manager().create_thread(
      [this]() -> std::uint32_t { return run_audio_callback_thread(); },
      thread_params);
  if (!audio_thread_) {
    set_error("Failed to create audio callback KernelThread");
    release_guest_thread_tls_context(*memory_, audio_thread_tls_);
    audio_thread_tls_ = {};
    return false;
  }
  write_guest_thread_id(*memory_, audio_thread_tls_, audio_thread_->thread_id());
  audio_thread_->set_guest_kthread_address(audio_thread_tls_.kthread_address);

  // Mark the pump active and wire the invoker before starting the thread, so
  // there is no window where the thread is running but has nothing to do (or
  // races begin_guest_callback_pump() against the thread's own startup).
  audio_->begin_guest_callback_pump();
  audio_->set_guest_callback_invoker(
      [this](cpu::GuestAddress callback, cpu::GuestAddress argument) {
        return invoke_audio_callback(callback, argument);
      });

  if (!audio_thread_->start()) {
    set_error("Failed to start audio callback KernelThread");
    audio_->stop_guest_callback_pump();
    audio_thread_.reset();
    release_guest_thread_tls_context(*memory_, audio_thread_tls_);
    audio_thread_tls_ = {};
    return false;
  }
  return true;
}

std::uint32_t XenonSession::run_audio_callback_thread() {
  // Registers this host thread as audio_thread_ for anything that resolves
  // "current thread" via kernel::ThreadManager (thread_local) - the same
  // mechanism run_execution() uses for main_thread_ - so a guest xboxkrnl
  // export invoked from inside a guest audio callback sees the audio
  // callback's own thread identity, not the main thread's or none at all.
  if (kernel_process_ && audio_thread_) {
    kernel_process_->thread_manager().set_current_thread(audio_thread_);
  }
  if (audio_) {
    audio_->run_guest_callback_pump_body();
  }
  return 0;
}

bool XenonSession::invoke_audio_callback(cpu::GuestAddress callback,
                                         cpu::GuestAddress argument) {
  if (!callback || !memory_ || !compiled_registry_binder_ || !kernel_process_ ||
      !audio_callback_stack_base_ || !audio_callback_stack_size_) {
    return false;
  }

  cpu::CpuState state{};
  state.cia = callback;
  state.gpr[1] = static_cast<std::uint64_t>(audio_callback_stack_base_) +
                 audio_callback_stack_size_ - 64u;
  state.gpr[3] = argument;
  // The real fix this pass makes: gpr[13] points at this thread's OWN KPCR
  // (allocated once in start_audio_guest_thread(), reused across every call -
  // TLS is per-thread state that outlives a single callback invocation, not
  // per-call scratch), instead of an isolated bare CpuState with no thread
  // identity at all. Compiled guest code that accesses __declspec(thread)
  // TLS or calls a real xboxkrnl export now does so through the same
  // r13-relative KPCR mechanism the main thread uses (see
  // guest_thread_context.hpp) and against the audio callback's own
  // ThreadManager::current_thread(), set once in run_audio_callback_thread().
  state.gpr[13] = audio_thread_tls_.kpcr_address;

  {
    static std::atomic<int> _n{0};
    const int _en = _n.fetch_add(1) + 1;
    if (_en <= 20 || (_en % 500) == 0) {
      logging::append_probe_log("audio_callback_diag.log", "invoke_audio_callback #%d ENTER: callback=0x%08X argument=0x%08X\n",
                   _en, (unsigned)callback, (unsigned)argument);
    }
    const bool _ok = run_guest_callback(state, callback, audio_thread_);
    if (_en <= 20 || (_en % 500) == 0) {
      logging::append_probe_log("audio_callback_diag.log",
                                "invoke_audio_callback #%d RETURNED: ok=%d\n", _en, (int)_ok);
    }
    return _ok;
  }
}
#endif

}  // namespace xenon::core
