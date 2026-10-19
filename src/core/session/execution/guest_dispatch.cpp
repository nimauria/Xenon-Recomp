#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/logging/diagnostic_events.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

XenonSession::GuestDispatchOutcome XenonSession::dispatch_guest_thread(
    cpu::CpuState& state, cpu::GuestAddress entry,
    const std::shared_ptr<kernel::KernelThread>& thread,
    bool trace_dispatches) {
  GuestDispatchOutcome outcome{};

  // Publish this thread's register file for the stall report in stop(). Reads
  // there are deliberately racy and diagnostic-only. A nested dispatch (a
  // routine the kernel calls back into, see call_guest_routine()) restores
  // the outer registration when it returns.
  const auto tracked_thread_id = thread ? thread->thread_id() : 0u;
  const cpu::CpuState* outer_state = nullptr;
  if (tracked_thread_id != 0u) {
    std::scoped_lock lock(in_flight_exports_mutex_);
    auto& slot = guest_thread_states_[tracked_thread_id];
    outer_state = slot;
    slot = &state;
  }
  struct StateRegistration {
    XenonSession* session;
    std::uint32_t thread_id;
    const cpu::CpuState* outer_state;
    ~StateRegistration() {
      if (thread_id == 0u) return;
      std::scoped_lock lock(session->in_flight_exports_mutex_);
      if (outer_state) {
        session->guest_thread_states_[thread_id] = outer_state;
      } else {
        session->guest_thread_states_.erase(thread_id);
      }
    }
  } state_registration{this, tracked_thread_id, outer_state};

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

    // No dispatch budget: a guest thread runs until it returns, terminates,
    // faults or the session stops, as on hardware. A title's main loop and
    // its worker threads legitimately never return, and returns through
    // shared restore helpers come back through this loop, so any lifetime
    // cap is eventually reached by correct code (AC6 hit a 1,000,000 cap
    // about 30 s in). A thread that spins is diagnosed by the stop report.
    while (!crashed && !stop_requested_.load() && !(thread && thread->is_terminated())) {
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
          if (!next_fn && is_recognized_import_thunk(result.next_address)) {
            // A compiled function tail-branched to an import thunk. Imports
            // are runtime services, never interpreted code, so this must not
            // depend on the Gen 7 fallback being enabled.
            result = cpu::call_import_from_branch(*this, result.next_address, state, *memory_);
            continue;
          }
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
            result = fallback.result;
          } else {
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
                                    const std::shared_ptr<kernel::KernelThread>& thread,
                                    CallbackWatch& watch, const char* kind) {
  const auto record = [&](logging::events::EventKind event_kind, std::uint32_t value) {
    if (!logging::events::enabled()) return;
    logging::events::Event event{};
    event.kind = event_kind;
    event.guest_thread_id = thread ? thread->thread_id() : 0u;
    event.guest_address = static_cast<std::uint32_t>(entry);
    event.value = value;
    event.source = kind;
    logging::events::record(event);
  };
  record(logging::events::EventKind::CallbackBegin, 0u);
  watch.begin(static_cast<std::uint32_t>(entry), kind, CallbackWatch::now_ns());
  // Callbacks fire tens of times a second; per-dispatch tracing would drown the log.
  const auto outcome = dispatch_guest_thread(state, entry, thread, /*trace_dispatches=*/false);
  const auto duration_ns = watch.end(CallbackWatch::now_ns());
  record(logging::events::EventKind::CallbackEnd,
         static_cast<std::uint32_t>(duration_ns / 1'000'000));
  if (outcome.crashed) {
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Guest callback 0x" << std::hex << entry << std::dec
                << " crashed: " << outcome.crash_message << std::endl;
    }
    report_pump_state("pump callback crashed");
    return false;
  }
  return outcome.final_result.reason != cpu::FlowReason::Trap;
}

bool XenonSession::call_guest_routine(cpu::CpuState& caller, cpu::GuestAddress routine,
                                      std::span<const std::uint64_t> arguments) {
  if (!kernel_process_ || routine == 0u) return false;
  auto thread = kernel_process_->thread_manager().current_thread();
  if (!thread) return false;
  // The routine runs on the caller's stack, below everything the caller (an
  // export call in progress) may still use, with the caller's KPCR (r13) and
  // other registers. It returns through lr == 0, which ends the dispatch
  // loop as for any callback.
  constexpr std::uint64_t kStackGap = 0x400u;
  cpu::CpuState state = caller;
  state.gpr[1] = (caller.gpr[1] - kStackGap) & ~std::uint64_t{0xF};
  for (std::size_t i = 0; i < arguments.size() && i < 8u; ++i) state.gpr[3 + i] = arguments[i];
  state.lr = 0u;
  state.cia = routine;
  const auto outcome = dispatch_guest_thread(state, routine, thread, /*trace_dispatches=*/false);
  if (outcome.crashed) {
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Guest routine 0x" << std::hex << routine << std::dec
                << " called from the kernel crashed: " << outcome.crash_message << std::endl;
    }
    return false;
  }
  return outcome.final_result.reason != cpu::FlowReason::Trap;
}

}  // namespace xenon::core
