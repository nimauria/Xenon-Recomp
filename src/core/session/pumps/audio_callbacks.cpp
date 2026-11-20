#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>

#include "xenon/core/guest_stack_walk.hpp"
#include "xenon/core/session.hpp"
#include "xenon/logging/activity_trace.hpp"
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
  audio_thread_->set_guest_kpcr_address(audio_thread_tls_.kpcr_address);

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
  if (const char* probe = std::getenv("XENON_AUDIO_STALL_PROBE");
      probe && *probe && std::string_view(probe) != "0" && !audio_stall_probe_running_.load()) {
    audio_stall_probe_running_.store(true);
    audio_stall_probe_thread_ = std::thread(&XenonSession::run_audio_stall_probe, this);
  }
  return true;
}

void XenonSession::stop_audio_stall_probe() {
  audio_stall_probe_running_.store(false);
  if (audio_stall_probe_thread_.joinable()) audio_stall_probe_thread_.join();
}

// Samples every guest thread while an audio render callback overruns: 12 ms
// in (the device holds about 37 ms of queued frames, so the queue is draining
// by then), then every 40 ms until it returns. Each sample lists, per guest
// thread, the kernel call it is blocked in (with its arguments and how long
// it has been there) or, for a thread running guest code, its last recorded
// address and caller chain. Capped so a long run stays readable.
void XenonSession::run_audio_stall_probe() {
  constexpr std::int64_t kFirstSampleNs = 12'000'000;
  constexpr std::int64_t kRepeatNs = 40'000'000;
  constexpr int kMaxSamples = 400;
  int samples = 0;
  std::int64_t sampled_callback_since = -1;
  std::int64_t last_sample_ns = 0;
  while (audio_stall_probe_running_.load() && samples < kMaxSamples) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto watch = audio_callback_watch_.snapshot();
    if (!watch.in_callback) continue;
    const auto now = CallbackWatch::now_ns();
    const auto stalled = now - watch.since_ns;
    if (stalled < kFirstSampleNs) continue;
    if (watch.since_ns == sampled_callback_since && now - last_sample_ns < kRepeatNs) continue;
    sampled_callback_since = watch.since_ns;
    last_sample_ns = now;
    ++samples;

    std::string text = "audio callback 0x" + [&] {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%08X", watch.entry);
      return std::string(buf);
    }() + " running " + std::to_string(stalled / 1'000'000) + " ms\n";
    {
      const auto in_flight = in_flight_exports_.snapshot();
      std::scoped_lock lock(guest_thread_states_mutex_);
      std::set<std::uint32_t> threads;
      for (const auto& [id, state] : guest_thread_states_) threads.insert(id);
      for (const auto& [id, call] : in_flight) threads.insert(id);
      for (const auto id : threads) {
        char line[512];
        std::string name;
        if (kernel_process_) {
          if (const auto* thread = kernel_process_->thread_manager().get_thread_ptr(id))
            name = thread->name();
        }
        if (const auto call = in_flight.find(id); call != in_flight.end()) {
          const auto& c = call->second;
          std::snprintf(line, sizeof(line),
                        "  t%u %s: in %s(%llX, %llX, %llX, %llX) for %lld ms, lr=%08llX\n", id,
                        name.c_str(), c.descriptor ? c.descriptor->name.c_str() : "?",
                        static_cast<unsigned long long>(c.arguments[0]),
                        static_cast<unsigned long long>(c.arguments[1]),
                        static_cast<unsigned long long>(c.arguments[2]),
                        static_cast<unsigned long long>(c.arguments[3]),
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - c.since)
                                .count()),
                        static_cast<unsigned long long>(c.lr));
          text += line;
        } else if (const auto state = guest_thread_states_.find(id);
                   state != guest_thread_states_.end() && state->second) {
          const auto& s = *state->second;
          std::snprintf(line, sizeof(line), "  t%u %s: running cia=%08llX lr=%08llX stack:", id,
                        name.c_str(), static_cast<unsigned long long>(s.cia),
                        static_cast<unsigned long long>(s.lr));
          text += line;
          if (memory_) {
            for (const auto& frame :
                 walk_guest_stack(*memory_, static_cast<std::uint32_t>(s.gpr[1]), 8u)) {
              std::snprintf(line, sizeof(line), " %08X", frame.return_address);
              text += line;
            }
          }
          text += "\n";
        }
      }
    }
    // One record per line: a run-log record holds up to 1 KB, and a sample
    // lists every guest thread.
    for (std::size_t begin = 0; begin < text.size();) {
      const auto end = text.find('\n', begin);
      const auto stop = end == std::string::npos ? text.size() : end + 1;
      logging::append_run_log("audio_stall_probe.log", "%s",
                              text.substr(begin, stop - begin).c_str());
      begin = stop;
    }
  }
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
  static const auto activity_key = logging::intern_activity(
      {"audio", "render callback", {"callback", "argument", "", ""}, false});
  logging::ActivityScope activity(activity_key, {callback, argument, 0, 0});
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
    const bool _ok =
        run_guest_callback(state, callback, audio_thread_, audio_callback_watch_, "audio render");
    if (_en <= 20 || (_en % 500) == 0) {
      logging::append_probe_log("audio_callback_diag.log",
                                "invoke_audio_callback #%d RETURNED: ok=%d\n", _en, (int)_ok);
    }
    return _ok;
  }
}
#endif

}  // namespace xenon::core
