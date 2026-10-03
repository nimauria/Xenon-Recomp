#pragma once

#include "xenon/core/export_registry.hpp"

namespace xenon::core {
class XenonSession;
}

namespace xenon::xam {

// Register xam.xex system-information exports (XamGetSystemVersion, etc.)
// `session` backs XamAlloc/XamFree (guest virtual allocator),
// XamGetExecutionId (loaded XEX execution-info header), and
// XamLoaderLaunchTitle/XamLoaderTerminateTitle (cooperative title stop) -
// only read at call time, never during this registration call itself.
[[nodiscard]] bool register_system_exports(core::ExportRegistry& registry, core::XenonSession& session);

}  // namespace xenon::xam
