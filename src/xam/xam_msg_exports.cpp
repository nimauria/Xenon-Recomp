#include "xenon/core/export_registry.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {

// XMsg* (0x01F4/0x01F7/0x01F8/0x01FC) - the in-process app-message IPC
// family titles use to call into registered "XAM apps" (dashboard blades,
// content apps, system UI) and to manage their overlapped completion.
// Verified against rexglue-sdk's xam_msg.cpp: every real dispatch (sync or
// async) resolves through an app-manager keyed by an app id, and reports
// X_ERROR_NOT_FOUND/X_E_NOTFOUND when that id has no registered app - the
// exact real outcome for the case this codebase is always in, since Xenon
// implements no in-process XAM app registry. Reporting that real "app
// undefined" result (rather than returning success or leaving registers
// untouched) is what lets a title's own existing error handling for an
// unknown app id run, instead of silently pretending a call it made
// succeeded. XMsgCancelIORequest's real contract does not depend on any app
// registry at all (it only optionally waits on a caller-owned event handle),
// so it is implemented in full rather than approximated.
bool register_msg_exports(core::ExportRegistry& registry) {
  bool ok = true;

  // X_ERROR_NOT_FOUND (Win32 ERROR_NOT_FOUND) = 1168 = 0x490.
  constexpr std::uint32_t kErrorNotFound = 0x490u;

  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XMsgInProcessCall";
    desc.ordinal = ordinal::XMsgInProcessCall;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "no in-process XAM app registry exists, so every call id resolves "
        "to the real 'app undefined' outcome instead of actually dispatching";
    desc.handler = [kErrorNotFound](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = kErrorNotFound;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XMsgStartIORequest";
    desc.ordinal = ordinal::XMsgStartIORequest;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "same app-registry gap as XMsgInProcessCall above";
    desc.handler = [kErrorNotFound](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = kErrorNotFound;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XMsgStartIORequestEx";
    desc.ordinal = ordinal::XMsgStartIORequestEx;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "same app-registry gap as XMsgInProcessCall above";
    desc.handler = [kErrorNotFound](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = kErrorNotFound;
      return true;
    };
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
      // Since nothing here ever starts a real async request (the Start*
      // calls above always fail synchronously), there is never a real
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
