#include "xenon/core/export_registry.hpp"

#include <cstdint>

#include "xenon/kernel/time.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/xbox/xbox_time_convert.hpp"
#include "xbox/exports/user_apcs.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
using xenon::kernel::TimeServices;

// KeQueryPerformanceFrequency (ordinal 0x83)
// Guest ABI: (no parameters) -> r3 = ticks per second.
bool ke_query_performance_frequency(ExportCallContext& context) {
  context.cpu.gpr[3] = TimeServices::performance_frequency();
  return true;
}

// KeQuerySystemTime (ordinal 0x84)
// Guest ABI: r3 = PLARGE_INTEGER out pointer (100ns units since 1601) -> void.
bool ke_query_system_time(ExportCallContext& context) {
  const auto time_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (time_ptr != 0u) {
    context.memory.write64_be(time_ptr, TimeServices::system_time());
  }
  return true;
}

// KeDelayExecutionThread (ordinal 0x5A)
// Guest ABI: r3 = processor mode (ignored - Xenon has no distinct kernel/user
// PPC mode), r4 = alertable (queued user APCs end the delay and run; returns
// STATUS_USER_APC; a thread alert ends it with STATUS_ALERTED), r5 =
// PLARGE_INTEGER interval (100ns units; negative = relative, positive =
// absolute, mandatory per real NT/Xbox semantics) -> r3 = NTSTATUS.
//
// Xenon's 1:1 guest-thread:host-thread model means blocking the calling host
// thread here genuinely suspends only this guest thread - other guest
// threads (each their own host thread) keep running, matching real Xbox 360
// per-thread delay semantics without needing the scheduler-safepoint work
// tracked separately for suspend()/terminate(). A guest thread terminated while
// it sleeps returns at once; its dispatch loop then stops it.
bool ke_delay_execution_thread(ExportCallContext& context) {
  const bool alertable = static_cast<std::uint32_t>(context.cpu.gpr[4]) != 0u;
  const auto interval_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  context.cpu.gpr[3] = 0u;  // STATUS_SUCCESS
  if (interval_ptr != 0u) {
    const auto raw = static_cast<std::int64_t>(context.memory.read64_be(interval_ptr));
    const auto delay = xbox_timeout_to_relative_ms(raw);
    const auto result = kernel::delay_current_thread(delay, alertable);
    if (result == kernel::WaitResult::UserApc) {
      deliver_user_apcs(context);
      context.cpu.gpr[3] = 0x000000C0u;  // STATUS_USER_APC
    } else if (result == kernel::WaitResult::Alerted) {
      context.cpu.gpr[3] = 0x00000101u;  // STATUS_ALERTED
    }
  }
  return true;
}

// KeStallExecutionProcessor (ordinal 0xA8)
// Guest ABI: r3 = microseconds -> void.
//
// Real hardware busy-spins the processor for the requested duration; Xenon
// sleeps for the same duration instead of burning a host CPU core. The
// guest-observable effect - elapsed time before the call returns - is the
// same, and not busy-spinning avoids starving other guest threads that share
// this host's CPU.
bool ke_stall_execution_processor(ExportCallContext& context) {
  const auto microseconds = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  TimeServices::delay_execution(std::chrono::microseconds(microseconds));
  return true;
}

struct TimeExportSpec {
  std::uint32_t ordinal;
  const char* name;
  core::ExportHandler handler;
  bool partial{false};
  const char* partial_note{nullptr};
};

// Ordinals verified against the xenia-project/xenia xboxkrnl export table
// (xboxkrnl_table.inc), not guessed - see xboxkrnl_time_exports.hpp.
const TimeExportSpec kTimeExports[] = {
    {0x083u, "KeQueryPerformanceFrequency", &ke_query_performance_frequency},
    {0x084u, "KeQuerySystemTime", &ke_query_system_time},
    {0x05Au, "KeDelayExecutionThread", &ke_delay_execution_thread, true,
     "an alertable delay runs queued user APCs and ends for a thread alert; "
     "the processor mode is not distinguished (one alert flag per thread)"},
    {0x0A8u, "KeStallExecutionProcessor", &ke_stall_execution_processor},
};

}  // namespace

bool register_xboxkrnl_time_exports(core::ExportRegistry& registry) {
  for (const auto& spec : kTimeExports) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.handler = spec.handler;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.partial = spec.partial;
    if (spec.partial_note) descriptor.partial_note = spec.partial_note;

    if (!registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
