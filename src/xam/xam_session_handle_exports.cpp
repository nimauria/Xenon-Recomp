#include "xenon/core/export_registry.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {

// XamSessionCreateHandle/XamSessionRefObjByHandle (0x0316/0x0317) - Xbox
// Live multiplayer session object handles. Xenon implements no Live session
// subsystem (no real XSESSION object exists anywhere in this codebase to
// back a real handle with). Verified against rexglue-sdk's xam_user.cpp,
// which - like every other available reference - returns a fixed sentinel
// handle/object pair with the identical "TODO: implement this properly, for
// now this prevents crashing" rationale: a title that creates this handle
// purely to have a non-null token to pass to other (also real-session-less)
// Live calls gets a stable, recognizable value rather than a crash, while a
// title that actually tries to use it as a real session object handle has
// nothing real to dereference regardless of what value this returns.
bool register_session_handle_exports(core::ExportRegistry& registry) {
  bool ok = true;

  constexpr std::uint32_t kSentinelHandle = 0xCAFEDEADu;
  constexpr std::uint32_t kSentinelObject = 0xDEADF00Du;

  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamSessionCreateHandle";
    desc.ordinal = ordinal::XamSessionCreateHandle;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "returns a fixed sentinel handle - no real XSESSION object backs "
        "it, matching xenia/rexglue-sdk's own identical precedent";
    desc.handler = [kSentinelHandle](core::ExportCallContext& ctx) -> bool {
      const auto handle_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
      if (handle_ptr != 0u) ctx.memory.write32_be(handle_ptr, kSentinelHandle);
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamSessionRefObjByHandle";
    desc.ordinal = ordinal::XamSessionRefObjByHandle;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "returns a fixed sentinel object pointer for the matching sentinel "
        "handle; any other handle is reported invalid rather than also "
        "returning the sentinel";
    desc.handler = [kSentinelHandle, kSentinelObject](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto obj_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      if (handle != kSentinelHandle) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      if (obj_ptr != 0u) ctx.memory.write32_be(obj_ptr, kSentinelObject);
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
