#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
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
bool register_ui_exports(core::ExportRegistry& registry, core::XenonSession& session) {
  bool ok = true;

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
