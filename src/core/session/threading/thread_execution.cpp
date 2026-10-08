#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "xenon/core/session.hpp"
#include "xenon/logging/probe_log.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace xenon::core {

std::uint32_t XenonSession::run_execution() {
  // Establishes this host thread as the active guest thread for anything
  // that resolves "current thread" via kernel::ThreadManager (thread_local),
  // so kernel/exception context below - and any future kernel export that
  // asks "who am I" - is scoped to the real KernelThread, not inferred.
  if (kernel_process_ && main_thread_) {
    kernel_process_->thread_manager().set_current_thread(main_thread_);
  }
  reach_boot_checkpoint(BootCheckpoint::EntryStarted);

  execution_active_.store(true);
  const auto outcome =
      dispatch_guest_thread(*main_cpu_state_, loaded_xex_->image.entry_point, main_thread_);
  execution_active_.store(false);

  // See run_created_guest_thread()'s matching cleanup: main_thread_ is
  // tracked by ThreadManager the same way a created thread is, and must
  // stop being counted as live once its dispatch has genuinely ended,
  // regardless of which outcome branch below is taken. This runs AS
  // main_thread_'s own entry_() (see start(), which spawns it exactly like
  // a created guest thread), so directly calling
  // remove_thread(main_thread_->thread_id()) here would be the same
  // self-destruction hazard described at run_created_guest_thread()'s
  // reap_finished_threads() call below - it could drop the map's last
  // *other* reference and destroy main_thread_ while thread_main() is
  // still executing on top of it. reap_finished_threads() is safe here
  // because it can never remove the calling thread itself; main_thread_
  // gets cleaned up by the next opportunistic reap elsewhere (another
  // thread finishing, a new create_thread(), a capability_report() call),
  // or unconditionally by ThreadManager::shutdown() at teardown.
  if (kernel_process_) {
    kernel_process_->thread_manager().reap_finished_threads();
  }

  if (outcome.crashed) {
    set_error(outcome.crash_message);
    return outcome.crash_exit_code;
  }
  if (outcome.thread_terminated) {
    // terminate() was called on main_thread_ (possibly mid-dispatch, via the
    // preemptive safepoint) - its own committed exit code is authoritative,
    // not any in-flight register state.
    set_state(SessionState::Stopped, "Game execution ended (thread terminated)");
    return main_thread_ ? main_thread_->exit_code() : 0;
  }
  if (outcome.stop_requested) {
    set_state(SessionState::Stopped, "Game execution ended (stop requested)");
    return 0;
  }
  if (outcome.final_result.reason == cpu::FlowReason::Trap) {
    set_error("Game execution trapped (code " + std::to_string(outcome.final_result.detail) + ")");
    return outcome.final_result.detail;
  }
  set_state(SessionState::Stopped, "Game execution completed");
  return 0;
}

std::uint32_t XenonSession::run_created_guest_thread(
    std::shared_ptr<kernel::KernelThread> thread, cpu::GuestAddress start_address,
    std::uint64_t start_context, memory::GuestAddress stack_base,
    std::uint32_t stack_size, GuestThreadTlsContext tls) {
  {
    static std::atomic<int> _thread_start_diag_count{0};
    const int _n = _thread_start_diag_count.fetch_add(1) + 1;
    if (_n <= 100) {
      {
#ifdef _WIN32
        const unsigned long _os_tid = GetCurrentThreadId();
#else
        const unsigned long _os_tid = 0;
#endif
        logging::append_probe_log("thread_start_diag.log",
                                  "thread start #%d: id=%u start_address=0x%08llX start_context=0x%08llX os_tid=%lu\n",
                                  _n, thread ? thread->thread_id() : 0u,
                                  (unsigned long long)start_address, (unsigned long long)start_context, _os_tid);
      }
    }
  }
  if (kernel_process_ && thread) {
    kernel_process_->thread_manager().set_current_thread(thread);
  }
  {
    std::ostringstream tid_oss;
    tid_oss << std::this_thread::get_id();
    auto readback = kernel_process_ ? kernel_process_->thread_manager().current_thread() : nullptr;
    logging::append_probe_log("set_current_thread_diag.log",
                 "run_created_guest_thread: set thread_id=%u os_thread=%s readback_id=%u "
                 "readback_ptr=%p set_ptr=%p\n",
                 thread ? thread->thread_id() : 0u, tid_oss.str().c_str(),
                 readback ? readback->thread_id() : 0xFFFFFFFFu, (void*)readback.get(),
                 (void*)thread.get());
  }

  cpu::CpuState state{};
  state.cia = start_address;
  // PowerPC stacks grow downward; r1 starts at the top of the allocation,
  // less a small back-chain reserve, matching create_guest_process()'s main
  // thread stack setup and start_audio_guest_thread()'s callback stack.
  state.gpr[1] = static_cast<std::uint64_t>(stack_base) + stack_size - 64u;
  state.gpr[3] = start_context;
  state.gpr[13] = tls.kpcr_address;

  const auto outcome = dispatch_guest_thread(state, start_address, thread);
  {
    logging::append_probe_log("dispatch_outcome_diag.log",
                 "dispatch_guest_thread returned: thread_id=%u start_address=0x%08llX "
                 "crashed=%d crash_message=\"%s\" thread_terminated=%d stop_requested=%d "
                 "final_reason=%s final_next=0x%08llX final_detail=0x%X\n",
                 thread ? thread->thread_id() : 0u, (unsigned long long)start_address,
                 outcome.crashed ? 1 : 0, outcome.crash_message.c_str(),
                 outcome.thread_terminated ? 1 : 0, outcome.stop_requested ? 1 : 0,
                 std::string(cpu::flow_reason_name(outcome.final_result.reason)).c_str(),
                 (unsigned long long)outcome.final_result.next_address,
                 outcome.final_result.detail);
  }

  std::uint32_t exit_code = 0;
  if (outcome.crashed) {
    exit_code = outcome.crash_exit_code;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Created guest thread "
                << (thread ? thread->thread_id() : 0u)
                << " crashed: " << outcome.crash_message << std::endl;
    }
  } else if (outcome.thread_terminated) {
    // terminate() was called on this thread (possibly mid-dispatch, via the
    // preemptive safepoint) - its own committed exit code is authoritative,
    // not any in-flight register state.
    exit_code = thread ? thread->exit_code() : 0;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Created guest thread "
                << (thread ? thread->thread_id() : 0u)
                << " terminated mid-dispatch, exit_code=" << exit_code << std::endl;
    }
  } else if (outcome.stop_requested) {
    // Session-wide stop was requested mid-dispatch; exit_code stays 0,
    // matching run_execution()'s own "stop requested" -> clean 0 convention.
  } else if (outcome.final_result.reason == cpu::FlowReason::Trap) {
    // A Trap is a distinct, non-"crashed" outcome in GuestDispatchOutcome
    // (see dispatch_guest_thread()) - it still must not be reported as a
    // successful exit code, exactly matching how run_execution() treats a
    // main-thread Trap (returns final_result.detail, not gpr[3]).
    exit_code = outcome.final_result.detail;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Created guest thread "
                << (thread ? thread->thread_id() : 0u)
                << " trapped (code " << exit_code << ')' << std::endl;
    }
  } else {
    // Natural return: PPC ABI convention - r3 holds the function's return
    // value, matching real ExCreateThread/ExTerminateThread semantics (a
    // thread function returning is an implicit ExTerminateThread(returnValue)).
    exit_code = static_cast<std::uint32_t>(state.gpr[3]);
  }

  // Release this thread's own guest stack/TLS now that nothing will execute
  // on them again. Safe to do from within the thread's own host-side
  // ThreadEntry body: this code runs on host_thread_'s native OS stack,
  // entirely separate from the guest-memory stack/TLS being freed here.
  if (memory_) {
    release_guest_thread_tls_context(*memory_, tls);
    static_cast<void>(memory_->release(stack_base));
  }

  // This thread id will never execute guest code again (KernelThread cannot
  // restart once terminated, and ids are never reused - see thread.cpp's
  // monotonic g_next_thread_id). A per-thread exception handler chain
  // registered for it must not linger forever in exception_dispatcher_,
  // and must never be silently inherited by a different, later thread.
  exception_dispatcher_.clear_thread_handlers(thread ? thread->thread_id() : 0u);

  // NOT calling ThreadManager::remove_thread(thread->thread_id()) here,
  // deliberately - a real, confirmed use-after-free bug used to live at
  // this exact spot. This function runs AS entry_(), called by
  // KernelThread::thread_main() (see thread.cpp) as `result = entry_();`;
  // thread_main() then keeps running AFTER entry_() returns, touching
  // mutex_/state_/completion_promise_ on `this`. This function's own
  // `thread` parameter is a BY-VALUE std::shared_ptr<KernelThread> copy -
  // the guest HandleTable's reference is the only other one once removed
  // from the map (a game may already have closed the handle before the
  // thread naturally finishes). Removing the map's reference here could
  // therefore drop the very last reference and run ~KernelThread()
  // synchronously, right before `thread` (this function's own copy) is
  // destroyed as this function returns - destroying the KernelThread
  // object while thread_main() is still executing ON it, one call frame
  // up. thread_main()'s subsequent member accesses then hit freed memory:
  // this was reachable in practice (any short-lived guest thread whose
  // handle closes before it returns - not merely a hypothetical "handle
  // closed early" edge case) and manifested as intermittent heap
  // corruption/access violations/hangs depending on heap layout - never a
  // clean, reliable crash, which is why this took so long to isolate.
  //
  // ThreadManager::reap_finished_threads() is the fix: it can never remove
  // the calling thread itself (see KernelThread::is_current_host_thread()),
  // only OTHER threads that have already finished, so it is safe to call
  // from right here, on this thread's own stack, one statement before this
  // function - and thread_main() above it - returns.
  if (kernel_process_) {
    kernel_process_->thread_manager().reap_finished_threads();
  }

  return exit_code;
}

}  // namespace xenon::core
