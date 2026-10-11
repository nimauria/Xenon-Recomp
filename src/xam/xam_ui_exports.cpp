#include <cstdint>
#include <functional>

#include "xam/xam_overlapped.hpp"
#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/xam/notification_manager.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {

// The rest of the XamShow* dashboard-overlay family (XamShowSigninUI and
// XamShowMarketplaceUI's ordinals were already declared in xam_exports.hpp
// but never had a registered handler; the others are new). Xenon implements
// no dashboard/shell overlay rendering, so most of these report the real
// "function failed" outcome real hardware gives when the shell UI cannot be
// shown - matching XamShowPartyUI/XamShowCommunitySessionsUI's existing
// precedent in this codebase - rather than leaving guest registers
// untouched. XamShowSigninUI is the one exception: Xenon's UserManager
// always keeps a valid local user already signed in by default (see
// user_manager.hpp), so the real precondition this UI exists to satisfy is
// already met, and the honestly correct answer is success, not failure.
bool register_ui_exports(core::ExportRegistry& registry, core::XenonSession& session,
                         NotificationManager& notifications) {
  bool ok = true;

  // Dialogs Xenon answers without drawing anything, as xenia does when it
  // runs headless (xeXamDispatchHeadless): XN_SYS_UI is broadcast on, the
  // answer is written, XN_SYS_UI is broadcast off, and the call returns its
  // result directly or completes its XOVERLAPPED. No dialog stays open, so
  // XamIsUIActive is false.
  const auto answer_dialog = [&session, &notifications](core::ExportCallContext& ctx,
                                                        cpu::GuestAddress overlapped,
                                                        const std::function<void()>& write) {
    notifications.broadcast(notification::kSystemUi, 1u);
    write();
    notifications.broadcast(notification::kSystemUi, 0u);
    ctx.cpu.gpr[3] = overlapped == 0u
                         ? result::Success
                         : complete_overlapped(ctx, session.kernel_process(), overlapped,
                                               result::Success, 0u, 0u);
  };

  // XamIsUIActive (0x02EC) -> BOOL.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamIsUIActive";
    desc.ordinal = ordinal::XamIsUIActive;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamShowMessageBoxUI (0x02CA): (user_index, title, text, button_count,
  // button_ptrs, active_button, flags, result_ptr, overlapped), the
  // overlapped from the stack. The focused button (active_button) is the
  // answer, as in xenia headless.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamShowMessageBoxUI";
    desc.ordinal = ordinal::XamShowMessageBoxUI;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "no dialog is shown: the focused button is returned as the user's choice";
    desc.handler = [answer_dialog](core::ExportCallContext& ctx) -> bool {
      const auto active_button = static_cast<std::uint32_t>(ctx.cpu.gpr[8]);
      const auto result_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[10]);
      if (result_ptr == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      answer_dialog(ctx, argument_u32(ctx, 8),
                    [&] { ctx.memory.write32_be(result_ptr, active_button); });
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamShowKeyboardUI (0x02C1): (user_index, flags, default_text, title,
  // description, buffer, buffer_length, overlapped). The default text, cut
  // to buffer_length - 1 characters and terminated, is the entered text (an
  // empty buffer without one), as in xenia headless.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamShowKeyboardUI";
    desc.ordinal = ordinal::XamShowKeyboardUI;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "no keyboard is shown: the default text is returned as the entered text";
    desc.handler = [answer_dialog](core::ExportCallContext& ctx) -> bool {
      const auto default_text = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto buffer = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[8]);
      const auto buffer_length = static_cast<std::uint32_t>(ctx.cpu.gpr[9]);
      const auto overlapped = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[10]);
      if (buffer == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      answer_dialog(ctx, overlapped, [&] {
        for (std::uint32_t i = 0; i < buffer_length; ++i) ctx.memory.write16_be(buffer + i * 2u, 0u);
        if (default_text == 0u || buffer_length == 0u) return;
        for (std::uint32_t i = 0; i + 1u < buffer_length; ++i) {
          const auto unit = ctx.memory.read16_be(default_text + i * 2u);
          if (unit == 0u) break;
          ctx.memory.write16_be(buffer + i * 2u, unit);
        }
      });
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamShowSigninUI (0x02BC) - the local offline user is always already
  // signed in (UserManager::initialize()), so there is nothing to prompt;
  // real hardware would also report success immediately in this state.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamShowSigninUI";
    desc.ordinal = ordinal::XamShowSigninUI;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  const auto function_failed_handler = [](core::ExportCallContext& ctx) -> bool {
    ctx.cpu.gpr[3] = result::FunctionFailed;
    return true;
  };

  for (const auto& entry : {
           std::pair{ordinal::XamShowMarketplaceUI, "XamShowMarketplaceUI"},
           std::pair{ordinal::XamShowFriendsUI, "XamShowFriendsUI"},
           std::pair{ordinal::XamShowPlayerReviewUI, "XamShowPlayerReviewUI"},
           std::pair{ordinal::XamShowGamerCardUIForXUID, "XamShowGamerCardUIForXUID"},
           std::pair{ordinal::XamShowMessageBoxUIEx, "XamShowMessageBoxUIEx"},
       }) {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = entry.second;
    desc.ordinal = entry.first;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = function_failed_handler;
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamShowDirtyDiscErrorUI (0x02D9) - void, no return value. Real hardware
  // shows a fatal "cannot read disc" dialog and never returns to the title.
  // Verified against rexglue-sdk's XamShowDirtyDiscErrorUI_entry, which
  // treats this identically: log prominently and terminate - "this is
  // death, and should never return." Mapped onto the same cooperative-stop
  // contract KeBugCheckEx and XamLoaderTerminateTitle already use, since a
  // title reaching this call has already decided its content is unreadable
  // and cannot continue; silently returning would leave it spinning on data
  // it has already given up on.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamShowDirtyDiscErrorUI";
    desc.ordinal = ordinal::XamShowDirtyDiscErrorUI;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session](core::ExportCallContext&) -> bool {
      // set_error() is private to XenonSession - stop() alone (public) is
      // the real, available signal: it flips stop_requested_ and moves the
      // session to Stopping/Stopped exactly like KeBugCheckEx's own fatal
      // path, so the title's execution loop observes the same cooperative
      // stop it would for any other unrecoverable guest condition.
      static_cast<void>(session.stop());
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
