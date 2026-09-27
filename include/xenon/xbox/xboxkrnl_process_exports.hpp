#pragma once

namespace xenon::core {
class ExportRegistry;
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// KeGetCurrentProcessType (ordinal 0x66) / KeSetCurrentProcessType (ordinal
// 0x9A), verified against xenia-project/xenia's xboxkrnl export table
// (xboxkrnl_table.inc). Both operate on real, mutable
// kernel::KernelProcess::process_type() state (X_PROCTYPE_IDLE=0/
// X_PROCTYPE_USER=1/X_PROCTYPE_SYSTEM=2) - a title that calls Set then Get
// observes its own change, matching xenia's kernel_state()->process_type().
//
// Exposed individually so a caller whose kernel::KernelProcess is not yet
// constructed at export-registration time (XenonSession::init_exports() runs
// before any title is loaded) can register its own lambda that dereferences
// its own KernelProcess pointer at CALL time and forwards here, exactly like
// xboxkrnl_sync_exports.hpp's Nt* handlers (see XenonSession::init_exports()'s
// kSyncBindings).
[[nodiscard]] bool ke_get_current_process_type_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_set_current_process_type_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);

// Convenience registrar for callers (tests/tools) that already have a
// constructed KernelProcess.
[[nodiscard]] bool register_xboxkrnl_process_exports(xenon::core::ExportRegistry& registry,
                                                     xenon::kernel::KernelProcess& process);

}  // namespace xenon::xbox
