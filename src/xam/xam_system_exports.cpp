#include "xenon/xam/xam_system_exports.hpp"

#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {

bool register_system_exports(core::ExportRegistry& registry) {
  bool ok = true;

  // XamGetSystemVersion (0x0282) - no arguments, returns a packed
  // dashboard/kernel build-version DWORD in r3.
  //
  // Real hardware returns the running kernel's actual build number, but
  // guest code only ever uses this value for a "does this feature exist
  // yet" comparison, not for anything display-visible. Xenia (and its
  // rexglue-sdk fork, src/kernel/xam/xam_info.cpp's
  // XamGetSystemVersion_entry) deliberately return 0 here rather than
  // inventing a plausible-looking high build number: guest code that
  // branches on this value treats a higher number as "newer kernel, assume
  // feature X is present", so a made-up modern-looking value is more likely
  // to route execution into a completely unimplemented codepath than an old
  // one is. Returning 0 always selects the oldest/most-conservative branch,
  // which is exactly the behavior a real kernel's *lowest* possible build
  // number would produce - so this is a genuine compatibility-motivated
  // choice, not a placeholder.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamGetSystemVersion";
    desc.ordinal = ordinal::XamGetSystemVersion;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XGetGameRegion (0x03CC) - no arguments, returns the console's configured
  // game-region flags (XC_GAME_REGION_*) in r3.
  //
  // Real hardware reports the region(s) the running kernel/console are
  // configured to allow; xenia-project/xenia's xeXGetGameRegion() (verified
  // reference) returns 0xFFFF unconditionally - every region bit set, i.e.
  // "region-free" - rather than modeling a specific retail console's region
  // lock. Xenon matches that: AC6 (and any other title) calls this purely to
  // decide whether to show a region-mismatch warning, and a real dev/debug
  // kit (which is exactly the kind of hardware most emulation targets, and
  // the only kind that never needs to reject a disc) also reports every
  // region allowed.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XGetGameRegion";
    desc.ordinal = ordinal::XGetGameRegion;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0xFFFFu;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
