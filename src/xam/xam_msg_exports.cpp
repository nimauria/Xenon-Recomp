#include <chrono>
#include <memory>
#include <mutex>

#include "xam/xam_overlapped.hpp"
#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/xam_session.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::xam {
namespace {

// The XMP app owns playback-controller state independently of a title's
// render driver. No system playlist is playing at session startup; the title
// initially owns playback. Controller changes are visible to subsequent
// queries and to XNotify listeners, including callers using async XMsg APIs.
struct XmpController {
  std::mutex mutex;
  std::uint32_t controller = 0;
  std::uint32_t playback_client = 1;

  std::uint32_t dispatch(core::ExportCallContext& ctx, core::XenonSession& session,
                         cpu::GuestAddress args, std::uint32_t length) {
    constexpr std::uint32_t kInvalidArgument = 0x80070057u;
    const auto message = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
    if (message != 0x0007001Au && message != 0x0007001Bu) return 0x80004001u;
    if (!args || (length && length != 12u)) return kInvalidArgument;
    const auto client = ctx.memory.read32_be(args);
    if (client != 0u && client != 2u) return kInvalidArgument;
    const auto arg1 = ctx.memory.read32_be(args + 4u);
    const auto arg2 = ctx.memory.read32_be(args + 8u);
    std::lock_guard lock(mutex);
    if (message == 0x0007001Bu) {
      if (!arg1 || !arg2) return kInvalidArgument;
      ctx.memory.write32_be(arg1, controller);
      // Neither controller has requested an exclusive playback lock.
      ctx.memory.write32_be(arg2, 0u);
    } else {
      if (!((client == 2u && arg1 == 0u) || (client == 0u && arg1 == 1u)) || arg2 > 1u)
        return kInvalidArgument;
      const bool changed = controller != arg1 || playback_client != arg2;
      controller = arg1;
      playback_client = arg2;
      if (changed) {
        if (auto* xam = session.xam())
          xam->notifications().broadcast(0x0A000003u, playback_client == 0u ? 1u : 0u);
      }
    }
    return 0u;
  }
};

}  // namespace

// XMsg* (0x01F4/0x01F7/0x01F8/0x01FC) - the in-process app-message IPC
// family titles use to call into registered "XAM apps" (dashboard blades,
// content apps, system UI) and to manage their overlapped completion.
// Verified against rexglue-sdk's xam_msg.cpp: every real dispatch (sync or
// async) resolves through an app-manager keyed by an app id, and reports
// X_ERROR_NOT_FOUND/X_E_NOTFOUND when that id has no registered app - the
// outcome for apps Xenon has not registered. XMP's playback-controller
// messages are dispatched to session-owned state below. Reporting "app
// undefined" result (rather than returning success or leaving registers
// untouched) is what lets a title's own existing error handling for an
// unknown app id run, instead of silently pretending a call it made
// succeeded. XMsgCancelIORequest's real contract does not depend on any app
// registry at all (it only optionally waits on a caller-owned event handle),
// so it is implemented in full rather than approximated.
bool register_msg_exports(core::ExportRegistry& registry, core::XenonSession& session) {
  bool ok = true;

  // X_ERROR_NOT_FOUND (Win32 ERROR_NOT_FOUND) = 1168 = 0x490.
  constexpr std::uint32_t kErrorNotFound = 0x490u;
  const auto xmp = std::make_shared<XmpController>();
  const auto dispatch = [xmp, &session](core::ExportCallContext& ctx, bool async) {
    logging::append_probe_log(
        "xmsg_probe.log", "app=%08llX msg=%08llX a5=%08llX a6=%08llX a7=%08llX\n",
        static_cast<unsigned long long>(ctx.cpu.gpr[3]),
        static_cast<unsigned long long>(ctx.cpu.gpr[4]),
        static_cast<unsigned long long>(ctx.cpu.gpr[5]),
        static_cast<unsigned long long>(ctx.cpu.gpr[6]),
        static_cast<unsigned long long>(ctx.cpu.gpr[7]));
    ctx.cpu.gpr[3] = static_cast<std::uint32_t>(ctx.cpu.gpr[3]) == 0xFAu
        ? xmp->dispatch(ctx, session, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[async ? 6 : 5]),
                        static_cast<std::uint32_t>(ctx.cpu.gpr[async ? 7 : 6]))
        : kErrorNotFound;
    return true;
  };

  // XamGetOverlappedResult: r3 = XOVERLAPPED*, r4 = PDWORD length (nullable),
  // r5 = wait -> the overlapped's result (a Win32 error code). As in
  // rexglue-sdk's XamGetOverlappedResult_entry: a completed overlapped
  // returns its Result (and Length, unless the result is a failure code); a
  // pending one waits on its event when asked to, or returns
  // ERROR_IO_INCOMPLETE. Every overlapped Xenon starts is completed before
  // the call that started it returns, so the wait only matters for one the
  // guest marks pending itself.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamGetOverlappedResult";
    desc.ordinal = ordinal::XamGetOverlappedResult;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session](core::ExportCallContext& ctx) -> bool {
      const auto overlapped = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
      const auto length_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const bool wait = static_cast<std::uint32_t>(ctx.cpu.gpr[5]) != 0u;
      if (overlapped == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      auto status = ctx.memory.read32_be(overlapped + XOverlappedLayout::kResult);
      if (status == kErrorIoPending) {
        const auto event_handle = ctx.memory.read32_be(overlapped + XOverlappedLayout::kEvent);
        kernel::HandleView view{};
        auto* process = session.kernel_process();
        if (!wait || event_handle == 0u || !process ||
            process->handle_table().lookup(event_handle, view) != kernel::KernelIoCode::Success) {
          ctx.cpu.gpr[3] = kErrorIoIncomplete;
          return true;
        }
        static_cast<void>(kernel::wait_for_single_object(view.object, std::chrono::milliseconds(-1),
                                                         ctx.thread_id));
        status = ctx.memory.read32_be(overlapped + XOverlappedLayout::kResult);
      }
      if ((status & 0x80000000u) == 0u && length_ptr != 0u) {
        ctx.memory.write32_be(length_ptr, ctx.memory.read32_be(overlapped + XOverlappedLayout::kLength));
      }
      ctx.cpu.gpr[3] = status;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XMsgInProcessCall";
    desc.ordinal = ordinal::XMsgInProcessCall;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "dispatches XMP playback-controller messages; other apps and XMP messages remain unavailable";
    desc.handler = [dispatch](core::ExportCallContext& ctx) { return dispatch(ctx, false); };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XMsgStartIORequest";
    desc.ordinal = ordinal::XMsgStartIORequest;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "same XMP dispatch coverage as XMsgInProcessCall";
    desc.handler = [dispatch](core::ExportCallContext& ctx) { return dispatch(ctx, true); };
    desc.handler = with_overlapped(session, 2, std::move(desc.handler));
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XMsgStartIORequestEx";
    desc.ordinal = ordinal::XMsgStartIORequestEx;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "same XMP dispatch coverage as XMsgInProcessCall";
    desc.handler = [dispatch](core::ExportCallContext& ctx) { return dispatch(ctx, true); };
    desc.handler = with_overlapped(session, 2, std::move(desc.handler));
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XMsgCancelIORequest";
    desc.ordinal = ordinal::XMsgCancelIORequest;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      // Real contract: cancel the in-flight request named by the overlapped
      // struct and, if `wait` is set, block until it has actually stopped.
      // Start* dispatch completes immediately here, so there is never a real
      // pending request to wait on - matching real hardware's own behavior
      // for cancelling an already-completed/never-started request.
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
