#pragma once

#include "xenon/core/export_registry.hpp"

namespace xenon::xbox {

// Register xboxkrnl.exe IRQL/critical-region/spin-lock exports
// (KeEnterCriticalRegion, KfAcquireSpinLock, etc.). These take no
// kernel::KernelProcess dependency - unlike the dispatcher-object exports in
// xboxkrnl_ke_sync_exports.cpp, spin locks are addressed directly by their
// guest memory location, not through the handle/object system.
[[nodiscard]] bool register_xboxkrnl_ke_irql_exports(core::ExportRegistry& registry);

}  // namespace xenon::xbox
