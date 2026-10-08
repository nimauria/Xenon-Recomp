#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

XenonSession::GuestDispatchOutcome XenonSession::dispatch_guest_thread(
    cpu::CpuState& state, cpu::GuestAddress entry,
    const std::shared_ptr<kernel::KernelThread>& thread,
    bool trace_dispatches) {
  GuestDispatchOutcome outcome{};

  // Publish this thread's register file for the stall report in stop(). Reads
  // there are deliberately racy and diagnostic-only.
  const auto tracked_thread_id = thread ? thread->thread_id() : 0u;
  if (tracked_thread_id != 0u) {
    std::scoped_lock lock(in_flight_exports_mutex_);
    guest_thread_states_[tracked_thread_id] = &state;
  }
  struct StateRegistration {
    XenonSession* session;
    std::uint32_t thread_id;
    ~StateRegistration() {
      if (thread_id == 0u) return;
      std::scoped_lock lock(session->in_flight_exports_mutex_);
      session->guest_thread_states_.erase(thread_id);
    }
  } state_registration{this, tracked_thread_id};

  cpu::ExecutionContext context(state, *memory_, *this);
  if (dynamic_fallback_) dynamic_fallback_->bind(context);
  if (!config_.adaptive_observation_path.empty() ||
      !config_.adaptive_observation_mirror_path.empty()) {
    context.compiled_lookup_observer = this;
    context.compiled_lookup_miss = &XenonSession::record_compiled_lookup_miss;
  }
  if (compiled_registry_binder_) {
    compiled_registry_binder_(context);
  }

  if (!context.compiled_lookup && !context.dynamic_fallback) {
    outcome.crashed = true;
    outcome.crash_message = "No compiled game code or dynamic fallback is available to execute";
    outcome.crash_exit_code = 0xFFFFFFFFu;
    return outcome;
  }

  auto* entry_fn = context.lookup_compiled(entry, cpu::CompiledLookupKind::Call);
  {
    logging::append_probe_log("dispatch_lookup_diag.log", "dispatch_guest_thread lookup: thread_id=%u entry=0x%08llX entry_fn=%p\n",
                 tracked_thread_id, (unsigned long long)entry, (void*)entry_fn);
  }

  cpu::ExecutionResult result{};
  bool crashed = false;
  std::string crash_message;
  std::uint32_t crash_exit_code = 0xC0000005u;  // NTSTATUS-style default (access violation).
  bool thread_terminated_mid_dispatch = false;
  const auto fail_execution = [&](std::string message, std::uint32_t exit_code) {
    crashed = true;
    crash_message = std::move(message);
    crash_exit_code = exit_code;
  };
  try {
    if (entry_fn) {
      logging::append_probe_log("dispatch_lookup_diag.log", "dispatch_guest_thread: thread_id=%u ABOUT TO CALL entry_fn=%p for entry=0x%08llX\n",
                   tracked_thread_id, (void*)entry_fn, (unsigned long long)entry);
      result = entry_fn(context);
      logging::append_probe_log("dispatch_lookup_diag.log", "dispatch_guest_thread: thread_id=%u entry_fn RETURNED reason=%s next=0x%08llX\n",
                   tracked_thread_id, std::string(cpu::flow_reason_name(result.reason)).c_str(),
                   (unsigned long long)result.next_address);
    } else {
      const auto fallback =
          context.try_dynamic_fallback(entry, cpu::CompiledLookupKind::Call);
      if (!fallback.handled) {
        std::ostringstream address;
        address << std::hex << std::uppercase << entry;
        fail_execution("The compiled registry has no entry for 0x" +
                           address.str() +
                           " and the target is not fallback-executable",
                       0xC000001Du);
      } else {
        result = fallback.result;
      }
    }
    if (!crashed && trace_dispatches && config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Guest entry returned: reason="
                << cpu::flow_reason_name(result.reason)
                << " next=0x" << std::hex << std::uppercase << result.next_address
                << " detail=0x" << result.detail
                << " cia=0x" << state.cia
                << " nia=0x" << state.nia
                << " lr=0x" << state.lr
                << " ctr=0x" << state.ctr
                << " r1=0x" << state.gpr[1]
                << " r3=0x" << state.gpr[3] << std::dec << std::endl;
    }

    constexpr std::uint32_t kMaxTopLevelDispatches = 1'000'000u;
    std::uint32_t dispatch_count = 0u;
    for (;
         !crashed && !stop_requested_.load() &&
         !(thread && thread->is_terminated()) &&
         dispatch_count < kMaxTopLevelDispatches;) {
      // Preemptive safepoint: parks if `thread` was suspend()ed by another
      // host thread (cheap no-op otherwise), then re-checks termination in
      // case terminate() raced with the suspend/resume - a thread must not
      // dispatch one more block after being terminated while parked here.
      if (thread) {
        thread->wait_while_suspended();
        if (thread->is_terminated()) {
          thread_terminated_mid_dispatch = true;
          break;
        }
      }
      if (memory_watch_.active()) {
        WatchObserver observer{};
        observer.phase = WatchPhase::DispatchBoundary;
        observer.thread_id = tracked_thread_id;
        observer.cia = static_cast<cpu::GuestAddress>(state.cia);
        observer.nia = static_cast<cpu::GuestAddress>(state.nia);
        observer.lr = state.lr;
        observer.describe = [this, tracked_thread_id] {
          return describe_recent_exports(tracked_thread_id, 6u);
        };
        sample_memory_watch(observer);
      }
      switch (result.reason) {
        case cpu::FlowReason::Return:
          // A Return reaching this OUTER dispatch loop (as opposed to being
          // absorbed by a nested C++ call's own "if next != expected, return
          // rr" propagation - see the giant per-function switch tables
          // codegen emits) can mean two different things. The top-level
          // guest entry genuinely finishing is the next_address==0/lr==0
          // sentinel pattern (see run_execution()'s caller, which sets the
          // initial synthetic LR to 0 specifically to detect this) - that
          // case is handled below, unchanged. But a nonzero next_address
          // means a tail branch (bctr/bclr with LK=0, not bl) crossed a
          // compiled-function boundary into a shared helper - most commonly
          // a register-restore helper (see runtime_helpers.hpp's
          // restore_gpr_lr_v2 and friends) - whose own Return is really
          // "continue at my caller's real return address", exactly like
          // Branch/Fallthrough below, just arriving as FlowReason::Return
          // because the helper had no idea it was reached via a tail branch
          // from a *different* enclosing function rather than entered as
          // this thread's own top-level function. Treating every such
          // Return as terminal previously made any tail-branch-into-helper
          // pattern that crosses a compiled-function boundary look like a
          // broken/early return, even though the guest call chain was
          // genuinely still live and simply needs to keep dispatching at
          // result.next_address.
          if (result.next_address == 0u && state.lr == 0u) break;
          [[fallthrough]];
        case cpu::FlowReason::Branch:
        case cpu::FlowReason::Fallthrough: {
          if (result.next_address == 0u) {
            fail_execution("Guest execution returned " +
                               std::string(cpu::flow_reason_name(result.reason)) +
                               " with a zero next address",
                           0xC000001Du);
            break;
          }
          auto* next_fn =
              context.lookup_compiled(result.next_address,
                                      cpu::CompiledLookupKind::Branch);
          if (!next_fn) {
            const auto fallback = context.try_dynamic_fallback(
                result.next_address, cpu::CompiledLookupKind::Branch);
            if (!fallback.handled) {
              std::ostringstream diagnostic;
              diagnostic << "Guest " << cpu::flow_reason_name(result.reason)
                         << " target 0x" << std::hex << std::uppercase
                         << result.next_address
                         << " is not compiled or fallback-executable";
              fail_execution(diagnostic.str(), 0xC000001Du);
              break;
            }
            ++dispatch_count;
            result = fallback.result;
          } else {
            ++dispatch_count;
            result = next_fn(context);
          }
          if (trace_dispatches && config_.enable_logging) {
            std::scoped_lock console_log_lock(console_log_mutex());
            std::cout << "[XenonSession] Guest dispatch returned: reason="
                      << cpu::flow_reason_name(result.reason)
                      << " next=0x" << std::hex << std::uppercase
                      << result.next_address << " detail=0x" << result.detail
                      << " cia=0x" << state.cia
                      << " nia=0x" << state.nia
                      << " lr=0x" << state.lr
                      << " ctr=0x" << state.ctr
                      << " r1=0x" << state.gpr[1]
                      << " r3=0x" << state.gpr[3];
            detail::append_title_dispatch_probe(std::cout, result.next_address, state,
                                                context);
            std::cout << std::dec << std::endl;
          }
          continue;
        }
        case cpu::FlowReason::Halt:
          break;
        case cpu::FlowReason::Trap:
          break;
        case cpu::FlowReason::Syscall:
          fail_execution("Unhandled guest syscall escaped RuntimeServices::syscall "
                             "(level " + std::to_string(result.detail) + ")",
                         0xC000001Du);
          break;
        case cpu::FlowReason::LongJump:
          {
            std::ostringstream diagnostic;
            diagnostic << "Guest LongJump escaped the owning compiled function "
                       << "(target 0x" << std::hex << std::uppercase
                       << result.next_address << ')';
            fail_execution(diagnostic.str(), 0xC000001Du);
          }
          break;
      }
      break;
    }
    // Distinguishes "the loop genuinely exhausted its dispatch budget" from
    // "a terminal case (Halt/Trap/a real Return-with-null-sentinel/...)
    // explicitly broke out of the switch" by checking the real counter
    // rather than inferring it from result.reason - Return can now reach
    // this point via either path (see the Return case above), so
    // result.reason alone can no longer tell them apart the way it could
    // when only Branch/Fallthrough ever re-entered the loop.
    if (!crashed && !stop_requested_.load() && !(thread && thread->is_terminated()) &&
        dispatch_count >= kMaxTopLevelDispatches) {
      // A guest that spins without ever leaving compiled/fallback code is almost
      // always retrying a kernel call that keeps failing, so name the last calls.
      fail_execution(detail::describe_dispatch_limit(
                         state, export_trace_, thread ? thread->thread_id() : 0u),
                     0xC000001Du);
    }
    if (!crashed && thread && thread->is_terminated()) {
      thread_terminated_mid_dispatch = true;
    }
  } catch (const memory::MemoryFault& fault) {
    // Real connection to the guest exception path (not a new subsystem):
    // Memory V2 already throws this on a genuine guest memory fault; route
    // it through kernel::ExceptionDispatcher, scoped to whichever thread
    // called dispatch_guest_thread() (the caller already registered itself
    // as current_thread() before invoking this). Also retain the guest
    // CPU/memory state that caused the first fault - "fault at 0" alone
    // cannot distinguish a null data dereference from an indirect call, bad
    // ABI state, or bad mapping.
    crashed = true;
    const auto& info = fault.info();
    const auto record = kernel::ExceptionDispatcher::fault_to_exception(info);
    crash_exit_code = static_cast<std::uint32_t>(record.code);
    // Phase 5 of the AC6 Runtime Readiness pass: scope this dispatch to the
    // actual faulting thread (0 if none, e.g. a test harness invoking
    // dispatch_guest_thread() directly) so a handler registered for one
    // guest thread is never consulted for a different thread's fault.
    static_cast<void>(exception_dispatcher_.dispatch_exception(
        record, thread ? thread->thread_id() : 0u));
    crash_message = detail::describe_guest_memory_fault(
        fault, state, loaded_xex_, *memory_, export_trace_,
        thread ? thread->thread_id() : 0u);
  } catch (const std::exception& ex) {
    crashed = true;
    crash_message = std::string("Unhandled exception during guest execution: ") + ex.what();
  } catch (...) {
    crashed = true;
    crash_message = "Unhandled unknown exception during guest execution";
  }

  outcome.crashed = crashed;
  outcome.crash_message = std::move(crash_message);
  outcome.crash_exit_code = crash_exit_code;
  outcome.final_result = result;
  outcome.stop_requested = stop_requested_.load();
  outcome.thread_terminated = thread_terminated_mid_dispatch;

  // Exception dispatch for a genuine guest Trap is generic fault
  // interpretation (like the MemoryFault catch above), not session-lifecycle
  // bookkeeping, so it happens here rather than in each caller - every
  // guest thread that traps gets this, not just the main thread.
  if (!crashed && result.reason == cpu::FlowReason::Trap) {
    static_cast<void>(exception_dispatcher_.dispatch_exception(
        kernel::ExceptionRecord{kernel::ExceptionCode::IllegalInstruction, 0,
                                static_cast<std::uint32_t>(state.cia), {}},
        thread ? thread->thread_id() : 0u));
  }

  return outcome;
}

bool XenonSession::run_guest_callback(cpu::CpuState& state, cpu::GuestAddress entry,
                                    const std::shared_ptr<kernel::KernelThread>& thread) {
  // Callbacks fire tens of times a second; per-dispatch tracing would drown the log.
  const auto outcome = dispatch_guest_thread(state, entry, thread, /*trace_dispatches=*/false);
  if (outcome.crashed) {
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Guest callback 0x" << std::hex << entry << std::dec
                << " crashed: " << outcome.crash_message << std::endl;
    }
    return false;
  }
  return outcome.final_result.reason != cpu::FlowReason::Trap;
}

}  // namespace xenon::core
