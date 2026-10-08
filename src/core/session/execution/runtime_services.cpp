#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "xenon/core/session.hpp"
#include "xenon/kernel/time.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

// RuntimeServices implementation

cpu::ExecutionResult XenonSession::call(cpu::GuestAddress target,
                                       cpu::CpuState& state,
                                       cpu::MemoryPort& memory) {
  if (!code_cache_) {
    return {cpu::FlowReason::Trap, state.cia, 0};
  }

  // Look up compiled code
  auto result = code_cache_->execute(target, state, memory, *this);
  if (result) {
    return *result;
  }

  // This is the real "recompiled PPC -> import -> ExportRegistry" boundary.
  // Native XEX function imports have two records: a type-0 address record and
  // a type-1 callable thunk. Only type-1 is a callable import. Standalone
  // type-0 records are imported variables and are bound to guest-backed
  // system variables during load_game().
  if (loaded_xex_) {
    for (const auto& import : loaded_xex_->image.imports) {
      if (!import.callable() || import.guest_thunk != target) continue;
      const bool handled = external_call(import.module, import.ordinal, state, memory);
      if (handled) {
        // Fallthrough (non-terminal) matches normal call/return semantics:
        // execution continues at the instruction after the `bl`.
        return {cpu::FlowReason::Fallthrough, state.cia, 0u};
      }
      // A recognized import call site whose specific export this build does
      // not implement is still a genuine, unrecoverable call failure from
      // the guest program's point of view. Returning a non-terminal result
      // here would be silently swallowed by Op::Call/CallIndirect's
      // `if(rr.terminal()) return rr;` check, letting execution fall through
      // to the next instruction with a stale, unspecified r3 as though the
      // call had quietly succeeded - exactly the "silent ignore"/"return
      // zero and continue" pattern the completion contract forbids. Trap is
      // terminal, so it propagates all the way out to run_execution(),
      // which already turns a terminal Trap into a dispatched guest
      // exception instead of pretending nothing happened.
      if (config_.enable_export_diagnostics) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] Unresolved import call: " << import.module << "!"
                  << (import.symbol.empty() ? std::to_string(import.ordinal) : import.symbol)
                  << " at 0x" << std::hex << target << std::dec << std::endl;
      }
      return {cpu::FlowReason::Trap, target,
              static_cast<std::uint32_t>(kernel::ExceptionCode::ProcedureNotFound)};
    }
  }

  // A thunk minted by XexGetProcedureAddress for a system export: dispatch it
  // through the ExportRegistry exactly like a XEX import thunk.
  if (module_registry_) {
    if (const auto thunk = module_registry_->thunk_target(target)) {
      if (external_call(thunk->library, thunk->ordinal, state, memory)) {
        return {cpu::FlowReason::Fallthrough, state.cia, 0u};
      }
      if (config_.enable_export_diagnostics) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] Unresolved dynamic import call: " << thunk->library << "!"
                  << thunk->ordinal << " at 0x" << std::hex << target << std::dec << std::endl;
      }
      return {cpu::FlowReason::Trap, target,
              static_cast<std::uint32_t>(kernel::ExceptionCode::ProcedureNotFound)};
    }
  }

  // The target is neither a locally compiled function (already checked by
  // Op::Call/CallIndirect's context.lookup_compiled() before ever reaching
  // here - see backend_cpp_aot.cpp) nor a recognized XEX import call site.
  // A `bl`/`bctrl` always expects SOME code to run and produce a real
  // result, so - same reasoning as the unresolved-import case above - this
  // must be a terminal, diagnosable failure rather than a silently
  // swallowed non-terminal Branch.
  if (config_.enable_export_diagnostics) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Unresolved call target: 0x" << std::hex << target
              << " (neither compiled guest code nor a known import)" << std::dec << std::endl;
  }
  return {cpu::FlowReason::Trap, target,
          static_cast<std::uint32_t>(kernel::ExceptionCode::ProcedureNotFound)};
}

bool XenonSession::is_recognized_import_thunk(cpu::GuestAddress target) {
  if (module_registry_ && module_registry_->is_thunk(target)) return true;
  if (!loaded_xex_) return false;
  for (const auto& import : loaded_xex_->image.imports) {
    if (import.callable() && import.guest_thunk == target) return true;
  }
  return false;
}

cpu::ExecutionResult XenonSession::syscall(std::uint32_t level,
                                          cpu::CpuState& state,
                                          cpu::MemoryPort& memory) {
  // Handle Xbox syscalls
  // For now, just return
  return {cpu::FlowReason::Syscall, state.cia + 4, level};
}

cpu::ExecutionResult XenonSession::trap(std::uint32_t trap_code,
                                       cpu::CpuState& state,
                                       cpu::MemoryPort& memory) {
  // Handle traps
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Trap: code=" << trap_code
              << " at 0x" << std::hex << state.cia << std::dec << std::endl;
  }
  return {cpu::FlowReason::Trap, state.cia, trap_code};
}

std::uint64_t XenonSession::read_spr(std::uint32_t spr,
                                    const cpu::CpuState& state) {
  // Reached only for SPRs outside mfspr/mftb's own xer/lr/ctr/vrsave/pvr/
  // time-base fast paths (dynamic_fallback.cpp) - i.e. a real, if rare, PPC
  // SPR this runtime does not model per-register semantics for. 0 is the
  // safest neutral default (matching many real-hardware unimplemented/
  // reserved SPRs' own behavior), but the access itself must not be
  // silent: counted here and surfaced in capability_report()'s "fallback"
  // section, so a title that actually depends on one shows up as a real,
  // diagnosable gap rather than a silently-wrong constant zero.
  unsupported_spr_reads_.fetch_add(1u, std::memory_order_relaxed);
  logging::Logger::instance().log_if_enabled(
      logging::Level::Debug, "cpu", [spr] {
        return "unsupported SPR read: spr=" + std::to_string(spr);
      });
  return 0;
}

void XenonSession::write_spr(std::uint32_t spr, std::uint64_t value,
                            cpu::CpuState& state) {
  // See read_spr() above - same "not silent" reasoning. The write itself
  // still has nowhere real to go (no per-SPR storage/semantics modeled),
  // but it is now an accounted, logged gap instead of a silent no-op.
  unsupported_spr_writes_.fetch_add(1u, std::memory_order_relaxed);
  logging::Logger::instance().log_if_enabled(
      logging::Level::Debug, "cpu", [spr, value] {
        return "unsupported SPR write: spr=" + std::to_string(spr) +
               " value=" + std::to_string(value);
      });
}

std::uint64_t XenonSession::read_time_base(const cpu::CpuState& state) {
  // The real Xbox 360 PPC time-base register runs at a fixed 50 MHz,
  // independent of CPU clock scaling - see TimeServices::
  // kGuestTimeBaseFrequencyHz's doc comment for how this was verified
  // against xenia-project/xenia rather than guessed. mftb/mftbu-reading
  // guest code (frame pacing, physics timestep, animation timing) needs
  // this to track real elapsed time at that exact rate, matching what
  // KeQueryPerformanceFrequency() (TimeServices::performance_frequency())
  // already reports - the two must stay in the same unit.
  return kernel::TimeServices::performance_counter();
}

bool XenonSession::external_call(std::string_view module,
                                 std::uint32_t ordinal,
                                 cpu::CpuState& state,
                                 cpu::MemoryPort& memory) {
  // Resolves to the real calling guest thread's id via
  // kernel::ThreadManager's thread_local current-thread slot, which every
  // guest-executing host thread registers itself into once at the start of
  // its run (run_execution() for the main thread, run_audio_callback_thread()
  // for the audio callback thread, run_created_guest_thread() for
  // ExCreateThread-spawned threads) - external_call() runs synchronously on
  // whichever host thread is currently executing the guest code that issued
  // this call, so this is always the correct thread, not an approximation.
  // Previously hardcoded to 0 regardless of caller, which made
  // ExportCallContext::thread_id meaningless in production for every export
  // that uses it (e.g. NtCreateMutant/NtWaitForSingleObjectEx's real thread
  // identity/ownership semantics) even though it worked correctly in
  // isolated tests that construct ExportCallContext directly.
  std::uint32_t calling_thread_id = 0;
  if (kernel_process_) {
    if (auto current = kernel_process_->thread_manager().current_thread()) {
      calling_thread_id = current->thread_id();
    }
  }
  if (calling_thread_id == 0u) {
    // Diagnostic: a guest-executing host thread with no registered
    // KernelThread breaks every thread_id-keyed export (mutant ownership,
    // waits). Log each such host thread's first few export calls.
    static std::mutex _anon_mutex;
    static std::map<std::thread::id, int> _anon_counts;
    int _count = 0;
    {
      std::scoped_lock _lock(_anon_mutex);
      _count = ++_anon_counts[std::this_thread::get_id()];
    }
    if (_count <= 5) {
      logging::append_probe_log("anonymous_thread_export_diag.log", "export %s ordinal=0x%X with thread_id=0: host_thread=%zu call #%d cia=0x%08X lr=0x%08X r13=0x%08X\n",
                   std::string(module).c_str(), ordinal,
                   std::hash<std::thread::id>{}(std::this_thread::get_id()), _count,
                   static_cast<unsigned>(state.cia), static_cast<unsigned>(state.lr),
                   static_cast<unsigned>(state.gpr[13]));
    }
  }

  // Try the export registry first
  ExportCallContext context{state, memory, state.cia, calling_thread_id, &stop_requested_};
  std::array<std::uint64_t, 8> arguments{};
  std::copy_n(state.gpr.begin() + 3, arguments.size(), arguments.begin());
  const auto call_address = context.call_address;
  const auto call_lr = state.lr;
  const auto call_ctr = state.ctr;
  const auto* descriptor = export_registry_.resolve(module, ordinal);
  const bool track_in_flight = export_trace_.enabled() && calling_thread_id != 0u;
  if (track_in_flight) {
    std::scoped_lock lock(in_flight_exports_mutex_);
    in_flight_exports_[calling_thread_id] = InFlightExport{
        ordinal, descriptor, call_lr, {arguments[0], arguments[1], arguments[2], arguments[3]},
        std::chrono::steady_clock::now()};
  }
  const bool watch_active = memory_watch_.active();
  const auto watch_observer = [&](WatchPhase phase) {
    WatchObserver observer{};
    observer.phase = phase;
    observer.thread_id = calling_thread_id;
    observer.cia = static_cast<cpu::GuestAddress>(call_address);
    observer.nia = static_cast<cpu::GuestAddress>(state.nia);
    observer.lr = call_lr;
    observer.note = std::string(module) + '!' +
                    (descriptor ? descriptor->name : std::to_string(ordinal));
    observer.describe = [this, calling_thread_id] {
      return describe_recent_exports(calling_thread_id, 6u);
    };
    return observer;
  };
  if (watch_active) sample_memory_watch(watch_observer(WatchPhase::BeforeKernelCall));
  auto result = export_registry_.invoke(module, ordinal, context);
  if (watch_active) sample_memory_watch(watch_observer(WatchPhase::AfterKernelCall));
  if (track_in_flight) {
    std::scoped_lock lock(in_flight_exports_mutex_);
    in_flight_exports_.erase(calling_thread_id);
  }
  export_trace_.record(calling_thread_id, module,
                       descriptor ? std::string_view(descriptor->name) : std::string_view{},
                       ordinal, call_address, call_lr, call_ctr, arguments,
                       state.gpr[3], descriptor != nullptr, result.handled, result.success);

  {
    static std::mutex _seen_exports_mutex;
    static std::set<std::string> _seen_exports;
    const std::string _name = descriptor ? std::string(descriptor->name)
                                          : (std::string(module) + "!ordinal_" + std::to_string(ordinal));
    bool _is_new = false;
    {
      std::scoped_lock _lock(_seen_exports_mutex);
      _is_new = _seen_exports.insert(_name).second;
    }
    if (_is_new) {
      logging::append_probe_log("unique_exports_seen_diag.log", "%s\n", _name.c_str());
    }
  }

  if (result.handled) {
    observe_boot_checkpoint_from_export_call(module, ordinal, state);
    return true;
  }

  // Fall back to legacy registry (for backwards compatibility)
  if (legacy_call_registry_.dispatch(module, ordinal, state, memory)) {
    return true;
  }

  // Unknown export
  if (config_.enable_export_diagnostics) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Unknown export: " << module
              << " ordinal " << ordinal
              << " at 0x" << std::hex << state.cia << std::dec << std::endl;
  }

  return false;
}

}  // namespace xenon::core
