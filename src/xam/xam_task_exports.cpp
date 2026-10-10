#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {

// XamTaskCloseHandle/XamTaskShouldExit (0x01B1/0x01B3) - the rest of the
// XamTask* family; XamTaskSchedule itself (0x01AF) is registered directly by
// XenonSession (see session.cpp's export_xam_task_schedule()) since spawning
// a real guest thread needs session-internal state this file cannot reach.
//
// Verified against rexglue-sdk's xam_task.cpp: XamTaskCloseHandle is a real,
// complete no-op on real hardware too (the task thread this handle names was
// already a self-contained KernelThread the kernel reaps on its own exit;
// there is no separate task-object refcount to release) - just reports
// success. XamTaskShouldExit answers "is the title shutting down" for a
// cooperative task thread to poll; wired to the same
// XenonSession::stop_requested() flag every other guest thread already
// cooperates with, not a fixed answer.
bool register_task_exports(core::ExportRegistry& registry, core::XenonSession& session) {
  bool ok = true;

  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamTaskCloseHandle";
    desc.ordinal = ordinal::XamTaskCloseHandle;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamTaskShouldExit";
    desc.ordinal = ordinal::XamTaskShouldExit;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = session.stop_requested() ? 1u : 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
