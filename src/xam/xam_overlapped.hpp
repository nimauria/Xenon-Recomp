#pragma once

// Private to the XAM exports: XOVERLAPPED completion.
//
// Many XAM calls take an optional XOVERLAPPED. When the caller passes one,
// the call returns ERROR_IO_PENDING and reports its real result through the
// overlapped: Result and Length are filled in, its event is signalled, and
// its completion routine is queued as a user APC to the calling thread,
// CompletionRoutine(Result, Length, Overlapped). The title then waits on the
// event, polls XamGetOverlappedResult, or waits alertably for the routine.
// xenia and rexglue-sdk complete most calls this way at once
// (CompleteOverlappedImmediate), and Xenon completes every call at once.

#include <cstdint>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"

namespace xenon::xam {

// XOVERLAPPED (28 bytes) as xenia and rexglue-sdk lay it out.
struct XOverlappedLayout {
  static constexpr std::uint32_t kResult = 0x00;
  static constexpr std::uint32_t kLength = 0x04;
  static constexpr std::uint32_t kContext = 0x08;
  static constexpr std::uint32_t kEvent = 0x0C;
  static constexpr std::uint32_t kCompletionRoutine = 0x10;
  static constexpr std::uint32_t kCompletionContext = 0x14;
  static constexpr std::uint32_t kExtendedError = 0x18;
};

constexpr std::uint32_t kErrorIoIncomplete = 0x000003E4u;
constexpr std::uint32_t kErrorIoPending = 0x000003E5u;

// Argument `index` of the call: r3 onwards for 0 to 7, then the caller's
// parameter save area, [r1 + 0x54 + (index - 8) * 8], as for every guest
// export (docs/xbox/IMPORT_DISPATCH.md).
inline std::uint32_t argument_u32(core::ExportCallContext& context, std::size_t index) {
  if (index < 8u) return static_cast<std::uint32_t>(context.cpu.gpr[3 + index]);
  const auto stack_pointer = static_cast<std::uint32_t>(context.cpu.gpr[1]);
  return context.memory.read32_be(stack_pointer + 0x54u +
                                  static_cast<std::uint32_t>((index - 8u) * 8u));
}

// Completes `overlapped` at once with `result`, `extended_error` and
// `length`, signals its event and queues its completion routine. Returns
// ERROR_IO_PENDING, what the call returns to the guest.
inline std::uint32_t complete_overlapped(core::ExportCallContext& context,
                                         kernel::KernelProcess* process,
                                         cpu::GuestAddress overlapped, std::uint32_t result,
                                         std::uint32_t extended_error, std::uint32_t length) {
  auto& memory = context.memory;
  memory.write32_be(overlapped + XOverlappedLayout::kResult, result);
  memory.write32_be(overlapped + XOverlappedLayout::kExtendedError, extended_error);
  memory.write32_be(overlapped + XOverlappedLayout::kLength, length);
  const auto event_handle = memory.read32_be(overlapped + XOverlappedLayout::kEvent);
  if (event_handle != 0u && process) {
    kernel::HandleView view{};
    if (process->handle_table().lookup(event_handle, view) == kernel::KernelIoCode::Success &&
        view.object && view.object->type() == kernel::ObjectType::Event) {
      static_cast<kernel::KernelEvent&>(*view.object).set();
    }
  }
  const auto routine = memory.read32_be(overlapped + XOverlappedLayout::kCompletionRoutine);
  if (routine != 0u) {
    if (auto* thread = kernel::KernelThread::current()) {
      thread->queue_user_apc({routine, result, length, overlapped});
    }
  }
  return kErrorIoPending;
}

// The overlapped Result, ExtendedError and Length for a call that returned
// `result` synchronously. As in xenia's and rexglue-sdk's
// CompleteOverlappedImmediate: Length 0 on success, 0xFFFFFFFF otherwise
// (titles have been seen to test it).
struct OverlappedOutcome {
  std::uint32_t result;
  std::uint32_t extended_error;
  std::uint32_t length;
};
inline OverlappedOutcome immediate_outcome(std::uint32_t result) {
  return {result, result, result == 0u ? 0u : 0xFFFFFFFFu};
}

// Wraps `handler`, whose XOVERLAPPED is argument `overlapped_index`: with no
// overlapped it behaves as before; with one, the handler's r3 becomes the
// overlapped's outcome (through `outcome`) and the call returns
// ERROR_IO_PENDING.
inline core::ExportHandler with_overlapped(
    core::XenonSession& session, std::size_t overlapped_index, core::ExportHandler handler,
    OverlappedOutcome (*outcome)(std::uint32_t) = &immediate_outcome) {
  return [&session, overlapped_index, handler = std::move(handler),
          outcome](core::ExportCallContext& context) -> bool {
    const auto overlapped = argument_u32(context, overlapped_index);
    if (!handler(context)) return false;
    if (overlapped == 0u) return true;
    const auto o = outcome(static_cast<std::uint32_t>(context.cpu.gpr[3]));
    context.cpu.gpr[3] = complete_overlapped(context, session.kernel_process(), overlapped,
                                             o.result, o.extended_error, o.length);
    return true;
  };
}

}  // namespace xenon::xam
